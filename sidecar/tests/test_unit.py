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
    EconomyCheck,
    ErrorCode,
    HoldActivityRequest,
    InspectActivityLeaseRequest,
    InspectRequest,
    InspectResult,
    ListRequest,
    ListResult,
    MapCheck,
    MoneyDecreaseCheck,
    ProfessionSkillCheck,
    RecoverRequest,
    RecoveryResult,
    ReleaseActivityRequest,
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

    @pytest.mark.parametrize(
        "kwargs",
        [
            {"bot_guid": 3, "skill_id": 186, "value": 76, "maximum": 75},
            {"bot_guid": 3, "skill_id": 186, "value": 1, "maximum": 80},
            {"bot_guid": 3, "skill_id": 186, "value": 1, "maximum": 525},
            {"bot_guid": 3, "skill_id": 0, "value": 1, "maximum": 75},
            {"bot_guid": 3, "skill_id": 186, "value": 0, "maximum": 75},
        ],
    )
    def test_a_skill_staging_request_the_server_would_refuse_is_refused_client_side(
        self, kwargs: dict[str, int]
    ) -> None:
        from playerbot_mcp.protocol import SetSkillRequest

        with pytest.raises(ValidationError):
            SetSkillRequest(**kwargs)

    def test_staging_requests_emit_the_exact_server_shapes(self) -> None:
        from playerbot_mcp.protocol import SetSkillRequest, TeleportToGameObjectRequest

        skill = json.loads(
            build_request_payload(
                SetSkillRequest(bot_guid=3, skill_id=186, value=75, maximum=75), request_id=6, token=TOKEN
            )
        )
        assert skill == {
            "schemaVersion": SCHEMA_VERSION,
            "requestId": 6,
            "token": TOKEN,
            "operation": "set_skill",
            "botGuid": 3,
            "skillId": 186,
            "value": 75,
            "maximum": 75,
        }
        teleport = json.loads(
            build_request_payload(
                TeleportToGameObjectRequest(bot_guid=3, game_object_entry=1731), request_id=7, token=TOKEN
            )
        )
        assert teleport == {
            "schemaVersion": SCHEMA_VERSION,
            "requestId": 7,
            "token": TOKEN,
            "operation": "teleport_to_gameobject",
            "botGuid": 3,
            "gameObjectEntry": 1731,
        }
        from playerbot_mcp.protocol import GmCommandRequest

        gm = json.loads(
            build_request_payload(GmCommandRequest(command=".tele name Bot brill"), request_id=8, token=TOKEN)
        )
        assert gm == {
            "schemaVersion": SCHEMA_VERSION,
            "requestId": 8,
            "token": TOKEN,
            "operation": "gm_command",
            "command": ".tele name Bot brill",
        }
        with pytest.raises(ValidationError):
            GmCommandRequest(command="")

    def test_activity_lease_requests_are_exact_bounded_and_token_owned(self) -> None:
        lease_token = "a" * 32
        acquire = json.loads(
            build_request_payload(
                HoldActivityRequest(bot_guid=3, duration_seconds=2400), request_id=9, token=TOKEN
            )
        )
        assert acquire == {
            "schemaVersion": SCHEMA_VERSION,
            "requestId": 9,
            "token": TOKEN,
            "operation": "hold_activity",
            "botGuid": 3,
            "durationSeconds": 2400,
        }

        renew = json.loads(
            build_request_payload(
                HoldActivityRequest(bot_guid=3, duration_seconds=600, lease_token=lease_token),
                request_id=10,
                token=TOKEN,
            )
        )
        assert renew["leaseToken"] == lease_token

        inspect = json.loads(
            build_request_payload(InspectActivityLeaseRequest(bot_guid=3), request_id=11, token=TOKEN)
        )
        assert inspect["operation"] == "inspect_activity_lease"
        assert set(inspect) == {"schemaVersion", "requestId", "token", "operation", "botGuid"}

        release = json.loads(
            build_request_payload(
                ReleaseActivityRequest(bot_guid=3, lease_token=lease_token), request_id=12, token=TOKEN
            )
        )
        assert release["operation"] == "release_activity"
        assert release["leaseToken"] == lease_token

        for duration in (0, 2701):
            with pytest.raises(ValidationError):
                HoldActivityRequest(bot_guid=3, duration_seconds=duration)
        with pytest.raises(ValidationError):
            HoldActivityRequest(bot_guid=3, duration_seconds=600, lease_token="ABC")
        with pytest.raises(ValidationError):
            ReleaseActivityRequest(bot_guid=3, lease_token="ABC")

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
                    "inspectionSchemaVersion": 6,
                    "moduleEnabled": "true",
                    "queueAvailable": True,
                    "queueSize": 0,
                    "botCount": 0,
                }
            )

    def test_status_rejects_an_old_inspection_schema(self) -> None:
        with pytest.raises(ValidationError):
            StatusResult.model_validate(
                {
                    "protocolSchemaVersion": 2,
                    "inspectionSchemaVersion": 4,
                    "moduleEnabled": True,
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

    def test_inspection_schema_mismatch_is_rejected(self) -> None:
        payload = inspection_payload()
        payload["schemaVersion"] = 4
        with pytest.raises(ValidationError):
            InspectResult.model_validate(payload)

    def test_idle_travel_is_explicit_and_keeps_independent_movement_capability(self) -> None:
        payload = inspection_payload()
        payload["movement"] = {"canMove": True}
        payload["travel"] |= {
            "available": True,
            "status": "cooldown",
            "idleNoDestination": True,
            "destination": None,
            "timeLeftMs": 120_000,
        }

        result = InspectResult.model_validate(payload)

        assert result.movement.can_move is True
        assert result.travel.idle_no_destination is True
        assert result.travel.destination is None

        payload["travel"]["timeLeftMs"] = 0
        assert InspectResult.model_validate(payload).travel.time_left_ms == 0

        payload["travel"]["idleNoDestination"] = False
        with pytest.raises(ValidationError):
            InspectResult.model_validate(payload)

        payload["travel"]["idleNoDestination"] = True
        payload["travel"]["status"] = "travel"
        with pytest.raises(ValidationError):
            InspectResult.model_validate(payload)

        payload["travel"]["status"] = "cooldown"
        payload["travel"]["timeLeftMs"] = None
        with pytest.raises(ValidationError):
            InspectResult.model_validate(payload)

        payload["travel"]["timeLeftMs"] = 300_001
        with pytest.raises(ValidationError):
            InspectResult.model_validate(payload)

    def test_live_null_cooldown_expiry_payload_is_a_valid_completed_idle_state(self) -> None:
        payload = inspection_payload()
        payload["travel"] |= {
            "available": True,
            "status": "none",
            "idleNoDestination": True,
            "destination": None,
            "timeLeftMs": 0,
        }

        result = InspectResult.model_validate(payload)

        assert result.travel.status == "none"
        assert result.travel.idle_no_destination is True
        assert result.travel.destination is None
        assert result.travel.time_left_ms == 0

    def test_live_expired_null_travel_payload_is_a_valid_terminal_idle_state(self) -> None:
        payload = inspection_payload()
        payload["travel"] |= {
            "available": True,
            "status": "expired",
            "idleNoDestination": True,
            "destination": None,
            "timeLeftMs": 0,
        }

        result = InspectResult.model_validate(payload)

        assert result.travel.status == "expired"
        assert result.travel.idle_no_destination is True
        assert result.travel.destination is None
        assert result.travel.time_left_ms == 0

    @pytest.mark.parametrize("status", ["travel", "work", "prepare"])
    def test_idle_travel_rejects_non_idle_statuses(self, status: str) -> None:
        payload = inspection_payload()
        payload["travel"] |= {
            "available": True,
            "status": status,
            "idleNoDestination": True,
            "destination": None,
            "timeLeftMs": 0,
        }

        with pytest.raises(ValidationError):
            InspectResult.model_validate(payload)

    @pytest.mark.parametrize("status", ["none", "expired"])
    @pytest.mark.parametrize("time_left_ms", [None, 1])
    def test_terminal_idle_travel_requires_exactly_zero_time(
        self, status: str, time_left_ms: int | None
    ) -> None:
        payload = inspection_payload()
        payload["travel"] |= {
            "available": True,
            "status": status,
            "idleNoDestination": True,
            "destination": None,
            "timeLeftMs": time_left_ms,
        }

        with pytest.raises(ValidationError):
            InspectResult.model_validate(payload)

    @pytest.mark.parametrize("status", ["none", "expired"])
    def test_idle_travel_rejects_a_destination_even_after_cooldown_expiry(self, status: str) -> None:
        payload = inspection_payload()
        payload["travel"] |= {
            "available": True,
            "status": status,
            "idleNoDestination": True,
            "timeLeftMs": 0,
        }

        with pytest.raises(ValidationError):
            InspectResult.model_validate(payload)

    def test_non_idle_none_state_still_requires_a_destination(self) -> None:
        payload = inspection_payload()
        payload["travel"] |= {
            "available": True,
            "status": "none",
            "idleNoDestination": False,
            "destination": None,
            "timeLeftMs": 0,
        }

        with pytest.raises(ValidationError):
            InspectResult.model_validate(payload)

    def test_waiting_out_a_reclaim_delay_is_not_a_failed_revive(self) -> None:
        """The whole point of the outcome field: an observer must tell waiting from failing."""
        payload = inspection_payload()
        revive = payload["recovery"]["latestRevive"]
        revive["outcome"] = "ineligible"
        revive["success"] = False
        revive["aliveAfter"] = False

        parsed = InspectResult.model_validate(payload).recovery.latest_revive
        assert parsed.outcome == "ineligible"
        assert parsed.success is False

        revive["outcome"] = "failed"
        failed = InspectResult.model_validate(payload).recovery.latest_revive
        assert failed.outcome == "failed"
        assert failed.success is False

        # Both are unsuccessful, so only the outcome separates a healthy corpse run from a stuck bot.
        assert parsed.outcome != failed.outcome

    def test_a_success_flag_disagreeing_with_the_outcome_is_refused(self) -> None:
        payload = inspection_payload()
        payload["recovery"]["latestRevive"]["outcome"] = "failed"
        with pytest.raises(ValidationError):
            InspectResult.model_validate(payload)

    def test_an_unknown_revive_outcome_is_refused_rather_than_coerced(self) -> None:
        payload = inspection_payload()
        payload["recovery"]["latestRevive"]["outcome"] = "pending"
        with pytest.raises(ValidationError):
            InspectResult.model_validate(payload)

    def test_recovery_requires_truthful_corpse_and_revive_details(self) -> None:
        payload = inspection_payload()
        payload["recovery"] = {
            "observedAtMs": 1_700_000_001_000,
            "currentDeathGeneration": 8,
            "alive": False,
            "ghost": True,
            "inArena": False,
            "corpse": {
                "present": True,
                "loaded": True,
                "mapId": 0,
                "distanceYards": 12.5,
                "sameMap": True,
                "withinReclaimRadius": True,
                "reclaimDelayRemainingSeconds": 0,
                "reclaimReady": True,
            },
            "latestRevive": {
                "available": True,
                "timestampMs": 1_700_000_000_000,
                "ageMs": 1_000,
                "attemptGeneration": 7,
                "currentCycle": False,
                "outcome": "failed",
                "success": False,
                "aliveAfter": False,
            },
        }

        result = InspectResult.model_validate(payload)

        assert result.recovery.corpse.reclaim_ready is True
        assert result.recovery.latest_revive.alive_after is False

        payload["recovery"]["corpse"]["reclaimReady"] = False
        with pytest.raises(ValidationError):
            InspectResult.model_validate(payload)

        payload["recovery"]["corpse"]["reclaimReady"] = True
        payload["recovery"]["corpse"]["reclaimDelayRemainingSeconds"] = 10
        with pytest.raises(ValidationError):
            InspectResult.model_validate(payload)

        payload["recovery"]["corpse"]["reclaimDelayRemainingSeconds"] = 0
        payload["recovery"]["alive"] = True
        with pytest.raises(ValidationError):
            InspectResult.model_validate(payload)

        payload = inspection_payload()
        payload["recovery"]["latestRevive"]["currentCycle"] = False
        with pytest.raises(ValidationError):
            InspectResult.model_validate(payload)

        payload = inspection_payload()
        payload["recovery"]["latestRevive"]["ageMs"] = 999
        with pytest.raises(ValidationError):
            InspectResult.model_validate(payload)

        payload = inspection_payload()
        payload["recovery"]["ghost"] = True
        with pytest.raises(ValidationError):
            InspectResult.model_validate(payload)

    def test_equipment_broken_state_must_match_durability(self) -> None:
        payload = inspection_payload()
        payload["equipment"]["items"][0] |= {
            "durability": 0,
            "maximumDurability": 80,
            "broken": True,
        }
        result = InspectResult.model_validate(payload)
        assert result.equipment.items[0].broken is True

        payload["equipment"]["items"][0]["broken"] = False
        with pytest.raises(ValidationError):
            InspectResult.model_validate(payload)

    def test_an_available_rpg_target_requires_complete_identity(self) -> None:
        payload = inspection_payload()
        payload["rpgTarget"]["guid"] = None
        with pytest.raises(ValidationError):
            InspectResult.model_validate(payload)

    def test_an_unavailable_rpg_target_requires_null_details(self) -> None:
        payload = inspection_payload()
        payload["rpgTarget"] = {
            "available": False,
            "type": None,
            "guid": None,
            "entry": None,
            "name": None,
            "npcFlags": None,
            "distanceYards": None,
            "moving": None,
        }
        result = InspectResult.model_validate(payload)
        assert result.rpg_target.available is False

        payload["rpgTarget"]["name"] = "unexpected"
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
        assert result.economy.chain_public_id == "chn_0123456789abcdef"
        assert result.economy.operation_identity == "buy_reagent:2589:20"
        assert result.economy.market_id == 7
        assert result.economy.item_family == "exact_reagent:2589"
        assert result.economy.remaining_quantity == 20
        assert result.economy.claim_age_seconds == 12
        assert result.economy.blocker_code == ""
        assert result.economy.cooldown_seconds == 60
        assert result.economy.quarantined is False

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
        "phase",
        [
            "buy_finished_good",
            "use_finished_good",
            "recover_finished_good",
            "gather",
            "market_making",
        ],
    )
    def test_every_extended_economy_phase_is_accepted(self, phase: str) -> None:
        payload = inspection_payload()
        payload["economy"]["phase"] = phase
        assert InspectResult.model_validate(payload).economy.phase == phase

    @pytest.mark.parametrize("outcome", ["released", "blocked", "quarantined"])
    def test_every_extended_economy_outcome_is_accepted(self, outcome: str) -> None:
        payload = inspection_payload()
        payload["economy"]["outcome"] = outcome
        assert InspectResult.model_validate(payload).economy.outcome == outcome

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
    async def test_exactly_the_fourteen_planned_tools_are_exposed(self) -> None:
        async with Client(self._server()) as client:
            listed = await client.list_tools()
        assert {tool.name for tool in listed.tools} == {
            "server_status",
            "list_bots",
            "inspect_bot",
            "inspect_bot_loops",
            "inspect_bot_activity_lease",
            "wait_for_bot",
            "send_bot_command",
            "set_bot_skill",
            "teleport_bot_to_gameobject",
            "run_gm_command",
            "reload_config",
            "hold_bot_activity",
            "release_bot_activity",
            "recover_bot",
        }

    @pytest.mark.anyio
    async def test_only_the_six_observation_tools_are_read_only(self) -> None:
        """Codex gates write approval on readOnlyHint, so the six read tools must carry it."""
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
            "inspect_bot_activity_lease",
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

    @pytest.mark.anyio
    async def test_activity_release_is_mutating_non_destructive_and_idempotent(self) -> None:
        async with Client(self._server()) as client:
            listed = await client.list_tools()
        release = next(tool for tool in listed.tools if tool.name == "release_bot_activity")
        assert release.annotations is not None
        assert release.annotations.read_only_hint is False
        assert release.annotations.destructive_hint is False
        assert release.annotations.idempotent_hint is True


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

    @pytest.mark.parametrize("outcome", ["released", "blocked", "quarantined"])
    def test_every_emitted_economy_terminal_outcome_can_be_awaited(self, outcome: str) -> None:
        built = build_check(
            "economy",
            bot_guid=3,
            after_sequence=88,
            economy_outcome=outcome,
        )
        assert isinstance(built, EconomyCheck)
        assert built.economy_outcome == outcome
