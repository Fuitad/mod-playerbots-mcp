"""End to end client tests against a fake framed loopback endpoint.

The fake endpoint is owned by the test process. Nothing here contacts a worldserver.
"""

from __future__ import annotations

import itertools
import json
import os
import socket
import subprocess
import sys
import threading
import time
from collections.abc import Callable
from types import TracebackType
from typing import Any, cast

import anyio
import pytest
from mcp import Client, ClientSession, StdioServerParameters, stdio_client
from mcp.server import MCPServer
from pydantic import SecretStr

from conftest import TOKEN, envelope, error_envelope, inspection_payload
from playerbot_mcp import server as server_module
from playerbot_mcp.client import (
    ProtocolMismatchError,
    ServerError,
    VerificationClient,
    VerificationSettings,
    VerificationTimeoutError,
)
from playerbot_mcp.protocol import (
    MAX_FRAME_PAYLOAD_BYTES,
    UINT32_MAX,
    ActionCheck,
    ConfigurationError,
    ErrorCode,
    VerificationConnectionError,
    VerificationError,
    VerificationTimeoutStage,
)
from playerbot_mcp.server import build_server

pytestmark = pytest.mark.integration


class RawFrame:
    """Bytes the fake endpoint writes verbatim, so a test can forge a bad frame header."""

    def __init__(self, payload: bytes) -> None:
        self.payload = payload


class DripFrame:
    """A well formed frame written one byte at a time, each gap inside the response timeout.

    This is the shape a per recv timeout can never catch: every individual read completes in
    time while the exchange as a whole runs far past the caller's deadline.
    """

    def __init__(self, payload: bytes, gap_seconds: float) -> None:
        self.payload = payload
        self.gap_seconds = gap_seconds


Handler = Callable[[dict[str, Any]], bytes | RawFrame | DripFrame | None]


class FakeVerificationServer:
    """Serves one framed request per connection using a caller supplied handler."""

    def __init__(self, handler: Handler) -> None:
        self._handler = handler
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._socket.bind(("127.0.0.1", 0))
        self._socket.listen(8)
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self.requests: list[dict[str, Any]] = []

    @property
    def port(self) -> int:
        return int(self._socket.getsockname()[1])

    def __enter__(self) -> FakeVerificationServer:
        self._thread.start()
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        traceback: TracebackType | None,
    ) -> None:
        self._stop.set()
        self._socket.close()
        self._thread.join(timeout=5)

    def _serve(self) -> None:
        while not self._stop.is_set():
            try:
                connection, _ = self._socket.accept()
            except OSError:
                return
            threading.Thread(target=self._handle, args=(connection,), daemon=True).start()

    def _handle(self, connection: socket.socket) -> None:
        with connection:
            try:
                header = self._read_exactly(connection, 4)
                body = self._read_exactly(connection, int.from_bytes(header, "big"))
            except (OSError, ConnectionError):
                return

            request = json.loads(body)
            self.requests.append(request)
            response = self._handler(request)
            if response is None:
                self._stop.wait(timeout=5)
                return
            if isinstance(response, RawFrame):
                connection.sendall(response.payload)
                return
            if isinstance(response, DripFrame):
                frame = len(response.payload).to_bytes(4, "big") + response.payload
                for index in range(len(frame)):
                    try:
                        connection.sendall(frame[index : index + 1])
                    except (BrokenPipeError, OSError):
                        # The client gave up at its deadline. That is the behaviour under test.
                        return
                    self._stop.wait(timeout=response.gap_seconds)
                return
            connection.sendall(len(response).to_bytes(4, "big") + response)

    @staticmethod
    def _read_exactly(connection: socket.socket, count: int) -> bytes:
        chunks: list[bytes] = []
        remaining = count
        while remaining:
            chunk = connection.recv(remaining)
            if not chunk:
                raise ConnectionError("The peer closed the connection early.")
            chunks.append(chunk)
            remaining -= len(chunk)
        return b"".join(chunks)


def make_client(port: int, *, response_timeout: float = 5.0) -> VerificationClient:
    return VerificationClient(
        VerificationSettings(
            port=port,
            token=SecretStr(TOKEN),
            connect_timeout=2.0,
            response_timeout=response_timeout,
        )
    )


def responder(result: dict[str, Any]) -> Handler:
    return lambda request: json.dumps(envelope(request["requestId"], result)).encode()


STATUS_RESULT: dict[str, Any] = {
    "protocolSchemaVersion": 2,
    "inspectionSchemaVersion": 2,
    "moduleEnabled": True,
    "queueAvailable": True,
    "queueSize": 3,
    "botCount": 12,
}


def recovery_result(**overrides: Any) -> dict[str, Any]:
    result: dict[str, Any] = {
        "timestampMs": 1_700_000_000_000,
        "operation": "recover",
        "requestId": 1,
        "botGuid": 3,
        "botName": "RecoveryBot",
        "destination": "homebind",
        "observedAtDestination": False,
        "movementReset": True,
        "travelReset": True,
        "taxiReset": True,
        "outcome": "recovered",
        "reason": "homebind_teleport_accepted",
        "mutationState": "completed",
        "persistenceState": "deferred",
    }
    result.update(overrides)
    return result


class TestHappyPath:
    def test_status_round_trips_over_the_framed_socket(self) -> None:
        with FakeVerificationServer(responder(STATUS_RESULT)) as server:
            result = make_client(server.port).status()

        assert result.queue_size == 3
        assert result.bot_count == 12
        assert server.requests[0]["operation"] == "status"
        assert server.requests[0]["token"] == TOKEN

    def test_inspect_returns_the_full_typed_snapshot(self) -> None:
        with FakeVerificationServer(responder(inspection_payload())) as server:
            result = make_client(server.port).inspect(bot_guid=3)

        assert result.identity.name == "Grimtusk"
        assert result.finance.money_copper == 123_456_789
        assert result.economy.outcome == "operation"

    def test_check_reports_the_match_and_the_snapshot_it_was_evaluated_against(self) -> None:
        payload = {"matched": True, "condition": "action", "snapshot": inspection_payload()}
        with FakeVerificationServer(responder(payload)) as server:
            result = make_client(server.port).check(
                ActionCheck(bot_guid=3, after_sequence=6, action_name="melee", action_result="success")
            )

        assert result.matched is True
        assert result.condition == "action"
        assert result.snapshot.action.latest_attempt.sequence == 7

    def test_command_reports_dispatch_and_baselines_without_claiming_acceptance(self) -> None:
        payload = {
            "dispatched": True,
            "botGuid": "0x0000000000000003",
            "masterGuid": "0x0000000000000001",
            "command": "follow",
            "baselineActionSequence": 7,
            "baselineEconomySequence": 88,
        }
        with FakeVerificationServer(responder(payload)) as server:
            result = make_client(server.port).command(bot_guid=3, master_guid=1, command="follow")

        assert result.dispatched is True
        assert result.baseline_action_sequence == 7
        assert result.baseline_economy_sequence == 88

    def test_each_call_uses_a_fresh_connection_and_a_fresh_request_id(self) -> None:
        with FakeVerificationServer(responder(STATUS_RESULT)) as server:
            client = make_client(server.port)
            client.status()
            client.status()

        assert [request["requestId"] for request in server.requests] == [1, 2]

    def test_recovery_round_trips_as_one_exact_serialized_call(self) -> None:
        with FakeVerificationServer(responder(recovery_result())) as server:
            result = make_client(server.port).recover(bot_guid=3)

        assert result.outcome == "recovered"
        assert server.requests == [
            {
                "schemaVersion": 2,
                "requestId": 1,
                "token": TOKEN,
                "operation": "recover",
                "botGuid": 3,
                "destination": "homebind",
            }
        ]


class TestServerErrors:
    @pytest.mark.parametrize(
        ("code", "expected"),
        [
            ("bot_not_found", ErrorCode.BOT_NOT_FOUND),
            ("master_is_bot", ErrorCode.MASTER_IS_BOT),
            ("invalid_relationship", ErrorCode.INVALID_RELATIONSHIP),
            ("queue_full", ErrorCode.QUEUE_FULL),
            ("timeout", ErrorCode.TIMEOUT),
            ("shutdown", ErrorCode.SHUTDOWN),
        ],
    )
    def test_a_typed_failure_is_raised_with_its_code(self, code: str, expected: ErrorCode) -> None:
        handler: Handler = lambda request: json.dumps(  # noqa: E731
            error_envelope(request["requestId"], code)
        ).encode()
        with FakeVerificationServer(handler) as server, pytest.raises(ServerError) as caught:
            make_client(server.port).inspect(bot_guid=3)

        assert caught.value.code is expected

    def test_response_too_large_raises_rather_than_yielding_a_partial_snapshot(self) -> None:
        handler: Handler = lambda request: json.dumps(  # noqa: E731
            error_envelope(request["requestId"], "response_too_large")
        ).encode()
        with FakeVerificationServer(handler) as server, pytest.raises(ServerError) as caught:
            make_client(server.port).inspect(bot_guid=3)

        assert caught.value.code is ErrorCode.RESPONSE_TOO_LARGE

    def test_a_server_error_never_leaks_the_token(self) -> None:
        handler: Handler = lambda request: json.dumps(  # noqa: E731
            error_envelope(request["requestId"], "authentication_failed")
        ).encode()
        with FakeVerificationServer(handler) as server, pytest.raises(ServerError) as caught:
            make_client(server.port).status()

        assert TOKEN not in str(caught.value)
        assert TOKEN not in repr(caught.value)

    @pytest.mark.parametrize(
        ("code", "expected"),
        [
            ("malformed_request", ErrorCode.MALFORMED_REQUEST),
            ("invalid_guid", ErrorCode.INVALID_GUID),
            ("unsupported_schema_version", ErrorCode.UNSUPPORTED_SCHEMA_VERSION),
            ("unknown_operation", ErrorCode.UNKNOWN_OPERATION),
            ("authentication_failed", ErrorCode.AUTHENTICATION_FAILED),
            ("bot_not_found", ErrorCode.BOT_NOT_FOUND),
            ("bot_unavailable", ErrorCode.BOT_UNAVAILABLE),
            ("not_managed_playerbot", ErrorCode.NOT_MANAGED_PLAYERBOT),
            ("unsupported_destination", ErrorCode.UNSUPPORTED_DESTINATION),
            ("recovery_failed", ErrorCode.RECOVERY_FAILED),
            ("queue_full", ErrorCode.QUEUE_FULL),
            ("shutdown", ErrorCode.SHUTDOWN),
            ("internal_error", ErrorCode.INTERNAL_ERROR),
            ("response_too_large", ErrorCode.RESPONSE_TOO_LARGE),
            ("timeout", ErrorCode.TIMEOUT),
        ],
    )
    def test_recovery_preserves_every_typed_server_failure(self, code: str, expected: ErrorCode) -> None:
        handler: Handler = lambda request: json.dumps(  # noqa: E731
            error_envelope(request["requestId"], code)
        ).encode()
        with FakeVerificationServer(handler) as server, pytest.raises(ServerError) as caught:
            make_client(server.port).recover(bot_guid=3)

        assert caught.value.code is expected


class TestProtocolFaults:
    def test_a_frame_shorter_than_its_header_claims_is_refused(self) -> None:
        def handler(request: dict[str, Any]) -> RawFrame:
            body = json.dumps(envelope(request["requestId"], STATUS_RESULT)).encode()
            # The header promises ten more bytes than the body carries, then the peer closes.
            return RawFrame((len(body) + 10).to_bytes(4, "big") + body)

        with FakeVerificationServer(handler) as server, pytest.raises(ProtocolMismatchError):
            make_client(server.port, response_timeout=2.0).status()

    def test_a_frame_above_the_payload_ceiling_is_refused_before_it_is_read(self) -> None:
        handler: Handler = lambda request: RawFrame(  # noqa: E731
            (MAX_FRAME_PAYLOAD_BYTES + 1).to_bytes(4, "big")
        )
        with FakeVerificationServer(handler) as server, pytest.raises(ProtocolMismatchError):
            make_client(server.port, response_timeout=2.0).status()

    def test_a_response_carrying_another_request_id_is_refused(self) -> None:
        handler: Handler = lambda request: json.dumps(  # noqa: E731
            envelope(request["requestId"] + 1, STATUS_RESULT)
        ).encode()
        with FakeVerificationServer(handler) as server, pytest.raises(ProtocolMismatchError):
            make_client(server.port).status()

    def test_a_peer_that_closes_without_answering_is_refused(self) -> None:
        with FakeVerificationServer(lambda request: b"") as server, pytest.raises(ProtocolMismatchError):
            make_client(server.port).status()

    def test_a_peer_that_never_answers_times_out(self) -> None:
        server = FakeVerificationServer(lambda request: None)
        with server, pytest.raises(VerificationTimeoutError):
            make_client(server.port, response_timeout=0.4).status()

    def test_recovery_response_timeout_retains_its_stage(self) -> None:
        server = FakeVerificationServer(lambda request: None)
        with server, pytest.raises(VerificationTimeoutError) as caught:
            make_client(server.port, response_timeout=0.2).recover(bot_guid=3)

        assert caught.value.stage is VerificationTimeoutStage.RESPONSE

    def test_recovery_connect_timeout_retains_its_stage(self, monkeypatch: pytest.MonkeyPatch) -> None:
        def time_out(*args: object, **kwargs: object) -> socket.socket:
            raise TimeoutError

        monkeypatch.setattr(socket, "create_connection", time_out)
        with pytest.raises(VerificationTimeoutError) as caught:
            make_client(1, response_timeout=0.2).recover(bot_guid=3)

        assert caught.value.stage is VerificationTimeoutStage.SOCKET

    def test_a_peer_that_trickles_bytes_hits_the_aggregate_deadline(self) -> None:
        """The timeout is one deadline for the whole exchange, not a fresh budget per read.

        Every gap here is well inside the response timeout, so a per read timeout never fires
        and the call completes late instead of failing. The client must give up at the deadline.
        """
        gap = 0.05
        timeout = 0.3

        def handler(request: dict[str, Any]) -> DripFrame:
            return DripFrame(json.dumps(envelope(request["requestId"], STATUS_RESULT)).encode(), gap)

        server = FakeVerificationServer(handler)
        with server:
            started = time.monotonic()
            with pytest.raises(VerificationTimeoutError):
                make_client(server.port, response_timeout=timeout).status()
            elapsed = time.monotonic() - started

        # The full drip needs well over a second. Returning near the deadline proves the client
        # stopped waiting on its own clock rather than on the peer finishing.
        assert elapsed < 1.0


class TestSerialization:
    def test_concurrent_callers_never_produce_two_in_flight_requests(self) -> None:
        lock = threading.Lock()
        state = {"active": 0, "peak": 0}

        def handler(request: dict[str, Any]) -> bytes:
            with lock:
                state["active"] += 1
                state["peak"] = max(state["peak"], state["active"])
            threading.Event().wait(0.05)
            with lock:
                state["active"] -= 1
            return json.dumps(envelope(request["requestId"], STATUS_RESULT)).encode()

        with FakeVerificationServer(handler) as server:
            client = make_client(server.port)
            threads = [threading.Thread(target=client.status) for _ in range(6)]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join(timeout=10)

        assert state["peak"] == 1
        assert len(server.requests) == 6
        assert sorted(request["requestId"] for request in server.requests) == [1, 2, 3, 4, 5, 6]

    def test_recovery_lock_wait_uses_the_aggregate_deadline(self) -> None:
        first_started = threading.Event()
        release_first = threading.Event()

        def handler(request: dict[str, Any]) -> bytes:
            first_started.set()
            release_first.wait(timeout=5)
            return json.dumps(
                envelope(request["requestId"], recovery_result(requestId=request["requestId"]))
            ).encode()

        with FakeVerificationServer(handler) as server:
            client = make_client(server.port)
            first = threading.Thread(target=client.recover, kwargs={"bot_guid": 3})
            first.start()
            assert first_started.wait(timeout=2)
            with pytest.raises(VerificationTimeoutError) as caught:
                client.recover(bot_guid=3, timeout=0.05)
            release_first.set()
            first.join(timeout=5)

        assert caught.value.stage is VerificationTimeoutStage.CLIENT_LOCK
        assert len(server.requests) == 1


def make_mcp_server(port: int, *, response_timeout: float = 5.0) -> MCPServer:
    """Builds the tool surface over a client pointed at the test owned fake endpoint."""
    return build_server(make_client(port, response_timeout=response_timeout))


class TestToolsOverTheWire:
    @pytest.mark.anyio
    async def test_server_status_returns_a_structured_result(self) -> None:
        with FakeVerificationServer(responder(STATUS_RESULT)) as server:
            async with Client(make_mcp_server(server.port)) as client:
                result = await client.call_tool("server_status", {})

        assert result.structured_content is not None
        assert result.structured_content["queueSize"] == 3
        assert result.structured_content["botCount"] == 12

    @pytest.mark.anyio
    async def test_list_bots_returns_pagination_metadata(self) -> None:
        payload = {
            "bots": [],
            "nextAfterGuid": 41,
            "hasMore": True,
            "completeness": {"totalCount": 300, "returnedCount": 0, "truncated": True},
        }
        with FakeVerificationServer(responder(payload)) as server:
            async with Client(make_mcp_server(server.port)) as client:
                result = await client.call_tool("list_bots", {"after_guid": 0, "limit": 50})

        assert result.structured_content is not None
        assert result.structured_content["nextAfterGuid"] == 41
        assert result.structured_content["completeness"]["totalCount"] == 300

    @pytest.mark.anyio
    async def test_inspect_bot_returns_the_full_snapshot(self) -> None:
        with FakeVerificationServer(responder(inspection_payload())) as server:
            async with Client(make_mcp_server(server.port)) as client:
                result = await client.call_tool("inspect_bot", {"bot_guid": 3})

        assert result.structured_content is not None
        assert result.structured_content["identity"]["name"] == "Grimtusk"
        assert result.structured_content["finance"]["moneyCopper"] == 123_456_789

    @pytest.mark.anyio
    async def test_inspect_bot_loops_returns_actionable_serverwide_records(self) -> None:
        payload = {
            "anomalies": [
                {
                    "bot": {"guid": "0x0000000000000003", "guidLow": 3, "name": "Grimtusk", "level": 7},
                    "classifier": "repeated_action",
                    "objective": {"kind": "profession", "key": 164, "title": "profession trainer"},
                    "action": "apply oil",
                    "evidence": {"firstTimestampMs": 1000, "lastTimestampMs": 36000, "count": 8},
                    "progressDelta": 0.0,
                    "deathCount": 1,
                    "recoveryCount": 2,
                }
            ],
            "completeness": {
                "totalBotCount": 300,
                "totalAnomalyCount": 73,
                "returnedCount": 1,
                "truncated": True,
            },
        }
        with FakeVerificationServer(responder(payload)) as server:
            async with Client(make_mcp_server(server.port)) as client:
                result = await client.call_tool("inspect_bot_loops", {"limit": 1})

        assert result.structured_content is not None
        assert result.structured_content["anomalies"][0]["action"] == "apply oil"
        assert result.structured_content["completeness"]["totalBotCount"] == 300
        assert result.structured_content["completeness"]["truncated"] is True
        assert server.requests[0]["operation"] == "anomalies"
        assert server.requests[0]["limit"] == 1

    @pytest.mark.anyio
    async def test_send_bot_command_reports_dispatch_and_baselines(self) -> None:
        payload = {
            "dispatched": True,
            "botGuid": "0x0000000000000003",
            "masterGuid": "0x0000000000000001",
            "command": "follow",
            "baselineActionSequence": 7,
            "baselineEconomySequence": 88,
        }

        def handler(request: dict[str, Any]) -> bytes:
            inspection = inspection_payload()
            inspection["master"]["guid"] = "GUID Full: 0x0000000000000001 Type: Player Low: 1"
            result = inspection if request["operation"] == "inspect" else payload
            return json.dumps(envelope(request["requestId"], result)).encode()

        with FakeVerificationServer(handler) as server:
            async with Client(make_mcp_server(server.port)) as client:
                result = await client.call_tool(
                    "send_bot_command", {"bot_guid": 3, "master_guid": 1, "command": "follow"}
                )

        assert result.structured_content is not None
        assert result.structured_content["dispatched"] is True
        assert result.structured_content["baselineActionSequence"] == 7
        assert [request["operation"] for request in server.requests] == ["inspect", "command"]

    @pytest.mark.anyio
    async def test_recover_bot_calls_only_the_narrow_recovery_operation(self) -> None:
        with FakeVerificationServer(responder(recovery_result())) as server:
            async with Client(make_mcp_server(server.port)) as client:
                result = await client.call_tool("recover_bot", {"bot_guid": 3, "destination": "homebind"})

        assert result.structured_content is not None
        assert result.structured_content["outcome"] == "recovered"
        assert [request["operation"] for request in server.requests] == ["recover"]
        assert "masterGuid" not in server.requests[0]

    @pytest.mark.anyio
    @pytest.mark.parametrize("bot_guid", [0, -1, UINT32_MAX + 1, True, "3"])
    async def test_recover_bot_rejects_an_invalid_guid_without_network_io(self, bot_guid: object) -> None:
        with FakeVerificationServer(responder(recovery_result())) as server:
            async with Client(make_mcp_server(server.port)) as client:
                result = await client.call_tool(
                    "recover_bot", {"bot_guid": bot_guid, "destination": "homebind"}
                )

        assert result.structured_content is not None
        assert result.structured_content["outcome"] == "invalid_request"
        assert result.structured_content["reason"] == "invalid_guid"
        assert result.structured_content["botGuid"] is None
        assert result.structured_content["mutationState"] == "not_started"
        assert server.requests == []

    @pytest.mark.anyio
    @pytest.mark.parametrize(
        ("destination", "outcome", "reason"),
        [
            ("inn", "unsupported_destination", "destination_not_homebind"),
            ("too-long!", "invalid_request", "invalid_tool_input"),
            (7, "invalid_request", "invalid_tool_input"),
        ],
    )
    async def test_recover_bot_rejects_invalid_destinations_without_network_io(
        self, destination: object, outcome: str, reason: str
    ) -> None:
        with FakeVerificationServer(responder(recovery_result())) as server:
            async with Client(make_mcp_server(server.port)) as client:
                result = await client.call_tool("recover_bot", {"bot_guid": 3, "destination": destination})

        assert result.structured_content is not None
        assert result.structured_content["outcome"] == outcome
        assert result.structured_content["reason"] == reason
        assert result.structured_content["destination"] in {"missing", "unsupported"}
        if isinstance(destination, str):
            assert destination not in json.dumps(result.structured_content)
        assert server.requests == []

    @pytest.mark.anyio
    @pytest.mark.parametrize(
        ("code", "outcome", "reason", "mutation"),
        [
            ("malformed_request", "invalid_request", "malformed_request", "not_started"),
            ("invalid_guid", "invalid_request", "invalid_guid", "not_started"),
            ("unsupported_schema_version", "invalid_request", "unsupported_schema", "not_started"),
            ("unknown_operation", "invalid_request", "unknown_operation", "not_started"),
            ("authentication_failed", "unauthorized", "authentication_failed", "not_started"),
            ("bot_not_found", "bot_not_found", "character_not_found", "not_started"),
            ("bot_unavailable", "bot_not_available", "character_offline", "not_started"),
            ("not_managed_playerbot", "not_managed_playerbot", "playerbot_ai_missing", "not_started"),
            (
                "unsupported_destination",
                "unsupported_destination",
                "destination_not_homebind",
                "not_started",
            ),
            ("operation_unavailable", "recovery_failed", "operation_unavailable", "not_started"),
            ("queue_full", "recovery_failed", "queue_full", "not_started"),
            ("shutdown", "recovery_failed", "shutting_down", "not_started"),
            (
                "response_too_large",
                "recovery_failed",
                "response_too_large",
                "unknown_after_execution_started",
            ),
            (
                "recovery_failed",
                "recovery_failed",
                "internal_error",
                "unknown_after_execution_started",
            ),
            (
                "internal_error",
                "recovery_failed",
                "internal_error",
                "unknown_after_execution_started",
            ),
            ("timeout", "recovery_timed_out", "queue_timeout_before_claim", "not_started"),
        ],
    )
    async def test_recover_bot_maps_every_typed_server_failure_without_copying_messages(
        self, code: str, outcome: str, reason: str, mutation: str
    ) -> None:
        handler: Handler = lambda request: json.dumps(  # noqa: E731
            error_envelope(request["requestId"], code, f"unsafe {TOKEN}")
        ).encode()
        with FakeVerificationServer(handler) as server:
            async with Client(make_mcp_server(server.port)) as client:
                result = await client.call_tool("recover_bot", {"bot_guid": 3, "destination": "homebind"})

        assert result.structured_content is not None
        assert result.structured_content["outcome"] == outcome
        assert result.structured_content["reason"] == reason
        assert result.structured_content["mutationState"] == mutation
        assert TOKEN not in json.dumps(result.structured_content)

    @pytest.mark.anyio
    async def test_recover_bot_maps_invalid_server_result_without_copying_it(self) -> None:
        invalid = recovery_result(outcome="recovered", reason="character_offline", unsafe=TOKEN)
        with FakeVerificationServer(responder(invalid)) as server:
            async with Client(make_mcp_server(server.port)) as client:
                result = await client.call_tool("recover_bot", {"bot_guid": 3, "destination": "homebind"})

        assert result.structured_content is not None
        assert result.structured_content["outcome"] == "recovery_failed"
        assert result.structured_content["reason"] == "invalid_server_response"
        assert TOKEN not in json.dumps(result.structured_content)


class RecoveryFailureClient:
    def __init__(self, error: Exception) -> None:
        self.error = error

    def recover(self, *, bot_guid: int, timeout: float | None = None) -> object:
        raise self.error


class TestRecoveryAdapterFailures:
    @staticmethod
    def _server(error: Exception) -> MCPServer:
        client = cast(VerificationClient, cast(object, RecoveryFailureClient(error)))
        return build_server(client)

    @pytest.mark.anyio
    @pytest.mark.parametrize(
        ("error", "outcome", "reason", "mutation"),
        [
            (
                VerificationTimeoutError(VerificationTimeoutStage.CLIENT_LOCK, "unsafe"),
                "recovery_timed_out",
                "client_lock_timeout",
                "not_started",
            ),
            (
                VerificationTimeoutError(VerificationTimeoutStage.SOCKET, "unsafe"),
                "recovery_timed_out",
                "socket_timeout",
                "not_started",
            ),
            (
                VerificationTimeoutError(VerificationTimeoutStage.RESPONSE, "unsafe"),
                "recovery_timed_out",
                "response_timeout",
                "unknown_after_execution_started",
            ),
            (
                ConfigurationError("unsafe"),
                "recovery_failed",
                "adapter_configuration",
                "not_started",
            ),
            (
                VerificationConnectionError(request_sent=False),
                "recovery_failed",
                "server_unreachable",
                "not_started",
            ),
            (
                VerificationConnectionError(request_sent=True),
                "recovery_failed",
                "server_unreachable",
                "unknown_after_execution_started",
            ),
            (
                ProtocolMismatchError(f"unsafe {TOKEN}"),
                "recovery_failed",
                "protocol_mismatch",
                "unknown_after_execution_started",
            ),
            (
                VerificationError(f"unsafe {TOKEN}"),
                "recovery_failed",
                "server_unreachable",
                "unknown_after_execution_started",
            ),
        ],
    )
    async def test_adapter_failure_is_a_closed_sanitized_result(
        self, error: Exception, outcome: str, reason: str, mutation: str
    ) -> None:
        async with Client(self._server(error)) as client:
            result = await client.call_tool("recover_bot", {"bot_guid": 3, "destination": "homebind"})

        assert result.structured_content is not None
        assert result.structured_content["outcome"] == outcome
        assert result.structured_content["reason"] == reason
        assert result.structured_content["mutationState"] == mutation
        assert TOKEN not in json.dumps(result.structured_content)


class TestMutatingToolGuards:
    @pytest.mark.anyio
    async def test_a_wrong_master_fails_before_a_command_request_is_sent(self) -> None:
        """A read only inspection must reject the mismatch before the command operation."""
        with FakeVerificationServer(responder(inspection_payload())) as server:
            async with Client(make_mcp_server(server.port)) as client:
                result = await client.call_tool(
                    "send_bot_command", {"bot_guid": 3, "master_guid": 99, "command": "follow"}
                )

        assert result.is_error is True
        assert [request["operation"] for request in server.requests] == ["inspect"]

    @pytest.mark.anyio
    async def test_an_invalid_guid_fails_before_any_request_is_sent(self) -> None:
        with FakeVerificationServer(responder(STATUS_RESULT)) as server:
            async with Client(make_mcp_server(server.port)) as client:
                result = await client.call_tool(
                    "send_bot_command", {"bot_guid": 0, "master_guid": 1, "command": "follow"}
                )

            assert result.is_error is True
            assert server.requests == []

    @pytest.mark.anyio
    async def test_an_empty_command_fails_before_any_request_is_sent(self) -> None:
        with FakeVerificationServer(responder(STATUS_RESULT)) as server:
            async with Client(make_mcp_server(server.port)) as client:
                result = await client.call_tool(
                    "send_bot_command", {"bot_guid": 3, "master_guid": 1, "command": ""}
                )

            assert result.is_error is True
            assert server.requests == []

    @pytest.mark.anyio
    async def test_a_malformed_condition_fails_before_any_request_is_sent(self) -> None:
        with FakeVerificationServer(responder(STATUS_RESULT)) as server:
            async with Client(make_mcp_server(server.port)) as client:
                result = await client.call_tool(
                    "wait_for_bot",
                    {"bot_guid": 3, "condition": "action", "action_name": "melee"},
                )

            assert result.is_error is True
            assert server.requests == []


def check_payload(matched: bool, condition: str, **snapshot_overrides: Any) -> dict[str, Any]:
    return {
        "matched": matched,
        "condition": condition,
        "snapshot": inspection_payload(**snapshot_overrides),
    }


def scripted(script: list[dict[str, Any]]) -> Handler:
    """Serves each scripted result in turn, then repeats the last one indefinitely."""
    counter = itertools.count()

    def handler(request: dict[str, Any]) -> bytes:
        index = min(next(counter), len(script) - 1)
        return json.dumps(envelope(request["requestId"], script[index])).encode()

    return handler


class TestWaitBehaviour:
    @pytest.mark.anyio
    async def test_a_condition_already_true_matches_on_the_first_poll(self) -> None:
        with FakeVerificationServer(scripted([check_payload(True, "map")])) as server:
            async with Client(make_mcp_server(server.port)) as client:
                result = await client.call_tool(
                    "wait_for_bot", {"bot_guid": 3, "condition": "map", "map_id": 571}
                )

        assert result.structured_content is not None
        assert result.structured_content["matched"] is True
        assert result.structured_content["timedOut"] is False
        assert result.structured_content["polls"] == 1

    @pytest.mark.anyio
    async def test_a_condition_that_becomes_true_later_is_matched(self) -> None:
        script = [
            check_payload(False, "map"),
            check_payload(False, "map"),
            check_payload(True, "map"),
        ]
        with FakeVerificationServer(scripted(script)) as server:
            async with Client(make_mcp_server(server.port)) as client:
                result = await client.call_tool(
                    "wait_for_bot",
                    {"bot_guid": 3, "condition": "map", "map_id": 571, "timeout_seconds": 5.0},
                )

        assert result.structured_content is not None
        assert result.structured_content["matched"] is True
        assert result.structured_content["polls"] == 3

    @pytest.mark.anyio
    async def test_a_condition_that_never_becomes_true_times_out(self) -> None:
        with FakeVerificationServer(scripted([check_payload(False, "map")])) as server:
            async with Client(make_mcp_server(server.port)) as client:
                result = await client.call_tool(
                    "wait_for_bot",
                    {"bot_guid": 3, "condition": "map", "map_id": 571, "timeout_seconds": 1.0},
                )

        assert result.structured_content is not None
        assert result.structured_content["matched"] is False
        assert result.structured_content["timedOut"] is True
        assert result.structured_content["elapsedSeconds"] >= 1.0

    @pytest.mark.anyio
    async def test_a_stalled_poll_cannot_overrun_the_wait_deadline(self) -> None:
        """The tool timeout bounds an unanswered check, not only the gaps between polls."""
        with FakeVerificationServer(lambda request: None) as server:
            async with Client(make_mcp_server(server.port, response_timeout=2.0)) as client:
                started = time.monotonic()
                result = await client.call_tool(
                    "wait_for_bot",
                    {"bot_guid": 3, "condition": "map", "map_id": 571, "timeout_seconds": 1.0},
                )
                elapsed = time.monotonic() - started

        assert result.is_error is True
        assert elapsed < 1.5

    @pytest.mark.anyio
    async def test_a_timeout_outside_the_allowed_range_is_refused(self) -> None:
        with FakeVerificationServer(scripted([check_payload(True, "map")])) as server:
            async with Client(make_mcp_server(server.port)) as client:
                for timeout in (0.5, 61.0):
                    result = await client.call_tool(
                        "wait_for_bot",
                        {
                            "bot_guid": 3,
                            "condition": "map",
                            "map_id": 571,
                            "timeout_seconds": timeout,
                        },
                    )
                    assert result.is_error is True
            assert server.requests == []

    @pytest.mark.anyio
    async def test_a_repeated_change_is_recorded_once(self) -> None:
        """Money moves once and then holds. The report must not repeat that transition."""
        script = [
            check_payload(False, "map"),
            check_payload(
                False,
                "map",
                finance={"moneyCopper": 1, "freeTradeskillCopper": 9000, "freeSpellsCopper": 4500},
            ),
            check_payload(
                False,
                "map",
                finance={"moneyCopper": 1, "freeTradeskillCopper": 9000, "freeSpellsCopper": 4500},
            ),
            check_payload(
                True,
                "map",
                finance={"moneyCopper": 1, "freeTradeskillCopper": 9000, "freeSpellsCopper": 4500},
            ),
        ]
        with FakeVerificationServer(scripted(script)) as server:
            async with Client(make_mcp_server(server.port)) as client:
                result = await client.call_tool(
                    "wait_for_bot",
                    {"bot_guid": 3, "condition": "map", "map_id": 571, "timeout_seconds": 5.0},
                )

        assert result.structured_content is not None
        finance = [t for t in result.structured_content["transitions"] if t["kind"] == "finance"]
        assert len(finance) == 1
        assert result.structured_content["transitionsTruncated"] is False

    @pytest.mark.anyio
    async def test_transitions_are_capped_and_the_cap_is_reported(
        self, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        # Shrink the interval so the cap is reached quickly. This is loop speed, not a sleep.
        monkeypatch.setattr(server_module, "WAIT_POLL_INTERVAL_SECONDS", 0.001)
        transition_cap = 8
        monkeypatch.setattr(server_module, "MAX_WAIT_TRANSITIONS", transition_cap)
        script = [
            check_payload(
                step == 5,
                "map",
                finance={
                    "moneyCopper": 1000 + step,
                    "freeTradeskillCopper": 9000,
                    "freeSpellsCopper": 4500,
                },
                position={
                    "mapId": 500 + step,
                    "zoneId": 1,
                    "areaId": 1,
                    "x": 1.0,
                    "y": 2.0,
                    "z": 3.0,
                    "orientation": 0.5,
                    "movementFlags": 0,
                    "moving": False,
                    "movementState": "stationary",
                },
            )
            for step in range(6)
        ]
        with FakeVerificationServer(scripted(script)) as server:
            async with Client(make_mcp_server(server.port)) as client:
                result = await client.call_tool(
                    "wait_for_bot",
                    {"bot_guid": 3, "condition": "map", "map_id": 571, "timeout_seconds": 5.0},
                )

        assert result.structured_content is not None
        assert len(result.structured_content["transitions"]) == transition_cap
        assert result.structured_content["transitionsTruncated"] is True

    @pytest.mark.anyio
    async def test_a_wait_in_progress_can_be_cancelled(self) -> None:
        """Codex must be able to abandon a wait, and the poll loop must actually stop.

        Asserting only that the call was cancelled would pass even if the loop kept polling
        the worldserver forever, so this also proves the request count stops growing.
        """
        with FakeVerificationServer(scripted([check_payload(False, "map")])) as server:
            async with Client(make_mcp_server(server.port)) as client:
                started = time.monotonic()
                with anyio.move_on_after(0.4) as scope:
                    await client.call_tool(
                        "wait_for_bot",
                        {
                            "bot_guid": 3,
                            "condition": "map",
                            "map_id": 571,
                            "timeout_seconds": 30.0,
                        },
                    )
                elapsed = time.monotonic() - started
                polled_at_cancel = len(server.requests)
                # Several poll intervals worth of quiet time. A live loop would add requests.
                await anyio.sleep(0.6)
                polled_after_cancel = len(server.requests)

        assert scope.cancelled_caught is True
        assert elapsed < 5.0
        assert polled_at_cancel > 0, "the wait should have polled at least once before cancelling"
        assert polled_after_cancel == polled_at_cancel

    @pytest.mark.anyio
    async def test_cancellation_does_not_wait_for_a_stalled_poll(self) -> None:
        """A blocked read may finish in its worker thread, but the MCP call must cancel promptly."""
        with FakeVerificationServer(lambda request: None) as server:
            async with Client(make_mcp_server(server.port, response_timeout=2.0)) as client:
                started = time.monotonic()
                with anyio.move_on_after(0.2) as scope:
                    await client.call_tool(
                        "wait_for_bot",
                        {
                            "bot_guid": 3,
                            "condition": "map",
                            "map_id": 571,
                            "timeout_seconds": 30.0,
                        },
                    )
                elapsed = time.monotonic() - started

        assert scope.cancelled_caught is True
        assert elapsed < 0.8
        assert [request["operation"] for request in server.requests] == ["check"]


class TestWaitConditionsWithBaselines:
    @pytest.mark.parametrize(
        ("condition", "arguments", "expected_fields"),
        [
            ("money_decrease", {"baseline_copper": 5000}, {"baselineCopper"}),
            ("known_recipe", {"spell_id": 3538}, {"spellId"}),
            ("profession_skill", {"skill_id": 164, "minimum_value": 300}, {"skillId", "minimumValue"}),
            (
                "economy",
                {"after_sequence": 88, "economy_outcome": "operation"},
                {"afterSequence", "economyOutcome"},
            ),
        ],
    )
    @pytest.mark.anyio
    async def test_a_caller_supplied_baseline_reaches_the_server_intact(
        self, condition: str, arguments: dict[str, Any], expected_fields: set[str]
    ) -> None:
        with FakeVerificationServer(scripted([check_payload(True, condition)])) as server:
            async with Client(make_mcp_server(server.port)) as client:
                result = await client.call_tool(
                    "wait_for_bot", {"bot_guid": 3, "condition": condition, **arguments}
                )

        assert result.structured_content is not None
        assert result.structured_content["matched"] is True
        sent = server.requests[0]
        assert sent["condition"] == condition
        assert expected_fields <= sent.keys()
        for name, value in arguments.items():
            camel = name.split("_")[0] + "".join(part.title() for part in name.split("_")[1:])
            assert sent[camel] == value

    @pytest.mark.anyio
    async def test_a_match_is_reported_even_when_display_collections_are_truncated(self) -> None:
        """The server evaluates conditions on complete state, so truncation must not soften it."""
        payload = check_payload(
            True,
            "known_recipe",
            knownRecipeSpellIds={
                "items": [2018, 3100],
                "completeness": {"totalCount": 900, "returnedCount": 2, "truncated": True},
            },
        )
        with FakeVerificationServer(scripted([payload])) as server:
            async with Client(make_mcp_server(server.port)) as client:
                result = await client.call_tool(
                    "wait_for_bot", {"bot_guid": 3, "condition": "known_recipe", "spell_id": 9999}
                )

        assert result.structured_content is not None
        assert result.structured_content["matched"] is True
        recipes = result.structured_content["snapshot"]["knownRecipeSpellIds"]
        assert recipes["completeness"]["truncated"] is True
        assert recipes["completeness"]["totalCount"] == 900


class TestStdioAdapter:
    """Proves the console script really starts and speaks MCP, not just that build_server works.

    This is the only test that launches a subprocess. It reads its port and token from the
    environment exactly as the deployed wrapper supplies them, and talks to a fake endpoint.
    """

    @pytest.mark.anyio
    async def test_the_adapter_starts_over_stdio_and_answers_server_status(self) -> None:
        with FakeVerificationServer(responder(STATUS_RESULT)) as server:
            parameters = StdioServerParameters(
                command=sys.executable,
                args=["-m", "playerbot_mcp.server"],
                env={
                    "PATH": os.environ.get("PATH", ""),
                    "PLAYERBOT_VERIFICATION_PORT": str(server.port),
                    "PLAYERBOT_VERIFICATION_TOKEN": TOKEN,
                },
            )
            async with (
                stdio_client(parameters) as (read, write),
                ClientSession(read, write) as session,
            ):
                await session.initialize()
                listed = await session.list_tools()
                result = await session.call_tool("server_status", {})

        assert {tool.name for tool in listed.tools} == {
            "server_status",
            "list_bots",
            "inspect_bot",
            "inspect_bot_loops",
            "wait_for_bot",
            "send_bot_command",
            "recover_bot",
        }
        assert result.structured_content is not None
        assert result.structured_content["botCount"] == 12

    @pytest.mark.anyio
    async def test_the_adapter_refuses_to_start_without_its_environment(self) -> None:
        """Fail closed. An adapter without a token must not come up and appear healthy."""
        completed = subprocess.run(
            [sys.executable, "-m", "playerbot_mcp.server"],
            env={"PATH": os.environ.get("PATH", "")},
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        assert completed.returncode != 0
        assert "PLAYERBOT_VERIFICATION_PORT" in completed.stderr
        assert TOKEN not in completed.stderr
