"""Model and framing tests. These never open a socket."""

from __future__ import annotations

import json

import pytest
from mcp import Client
from mcp.server import MCPServer
from pydantic import SecretStr, ValidationError

from conftest import (
    PORT,
    TOKEN,
    action_attempt,
    completeness,
    environment,
    error_envelope,
    inspection_payload,
    latest_attempt,
)
from playerbot_mcp.client import (
    ConfigurationError,
    ProtocolMismatchError,
    ServerError,
    VerificationClient,
    VerificationSettings,
)
from playerbot_mcp.protocol import (
    CHECK_MODELS,
    MAX_ANOMALY_LIMIT,
    MAX_FRAME_PAYLOAD_BYTES,
    MAX_LIST_LIMIT,
    SCHEMA_VERSION,
    ActionCheck,
    AnomaliesRequest,
    CommandRequest,
    ErrorCode,
    InspectRequest,
    InspectResult,
    ListRequest,
    ListResult,
    MapCheck,
    MoneyDecreaseCheck,
    ProfessionSkillCheck,
    RecoverRequest,
    RecoveryResult,
    StatusRequest,
    StatusResult,
    build_check,
    build_request_payload,
    decode_frame,
    encode_frame,
    parse_envelope,
)
from playerbot_mcp.server import build_server

pytestmark = pytest.mark.unit


class TestFraming:
    def test_round_trip_preserves_payload_bytes(self) -> None:
        payload = b'{"ok":true}'
        frame = encode_frame(payload)
        assert frame[:4] == len(payload).to_bytes(4, "big")
        assert decode_frame(frame) == payload

    def test_payload_above_the_ceiling_is_refused_before_it_reaches_the_socket(self) -> None:
        with pytest.raises(ProtocolMismatchError):
            encode_frame(b"x" * (MAX_FRAME_PAYLOAD_BYTES + 1))

    def test_declared_length_that_disagrees_with_the_body_is_rejected(self) -> None:
        body = b'{"ok":true}'
        with pytest.raises(ProtocolMismatchError):
            decode_frame((len(body) + 5).to_bytes(4, "big") + body)


class TestSettings:
    @pytest.mark.parametrize(
        "overrides",
        [
            {"PLAYERBOT_VERIFICATION_PORT": None},
            {"PLAYERBOT_VERIFICATION_TOKEN": None},
        ],
    )
    def test_a_missing_variable_is_a_configuration_error(self, overrides: dict[str, None]) -> None:
        with pytest.raises(ConfigurationError):
            VerificationSettings.from_environment(environment(**overrides))

    @pytest.mark.parametrize("port", ["0", "65536", "not-a-port", "-1"])
    def test_a_port_outside_the_valid_range_is_a_configuration_error(self, port: str) -> None:
        with pytest.raises(ConfigurationError):
            VerificationSettings.from_environment(environment(PLAYERBOT_VERIFICATION_PORT=port))

    def test_a_token_below_the_server_minimum_is_a_configuration_error(self) -> None:
        with pytest.raises(ConfigurationError):
            VerificationSettings.from_environment(environment(PLAYERBOT_VERIFICATION_TOKEN="short"))

    def test_the_host_is_fixed_to_loopback(self) -> None:
        settings = VerificationSettings.from_environment(environment())
        assert settings.host == "127.0.0.1"
        assert settings.port == int(PORT)

    def test_a_caller_cannot_point_the_client_at_another_host(self) -> None:
        """The token is only safe because it never leaves loopback, so host must be unreachable."""
        with pytest.raises(ValidationError):
            VerificationSettings(port=48765, token=SecretStr(TOKEN), host="203.0.113.9")  # type: ignore[call-arg]

    def test_the_host_cannot_be_reassigned_after_construction(self) -> None:
        settings = VerificationSettings.from_environment(environment())
        with pytest.raises((ValidationError, AttributeError)):
            settings.host = "203.0.113.9"  # type: ignore[misc]
        assert settings.host == "127.0.0.1"

    def test_a_host_variable_in_the_environment_is_ignored(self) -> None:
        settings = VerificationSettings.from_environment(
            environment(PLAYERBOT_VERIFICATION_HOST="203.0.113.9")
        )
        assert settings.host == "127.0.0.1"


class TestTokenSecrecy:
    def test_the_token_never_appears_in_settings_output(self) -> None:
        settings = VerificationSettings.from_environment(environment())
        assert TOKEN not in repr(settings)
        assert TOKEN not in str(settings)
        assert TOKEN not in json.dumps(settings.model_dump(mode="json"))

    def test_the_token_never_appears_in_a_request_model(self) -> None:
        request = CommandRequest(bot_guid=3, master_guid=1, command="follow")
        assert TOKEN not in repr(request)
        assert TOKEN not in json.dumps(request.model_dump(mode="json", by_alias=True))

    def test_the_token_never_appears_in_a_server_error(self) -> None:
        error = ServerError(ErrorCode.AUTHENTICATION_FAILED, "Authentication failed.")
        assert TOKEN not in repr(error)
        assert TOKEN not in str(error)

    def test_the_token_is_still_carried_on_the_wire(self) -> None:
        payload = build_request_payload(StatusRequest(), request_id=1, token=TOKEN)
        assert json.loads(payload)["token"] == TOKEN


class TestRequestPayloads:
    def test_every_request_carries_the_envelope_the_server_requires(self) -> None:
        payload = json.loads(build_request_payload(InspectRequest(bot_guid=3), request_id=9, token=TOKEN))
        assert payload["schemaVersion"] == SCHEMA_VERSION
        assert payload["requestId"] == 9
        assert payload["operation"] == "inspect"
        assert payload["botGuid"] == 3

    def test_anomaly_requests_are_serverwide_and_bounded(self) -> None:
        payload = json.loads(
            build_request_payload(AnomaliesRequest(limit=MAX_ANOMALY_LIMIT), request_id=10, token=TOKEN)
        )
        assert payload == {
            "schemaVersion": SCHEMA_VERSION,
            "requestId": 10,
            "token": TOKEN,
            "operation": "anomalies",
            "limit": MAX_ANOMALY_LIMIT,
        }

    def test_a_check_request_emits_exactly_the_fields_its_condition_requires(self) -> None:
        payload = json.loads(
            build_request_payload(
                ActionCheck(bot_guid=3, after_sequence=7, action_name="melee", action_result="success"),
                request_id=2,
                token=TOKEN,
            )
        )
        assert payload.keys() == {
            "schemaVersion",
            "requestId",
            "token",
            "operation",
            "botGuid",
            "condition",
            "afterSequence",
            "actionName",
            "actionResult",
        }
        assert payload["condition"] == "action"

    def test_an_optional_condition_field_is_omitted_rather_than_sent_as_null(self) -> None:
        from playerbot_mcp.protocol import TransportAttachedCheck

        payload = json.loads(
            build_request_payload(TransportAttachedCheck(bot_guid=3), request_id=3, token=TOKEN)
        )
        assert "transportEntry" not in payload

    @pytest.mark.parametrize("limit", [0, MAX_LIST_LIMIT + 1])
    def test_a_list_limit_the_server_would_reject_is_refused_client_side(self, limit: int) -> None:
        with pytest.raises(ValidationError):
            ListRequest(after_guid=0, limit=limit)

    @pytest.mark.parametrize(
        ("model", "kwargs"),
        [
            (InspectRequest, {"bot_guid": 0}),
            (CommandRequest, {"bot_guid": 3, "master_guid": 0, "command": "follow"}),
            (CommandRequest, {"bot_guid": 3, "master_guid": 1, "command": ""}),
            (ProfessionSkillCheck, {"bot_guid": 3, "skill_id": 0, "minimum_value": 1}),
        ],
    )
    def test_values_the_server_would_reject_are_refused_client_side(
        self, model: type, kwargs: dict[str, object]
    ) -> None:
        with pytest.raises(ValidationError):
            model(**kwargs)

    def test_a_map_check_accepts_map_zero_because_the_server_does(self) -> None:
        payload = json.loads(build_request_payload(MapCheck(bot_guid=3, map_id=0), request_id=4, token=TOKEN))
        assert payload["mapId"] == 0

    def test_recovery_emits_only_the_closed_homebind_request(self) -> None:
        payload = json.loads(
            build_request_payload(
                RecoverRequest(bot_guid=3, destination="homebind"), request_id=5, token=TOKEN
            )
        )
        assert payload == {
            "schemaVersion": SCHEMA_VERSION,
            "requestId": 5,
            "token": TOKEN,
            "operation": "recover",
            "botGuid": 3,
            "destination": "homebind",
        }

    @pytest.mark.parametrize(
        ("field", "value"),
        [
            ("command", "tele home"),
            ("mapId", 0),
            ("x", 1.0),
            ("y", 2.0),
            ("z", 3.0),
            ("orientation", 4.0),
            ("name", "RecoveryBot"),
            ("accountId", 7),
        ],
    )
    def test_recovery_rejects_every_arbitrary_target_or_command_field(
        self, field: str, value: object
    ) -> None:
        with pytest.raises(ValidationError):
            RecoverRequest.model_validate({"botGuid": 3, "destination": "homebind", field: value})

    def test_recovery_rejects_any_protocol_destination_other_than_homebind(self) -> None:
        with pytest.raises(ValidationError):
            RecoverRequest.model_validate({"botGuid": 3, "destination": "somewhere"})


class TestStrictModels:
    def test_an_unknown_field_is_rejected_rather_than_ignored(self) -> None:
        with pytest.raises(ValidationError):
            InspectResult.model_validate(inspection_payload(unexpectedField=1))

    def test_a_wrong_type_is_rejected_rather_than_coerced(self) -> None:
        with pytest.raises(ValidationError):
            StatusResult.model_validate(
                {
                    "protocolSchemaVersion": 2,
                    "inspectionSchemaVersion": 2,
                    "moduleEnabled": "true",
                    "queueAvailable": True,
                    "queueSize": 0,
                    "botCount": 0,
                }
            )

    def test_a_missing_field_is_rejected_rather_than_defaulted(self) -> None:
        payload = inspection_payload()
        del payload["finance"]
        with pytest.raises(ValidationError):
            InspectResult.model_validate(payload)

    def test_recovery_result_preserves_closed_outcome_and_truth_fields(self) -> None:
        result = RecoveryResult.model_validate(
            {
                "timestampMs": 1_700_000_000_000,
                "operation": "recover",
                "requestId": 5,
                "botGuid": 3,
                "botName": "RecoveryBot",
                "destination": "homebind",
                "beforePosition": {
                    "mapId": 0,
                    "zoneId": 12,
                    "areaId": 12,
                    "x": 100.0,
                    "y": 110.0,
                    "z": 120.0,
                    "orientation": 1.0,
                },
                "acceptedDestination": {
                    "mapId": 0,
                    "zoneId": 12,
                    "areaId": 12,
                    "x": 20.0,
                    "y": 30.0,
                    "z": 40.0,
                    "orientation": 1.0,
                },
                "observedPosition": {
                    "mapId": 0,
                    "zoneId": 12,
                    "areaId": 12,
                    "x": 100.0,
                    "y": 110.0,
                    "z": 120.0,
                    "orientation": 1.0,
                },
                "observedAtDestination": False,
                "movementReset": True,
                "travelReset": True,
                "taxiReset": True,
                "outcome": "recovered",
                "reason": "homebind_teleport_accepted",
                "mutationState": "completed",
                "persistenceState": "deferred",
            }
        )
        assert result.outcome == "recovered"
        assert result.accepted_destination is not None
        assert result.observed_at_destination is False


class TestValuePreservation:
    def test_exact_copper_values_survive_parsing(self) -> None:
        result = InspectResult.model_validate(inspection_payload())
        assert result.finance.money_copper == 123_456_789
        assert result.finance.free_tradeskill_copper == 9_000
        assert result.finance.free_spells_copper == 4_500

    def test_action_and_economy_sequences_survive_parsing(self) -> None:
        result = InspectResult.model_validate(inspection_payload())
        assert [attempt.sequence for attempt in result.action.attempts] == [6, 7]
        assert result.action.latest_attempt.sequence == 7
        assert result.action.completeness.total_count == 7
        assert result.economy.sequence == 88

    def test_recipe_ids_keep_the_order_the_server_sorted_them_into(self) -> None:
        result = InspectResult.model_validate(inspection_payload())
        assert result.known_recipe_spell_ids.items == [2018, 3100, 3538, 9785]
        assert result.known_recipe_spell_ids.items == sorted(result.known_recipe_spell_ids.items)

    def test_career_and_economy_enums_are_typed_rather_than_free_strings(self) -> None:
        result = InspectResult.model_validate(inspection_payload())
        assert result.career.status == "valid"
        assert result.career.spending_style == "progression"
        assert result.career.source == "loaded"
        assert result.economy.outcome == "operation"
        assert result.economy.phase == "buy_reagent"

    @pytest.mark.parametrize(
        ("section", "field", "value"),
        [
            ("career", "status", "retired"),
            ("economy", "outcome", "cancelled"),
            ("economy", "phase", "napping"),
        ],
    )
    def test_an_enum_value_the_server_cannot_emit_is_rejected(
        self, section: str, field: str, value: str
    ) -> None:
        payload = inspection_payload()
        payload[section][field] = value
        with pytest.raises(ValidationError):
            InspectResult.model_validate(payload)

    def test_a_truncated_action_name_stays_flagged_after_parsing(self) -> None:
        payload = inspection_payload()
        payload["action"]["attempts"] = [action_attempt(6, "a" * 127, truncated=True)]
        payload["action"]["latestAttempt"] = latest_attempt(6, "a" * 127, truncated=True)
        result = InspectResult.model_validate(payload)
        assert result.action.latest_attempt.name_truncated is True


class TestCompleteness:
    def test_collection_metadata_survives_parsing(self) -> None:
        payload = inspection_payload()
        payload["inventory"]["completeness"] = completeness(210, 64, True)
        result = InspectResult.model_validate(payload)
        assert result.inventory.completeness.total_count == 210
        assert result.inventory.completeness.returned_count == 64
        assert result.inventory.completeness.truncated is True

    def test_pagination_metadata_survives_parsing(self) -> None:
        result = ListResult.model_validate(
            {
                "bots": [],
                "nextAfterGuid": 41,
                "hasMore": True,
                "completeness": completeness(300, 0, True),
            }
        )
        assert result.next_after_guid == 41
        assert result.has_more is True
        assert result.completeness.total_count == 300


class TestEnvelopes:
    def test_a_failure_envelope_becomes_a_typed_error(self) -> None:
        envelope = parse_envelope(json.dumps(error_envelope(5, "bot_not_found")).encode(), expected_id=5)
        assert envelope.ok is False
        assert envelope.error is not None
        assert envelope.error.code is ErrorCode.BOT_NOT_FOUND

    def test_response_too_large_is_an_error_and_never_a_partial_result(self) -> None:
        envelope = parse_envelope(json.dumps(error_envelope(6, "response_too_large")).encode(), expected_id=6)
        assert envelope.ok is False
        assert envelope.result is None
        assert envelope.error is not None
        assert envelope.error.code is ErrorCode.RESPONSE_TOO_LARGE

    def test_an_unknown_error_code_is_a_protocol_mismatch_rather_than_a_silent_pass(self) -> None:
        with pytest.raises(ProtocolMismatchError):
            parse_envelope(json.dumps(error_envelope(7, "brand_new_code")).encode(), expected_id=7)

    def test_a_mismatched_request_id_is_refused(self) -> None:
        payload = json.dumps(error_envelope(8, "timeout")).encode()
        with pytest.raises(ProtocolMismatchError):
            parse_envelope(payload, expected_id=9)

    def test_a_mismatched_schema_version_is_refused(self) -> None:
        payload = json.dumps({"schemaVersion": 99, "requestId": 1, "ok": True, "result": {}}).encode()
        with pytest.raises(ProtocolMismatchError):
            parse_envelope(payload, expected_id=1)

    def test_a_body_that_is_not_json_is_refused(self) -> None:
        with pytest.raises(ProtocolMismatchError):
            parse_envelope(b"not json at all", expected_id=1)


class TestToolSurface:
    """The tool surface is a contract with Codex: names, annotations, and input validation."""

    @staticmethod
    def _server() -> MCPServer:
        # Listing tools and validating inputs never opens a socket, so the port is never dialled.
        settings = VerificationSettings(port=1, token=SecretStr(TOKEN))
        return build_server(VerificationClient(settings))

    @pytest.mark.anyio
    async def test_exactly_the_seven_planned_tools_are_exposed(self) -> None:
        async with Client(self._server()) as client:
            listed = await client.list_tools()
        assert {tool.name for tool in listed.tools} == {
            "server_status",
            "list_bots",
            "inspect_bot",
            "inspect_bot_loops",
            "wait_for_bot",
            "send_bot_command",
            "recover_bot",
        }

    @pytest.mark.anyio
    async def test_only_the_five_observation_tools_are_read_only(self) -> None:
        """Codex gates write approval on readOnlyHint, so the five read tools must carry it."""
        async with Client(self._server()) as client:
            listed = await client.list_tools()
        read_only = {
            tool.name for tool in listed.tools if tool.annotations and tool.annotations.read_only_hint
        }
        assert read_only == {
            "server_status",
            "list_bots",
            "inspect_bot",
            "inspect_bot_loops",
            "wait_for_bot",
        }

    @pytest.mark.anyio
    async def test_the_mutating_tool_is_not_marked_destructive_or_idempotent(self) -> None:
        async with Client(self._server()) as client:
            listed = await client.list_tools()
        command = next(tool for tool in listed.tools if tool.name == "send_bot_command")
        assert command.annotations is not None
        assert command.annotations.read_only_hint is False
        assert command.annotations.destructive_hint is False
        assert command.annotations.idempotent_hint is False

    @pytest.mark.anyio
    async def test_recovery_is_mutating_non_destructive_and_idempotent(self) -> None:
        async with Client(self._server()) as client:
            listed = await client.list_tools()
        recovery = next(tool for tool in listed.tools if tool.name == "recover_bot")
        assert recovery.annotations is not None
        assert recovery.annotations.read_only_hint is False
        assert recovery.annotations.destructive_hint is False
        assert recovery.annotations.idempotent_hint is True


class TestConditionBuilding:
    def test_each_condition_maps_to_its_own_request_model(self) -> None:
        assert set(CHECK_MODELS) == {
            "transport_attached",
            "transport_detached",
            "map",
            "action",
            "profession_skill",
            "inventory",
            "money_at_most",
            "money_decrease",
            "known_recipe",
            "economy",
        }

    def test_an_unknown_condition_is_refused(self) -> None:
        with pytest.raises(ValueError, match="condition"):
            build_check("teleported", bot_guid=3)

    def test_a_condition_missing_a_required_field_is_refused(self) -> None:
        with pytest.raises(ValidationError):
            build_check("action", bot_guid=3, after_sequence=1, action_name="melee")

    def test_a_field_belonging_to_another_condition_is_refused(self) -> None:
        with pytest.raises(ValidationError):
            build_check("map", bot_guid=3, map_id=571, action_name="melee")

    def test_a_valid_condition_builds_the_matching_model(self) -> None:
        built = build_check("money_decrease", bot_guid=3, baseline_copper=500)
        assert isinstance(built, MoneyDecreaseCheck)
        assert built.baseline_copper == 500
