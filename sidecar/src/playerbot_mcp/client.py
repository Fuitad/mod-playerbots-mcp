"""Synchronous client for the loopback verification server.

The server answers one request per connection and processes work on the world thread, so this
client opens a fresh socket per call and serialises every call behind one lock. That matches the
version 2 server throughput contract and keeps request ids strictly increasing.
"""

from __future__ import annotations

import os
import socket
import threading
import time
from collections.abc import Mapping

from pydantic import BaseModel, ConfigDict, Field, SecretStr

from playerbot_mcp.protocol import (
    FRAME_HEADER_BYTES,
    MAX_ANOMALY_LIMIT,
    MAX_FRAME_PAYLOAD_BYTES,
    MAX_LIST_LIMIT,
    MIN_TOKEN_BYTES,
    AnomaliesRequest,
    AnomaliesResult,
    AnyCheck,
    CheckResult,
    CommandRequest,
    CommandResult,
    ConfigurationError,
    InspectRequest,
    InspectResult,
    ListRequest,
    ListResult,
    ProtocolMismatchError,
    RecoverRequest,
    RecoveryResult,
    ServerError,
    SetSkillRequest,
    SetSkillResult,
    StatusRequest,
    StatusResult,
    TeleportToGameObjectRequest,
    TeleportToGameObjectResult,
    VerificationConnectionError,
    VerificationError,
    VerificationRequest,
    VerificationTimeoutError,
    VerificationTimeoutStage,
    build_request_payload,
    encode_frame,
    parse_envelope,
)

__all__ = [
    "ConfigurationError",
    "ProtocolMismatchError",
    "ServerError",
    "VerificationClient",
    "VerificationError",
    "VerificationSettings",
    "VerificationTimeoutError",
]

PORT_VARIABLE = "PLAYERBOT_VERIFICATION_PORT"
TOKEN_VARIABLE = "PLAYERBOT_VERIFICATION_TOKEN"

# The server binds loopback only. A configurable host would just be a way to leak the token.
LOOPBACK_HOST = "127.0.0.1"


class VerificationSettings(BaseModel):
    """Connection settings. The token is a SecretStr so it stays out of reprs and dumps."""

    # extra="forbid" makes an attempt to supply a host a loud error rather than a silent no op.
    model_config = ConfigDict(frozen=True, extra="forbid")

    port: int = Field(ge=1, le=65535)
    token: SecretStr
    connect_timeout: float = Field(default=2.0, gt=0)
    response_timeout: float = Field(default=10.0, gt=0)

    @property
    def host(self) -> str:
        """Always loopback. The host is not a field, so no caller can point the token elsewhere."""
        return LOOPBACK_HOST

    @classmethod
    def from_environment(cls, env: Mapping[str, str] | None = None) -> VerificationSettings:
        """Reads the deployed secrets.env variables, failing closed on anything unusable."""
        source = os.environ if env is None else env

        raw_port = source.get(PORT_VARIABLE)
        if not raw_port:
            raise ConfigurationError(f"{PORT_VARIABLE} is not set.")
        try:
            port = int(raw_port)
        except ValueError as error:
            raise ConfigurationError(f"{PORT_VARIABLE} is not an integer.") from error
        if not 1 <= port <= 65535:
            raise ConfigurationError(f"{PORT_VARIABLE} must be from 1 through 65535.")

        token = source.get(TOKEN_VARIABLE)
        if not token:
            raise ConfigurationError(f"{TOKEN_VARIABLE} is not set.")
        if len(token.encode()) < MIN_TOKEN_BYTES:
            raise ConfigurationError(f"{TOKEN_VARIABLE} must be at least {MIN_TOKEN_BYTES} bytes.")

        return cls(port=port, token=SecretStr(token))


class VerificationClient:
    """Typed client for the nine verification operations."""

    def __init__(self, settings: VerificationSettings) -> None:
        self._settings = settings
        self._lock = threading.Lock()
        self._last_request_id = 0

    def status(self) -> StatusResult:
        return self._call(StatusRequest(), StatusResult)

    def list_bots(self, *, after_guid: int = 0, limit: int = MAX_LIST_LIMIT) -> ListResult:
        return self._call(ListRequest(after_guid=after_guid, limit=limit), ListResult)

    def inspect(self, *, bot_guid: int) -> InspectResult:
        return self._call(InspectRequest(bot_guid=bot_guid), InspectResult)

    def anomalies(self, *, limit: int = MAX_ANOMALY_LIMIT) -> AnomaliesResult:
        return self._call(AnomaliesRequest(limit=limit), AnomaliesResult)

    def check(self, request: AnyCheck, *, timeout: float | None = None) -> CheckResult:
        """Evaluates one condition, optionally within a caller supplied aggregate deadline."""
        return self._call(request, CheckResult, timeout=timeout)

    def command(self, *, bot_guid: int, master_guid: int, command: str) -> CommandResult:
        """Dispatches a command. A success here means dispatched, never accepted by the bot."""
        request = CommandRequest(bot_guid=bot_guid, master_guid=master_guid, command=command)
        return self._call(request, CommandResult)

    def set_skill(self, *, bot_guid: int, skill_id: int, value: int, maximum: int) -> SetSkillResult:
        """Verification staging: overwrite one skill the bot already knows."""
        request = SetSkillRequest(bot_guid=bot_guid, skill_id=skill_id, value=value, maximum=maximum)
        return self._call(request, SetSkillResult)

    def teleport_to_gameobject(self, *, bot_guid: int, game_object_entry: int) -> TeleportToGameObjectResult:
        """Verification staging: park the bot beside the nearest spawned gameobject of one entry."""
        request = TeleportToGameObjectRequest(bot_guid=bot_guid, game_object_entry=game_object_entry)
        return self._call(request, TeleportToGameObjectResult)

    def recover(self, *, bot_guid: int, timeout: float | None = None) -> RecoveryResult:
        """Request one exact online managed Playerbot's authoritative homebind."""
        deadline = self._settings.response_timeout if timeout is None else timeout
        return self._call(RecoverRequest(bot_guid=bot_guid), RecoveryResult, timeout=deadline)

    def _call[ResultT: BaseModel](
        self,
        request: VerificationRequest,
        result_model: type[ResultT],
        *,
        timeout: float | None = None,
    ) -> ResultT:
        deadline = time.monotonic() + timeout if timeout is not None else None
        if deadline is None:
            self._lock.acquire()
        else:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not self._lock.acquire(timeout=remaining):
                raise VerificationTimeoutError(
                    VerificationTimeoutStage.CLIENT_LOCK,
                    f"The verification request exceeded its {timeout}s deadline.",
                )

        try:
            self._last_request_id += 1
            request_id = self._last_request_id
            payload = build_request_payload(
                request, request_id=request_id, token=self._settings.token.get_secret_value()
            )
            response = self._exchange(payload, deadline=deadline, timeout=timeout)
        finally:
            self._lock.release()

        envelope = parse_envelope(response, expected_id=request_id)
        if envelope.error is not None:
            raise ServerError(envelope.error.code, envelope.error.message)
        assert envelope.result is not None  # parse_envelope rejects a success with no result
        return result_model.model_validate(envelope.result)

    def _exchange(
        self, payload: bytes, *, deadline: float | None = None, timeout: float | None = None
    ) -> bytes:
        frame = encode_frame(payload)
        request_sent = False
        timeout_seconds = (
            self._settings.response_timeout
            if timeout is None
            else min(self._settings.response_timeout, timeout)
        )
        try:
            connect_timeout = self._settings.connect_timeout
            if deadline is not None:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise VerificationTimeoutError(
                        VerificationTimeoutStage.RESPONSE,
                        f"The verification request exceeded its {timeout_seconds}s deadline.",
                    )
                connect_timeout = min(connect_timeout, remaining)

            try:
                connection = socket.create_connection(
                    (self._settings.host, self._settings.port), timeout=connect_timeout
                )
            except TimeoutError as error:
                raise VerificationTimeoutError(
                    VerificationTimeoutStage.SOCKET,
                    f"The verification server connection exceeded {timeout_seconds}s.",
                ) from error

            with connection:
                # One deadline covers the whole exchange. A per operation timeout would let a peer
                # trickle one byte just inside the limit forever and never trip anything.
                response_deadline = time.monotonic() + self._settings.response_timeout
                exchange_deadline = (
                    response_deadline if deadline is None else min(deadline, response_deadline)
                )
                self._arm(connection, exchange_deadline, timeout_seconds)
                connection.sendall(frame)
                request_sent = True
                header = self._read_exactly(
                    connection, FRAME_HEADER_BYTES, exchange_deadline, timeout_seconds
                )
                declared = int.from_bytes(header, "big")
                if declared > MAX_FRAME_PAYLOAD_BYTES:
                    raise ProtocolMismatchError(f"The response declares {declared} bytes, above the ceiling.")
                return self._read_exactly(connection, declared, exchange_deadline, timeout_seconds)
        except TimeoutError as error:
            raise VerificationTimeoutError(
                VerificationTimeoutStage.RESPONSE,
                f"The verification server did not answer within {timeout_seconds}s.",
            ) from error
        except (ProtocolMismatchError, VerificationTimeoutError):
            raise
        except OSError as error:
            raise VerificationConnectionError(request_sent=request_sent) from error

    @staticmethod
    def _arm(connection: socket.socket, deadline: float, timeout_seconds: float) -> None:
        """Sets the socket timeout to whatever is left of the exchange deadline."""
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise VerificationTimeoutError(
                VerificationTimeoutStage.RESPONSE,
                f"The verification server did not answer within {timeout_seconds}s.",
            )
        connection.settimeout(remaining)

    def _read_exactly(
        self, connection: socket.socket, count: int, deadline: float, timeout_seconds: float
    ) -> bytes:
        chunks: list[bytes] = []
        remaining = count
        while remaining:
            self._arm(connection, deadline, timeout_seconds)
            chunk = connection.recv(remaining)
            if not chunk:
                raise ProtocolMismatchError(
                    f"The peer closed after {count - remaining} of {count} expected bytes."
                )
            chunks.append(chunk)
            remaining -= len(chunk)
        return b"".join(chunks)
