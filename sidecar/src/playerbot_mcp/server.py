"""MCP tool surface over the verification client.

Five read tools carry the read only annotation so Codex can call them without a write prompt.
`send_bot_command`, `set_bot_skill`, `teleport_bot_to_gameobject`, and `recover_bot` are mutating.
Recovery is idempotent and is limited to one exact online managed Playerbot's authoritative homebind.
The two staging tools exist for verification scenarios that would otherwise wait on the game clock.

The server holds no game rules. It validates inputs, forwards them, and reports what the
authoritative C++ side said. A dispatch result means the bridge sent the command, never that
the bot accepted it.
"""

from __future__ import annotations

import time
from functools import partial
from typing import Any, Literal

import anyio
import anyio.to_thread
from mcp.server import MCPServer
from mcp.types import ToolAnnotations
from pydantic import BaseModel, ConfigDict, ValidationError
from pydantic.alias_generators import to_camel

from playerbot_mcp.client import VerificationClient, VerificationSettings
from playerbot_mcp.protocol import (
    MAX_ANOMALY_LIMIT,
    MAX_LIST_LIMIT,
    UINT32_MAX,
    AnomaliesResult,
    CommandRequest,
    CommandResult,
    ConfigurationError,
    ErrorCode,
    InspectResult,
    ListResult,
    ProtocolMismatchError,
    RecoveryMutationState,
    RecoveryOutcome,
    RecoveryPersistenceState,
    RecoveryReason,
    RecoveryResult,
    ServerError,
    SetSkillRequest,
    SetSkillResult,
    StatusResult,
    GmCommandRequest,
    GmCommandResult,
    TeleportToGameObjectRequest,
    TeleportToGameObjectResult,
    VerificationConnectionError,
    VerificationError,
    VerificationTimeoutError,
    VerificationTimeoutStage,
    build_check,
)

SERVER_NAME = "playerbot-verification"

WAIT_POLL_INTERVAL_SECONDS = 0.1
WAIT_MIN_TIMEOUT_SECONDS = 1.0
WAIT_MAX_TIMEOUT_SECONDS = 60.0
MAX_WAIT_TRANSITIONS = 64


def _player_guid_low(guid: str) -> int:
    """Return the low part from either a raw hex GUID or AzerothCore's display form."""
    display_prefix, separator, low = guid.rpartition(" Type: Player Low: ")
    if separator and display_prefix.startswith("GUID Full: 0x"):
        return int(low)
    return int(guid, 16)


# Reads never change game state and can be repeated safely. open_world_hint is false because the
# only reachable peer is the loopback verification server.
READ_ONLY = ToolAnnotations(
    read_only_hint=True, destructive_hint=False, idempotent_hint=True, open_world_hint=False
)

# A command is not destructive, but it is also not idempotent: two dispatches are two whispers.
MUTATING = ToolAnnotations(
    read_only_hint=False, destructive_hint=False, idempotent_hint=False, open_world_hint=False
)

IDEMPOTENT_MUTATING = ToolAnnotations(
    read_only_hint=False, destructive_hint=False, idempotent_hint=True, open_world_hint=False
)

SERVER_RECOVERY_FAILURES: dict[ErrorCode, tuple[RecoveryOutcome, RecoveryReason, RecoveryMutationState]] = {
    ErrorCode.MALFORMED_REQUEST: (
        RecoveryOutcome.INVALID_REQUEST,
        RecoveryReason.MALFORMED_REQUEST,
        RecoveryMutationState.NOT_STARTED,
    ),
    ErrorCode.INVALID_GUID: (
        RecoveryOutcome.INVALID_REQUEST,
        RecoveryReason.INVALID_GUID,
        RecoveryMutationState.NOT_STARTED,
    ),
    ErrorCode.UNSUPPORTED_SCHEMA_VERSION: (
        RecoveryOutcome.INVALID_REQUEST,
        RecoveryReason.UNSUPPORTED_SCHEMA,
        RecoveryMutationState.NOT_STARTED,
    ),
    ErrorCode.UNKNOWN_OPERATION: (
        RecoveryOutcome.INVALID_REQUEST,
        RecoveryReason.UNKNOWN_OPERATION,
        RecoveryMutationState.NOT_STARTED,
    ),
    ErrorCode.AUTHENTICATION_FAILED: (
        RecoveryOutcome.UNAUTHORIZED,
        RecoveryReason.AUTHENTICATION_FAILED,
        RecoveryMutationState.NOT_STARTED,
    ),
    ErrorCode.BOT_NOT_FOUND: (
        RecoveryOutcome.BOT_NOT_FOUND,
        RecoveryReason.CHARACTER_NOT_FOUND,
        RecoveryMutationState.NOT_STARTED,
    ),
    ErrorCode.BOT_UNAVAILABLE: (
        RecoveryOutcome.BOT_NOT_AVAILABLE,
        RecoveryReason.CHARACTER_OFFLINE,
        RecoveryMutationState.NOT_STARTED,
    ),
    ErrorCode.NOT_MANAGED_PLAYERBOT: (
        RecoveryOutcome.NOT_MANAGED_PLAYERBOT,
        RecoveryReason.PLAYERBOT_AI_MISSING,
        RecoveryMutationState.NOT_STARTED,
    ),
    ErrorCode.UNSUPPORTED_DESTINATION: (
        RecoveryOutcome.UNSUPPORTED_DESTINATION,
        RecoveryReason.DESTINATION_NOT_HOMEBIND,
        RecoveryMutationState.NOT_STARTED,
    ),
    ErrorCode.OPERATION_UNAVAILABLE: (
        RecoveryOutcome.RECOVERY_FAILED,
        RecoveryReason.OPERATION_UNAVAILABLE,
        RecoveryMutationState.NOT_STARTED,
    ),
    ErrorCode.QUEUE_FULL: (
        RecoveryOutcome.RECOVERY_FAILED,
        RecoveryReason.QUEUE_FULL,
        RecoveryMutationState.NOT_STARTED,
    ),
    ErrorCode.SHUTDOWN: (
        RecoveryOutcome.RECOVERY_FAILED,
        RecoveryReason.SHUTTING_DOWN,
        RecoveryMutationState.NOT_STARTED,
    ),
    ErrorCode.RESPONSE_TOO_LARGE: (
        RecoveryOutcome.RECOVERY_FAILED,
        RecoveryReason.RESPONSE_TOO_LARGE,
        RecoveryMutationState.UNKNOWN_AFTER_EXECUTION_STARTED,
    ),
    ErrorCode.TIMEOUT: (
        RecoveryOutcome.RECOVERY_TIMED_OUT,
        RecoveryReason.QUEUE_TIMEOUT_BEFORE_CLAIM,
        RecoveryMutationState.NOT_STARTED,
    ),
    ErrorCode.RECOVERY_FAILED: (
        RecoveryOutcome.RECOVERY_FAILED,
        RecoveryReason.INTERNAL_ERROR,
        RecoveryMutationState.UNKNOWN_AFTER_EXECUTION_STARTED,
    ),
    ErrorCode.INTERNAL_ERROR: (
        RecoveryOutcome.RECOVERY_FAILED,
        RecoveryReason.INTERNAL_ERROR,
        RecoveryMutationState.UNKNOWN_AFTER_EXECUTION_STARTED,
    ),
}


def _recovery_result(
    *,
    bot_guid: int | None,
    destination: Literal["missing", "homebind", "unsupported"],
    outcome: RecoveryOutcome,
    reason: RecoveryReason,
    mutation_state: RecoveryMutationState = RecoveryMutationState.NOT_STARTED,
) -> RecoveryResult:
    """Build one bounded adapter-owned result without copying exception or caller text."""
    return RecoveryResult(
        timestamp_ms=time.time_ns() // 1_000_000,
        operation="recover",
        bot_guid=bot_guid,
        destination=destination,
        observed_at_destination=False,
        movement_reset=False,
        travel_reset=False,
        taxi_reset=False,
        outcome=outcome,
        reason=reason,
        mutation_state=mutation_state,
        persistence_state=RecoveryPersistenceState.NOT_REQUESTED,
    )


def _server_recovery_error(bot_guid: int, error: ServerError) -> RecoveryResult:
    outcome, reason, mutation = SERVER_RECOVERY_FAILURES.get(
        error.code,
        (
            RecoveryOutcome.RECOVERY_FAILED,
            RecoveryReason.INTERNAL_ERROR,
            RecoveryMutationState.UNKNOWN_AFTER_EXECUTION_STARTED,
        ),
    )
    return _recovery_result(
        bot_guid=bot_guid,
        destination="homebind",
        outcome=outcome,
        reason=reason,
        mutation_state=mutation,
    )


class WaitTransition(BaseModel):
    """One deduplicated observed change, so a report can separate cause from coincidence."""

    model_config = ConfigDict(alias_generator=to_camel, populate_by_name=True, frozen=True)

    at_seconds: float
    kind: str
    detail: str


class WaitResult(BaseModel):
    model_config = ConfigDict(alias_generator=to_camel, populate_by_name=True, frozen=True)

    matched: bool
    timed_out: bool
    condition: str
    elapsed_seconds: float
    polls: int
    transitions: list[WaitTransition]
    transitions_truncated: bool
    snapshot: InspectResult


def observe_transitions(before: InspectResult, after: InspectResult) -> list[tuple[str, str]]:
    """Reports what changed between two snapshots.

    Collections are compared through their completeness totals rather than their displayed
    items, because display caps would otherwise hide a change the server did observe.
    """
    changes: list[tuple[str, str]] = []

    latest = after.action.latest_attempt
    if latest.available and latest.sequence != before.action.latest_attempt.sequence:
        name = f"{latest.action_name} (truncated)" if latest.name_truncated else latest.action_name
        outcome = "success" if latest.success else "failure"
        changes.append(("action", f"{name} {outcome} sequence={latest.sequence}"))

    if after.economy.sequence != before.economy.sequence:
        changes.append(
            (
                "economy",
                f"{after.economy.outcome} phase={after.economy.phase} sequence={after.economy.sequence}",
            )
        )

    if after.finance.money_copper != before.finance.money_copper:
        changes.append(
            (
                "finance",
                f"money {before.finance.money_copper} to {after.finance.money_copper} copper",
            )
        )

    if after.position.map_id != before.position.map_id:
        changes.append(("position", f"map {before.position.map_id} to {after.position.map_id}"))

    if after.transport.attached != before.transport.attached:
        changes.append(("transport", "attached" if after.transport.attached else "detached"))

    if (after.career.status, after.career.version) != (before.career.status, before.career.version):
        changes.append(("career", f"{after.career.status} version={after.career.version}"))

    recipes_before = before.known_recipe_spell_ids.completeness.total_count
    recipes_after = after.known_recipe_spell_ids.completeness.total_count
    if recipes_after != recipes_before:
        changes.append(("recipe", f"known recipes {recipes_before} to {recipes_after}"))

    return changes


def build_server(client: VerificationClient) -> MCPServer:
    """Builds the tool surface over one client. The client is injected so tests can supply theirs."""
    server = MCPServer(
        SERVER_NAME,
        instructions=(
            "Read only tools report authoritative Playerbot state from the running worldserver. "
            "send_bot_command dispatches a whisper through the normal command path and reports "
            "dispatch only, never acceptance. recover_bot can return one exact online managed "
            "Playerbot only to its authoritative homebind. Observe effects through wait_for_bot "
            "or inspect_bot. inspect_bot_loops reports bounded anomalies across every online bot."
        ),
    )

    @server.tool(
        annotations=READ_ONLY,
        description="Report verification server readiness, queue depth, and the online bot count.",
    )
    async def server_status() -> StatusResult:
        return await anyio.to_thread.run_sync(client.status)

    @server.tool(
        annotations=READ_ONLY,
        description="List online bots in GUID order with their master relationship and map.",
    )
    async def list_bots(after_guid: int = 0, limit: int = MAX_LIST_LIMIT) -> ListResult:
        return await anyio.to_thread.run_sync(partial(client.list_bots, after_guid=after_guid, limit=limit))

    @server.tool(
        annotations=READ_ONLY,
        description="Return the full verification snapshot for one bot.",
    )
    async def inspect_bot(bot_guid: int) -> InspectResult:
        return await anyio.to_thread.run_sync(partial(client.inspect, bot_guid=bot_guid))

    @server.tool(
        annotations=READ_ONLY,
        description=(
            "Inspect every online managed Playerbot for movement, action, death, and recovery "
            "loops. The complete scan reports bounded anomaly records and explicit truncation."
        ),
    )
    async def inspect_bot_loops(limit: int = MAX_ANOMALY_LIMIT) -> AnomaliesResult:
        return await anyio.to_thread.run_sync(partial(client.anomalies, limit=limit))

    @server.tool(
        annotations=READ_ONLY,
        description=(
            "Poll one exact condition until it matches or the timeout expires. Returns the "
            "deduplicated transitions observed while waiting and the final snapshot."
        ),
    )
    async def wait_for_bot(
        bot_guid: int,
        condition: str,
        timeout_seconds: float = 10.0,
        transport_entry: int | None = None,
        map_id: int | None = None,
        after_sequence: int | None = None,
        action_name: str | None = None,
        action_result: str | None = None,
        skill_id: int | None = None,
        minimum_value: int | None = None,
        item_id: int | None = None,
        minimum_count: int | None = None,
        maximum_copper: int | None = None,
        baseline_copper: int | None = None,
        spell_id: int | None = None,
        economy_outcome: str | None = None,
    ) -> WaitResult:
        if not WAIT_MIN_TIMEOUT_SECONDS <= timeout_seconds <= WAIT_MAX_TIMEOUT_SECONDS:
            raise ValueError(
                f"timeout_seconds must be from {WAIT_MIN_TIMEOUT_SECONDS} through {WAIT_MAX_TIMEOUT_SECONDS}."
            )

        # Building the check first means a malformed condition never reaches the server.
        check = build_check(
            condition,
            bot_guid=bot_guid,
            transport_entry=transport_entry,
            map_id=map_id,
            after_sequence=after_sequence,
            action_name=action_name,
            action_result=action_result,
            skill_id=skill_id,
            minimum_value=minimum_value,
            item_id=item_id,
            minimum_count=minimum_count,
            maximum_copper=maximum_copper,
            baseline_copper=baseline_copper,
            spell_id=spell_id,
            economy_outcome=economy_outcome,
        )

        started = time.monotonic()
        deadline = started + timeout_seconds
        seen: set[tuple[str, str]] = set()
        transitions: list[WaitTransition] = []
        truncated = False
        previous: InspectResult | None = None
        polls = 0

        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0 and previous is not None:
                return WaitResult(
                    matched=False,
                    timed_out=True,
                    condition=condition,
                    elapsed_seconds=round(time.monotonic() - started, 3),
                    polls=polls,
                    transitions=transitions,
                    transitions_truncated=truncated,
                    snapshot=previous,
                )
            observed = await anyio.to_thread.run_sync(
                partial(client.check, check, timeout=remaining), abandon_on_cancel=True
            )
            polls += 1

            if previous is not None:
                for kind, detail in observe_transitions(previous, observed.snapshot):
                    if (kind, detail) in seen:
                        continue
                    seen.add((kind, detail))
                    if len(transitions) >= MAX_WAIT_TRANSITIONS:
                        truncated = True
                        continue
                    transitions.append(
                        WaitTransition(
                            at_seconds=round(time.monotonic() - started, 3),
                            kind=kind,
                            detail=detail,
                        )
                    )
            previous = observed.snapshot

            expired = time.monotonic() >= deadline
            if observed.matched or expired:
                return WaitResult(
                    matched=observed.matched,
                    timed_out=not observed.matched and expired,
                    condition=condition,
                    elapsed_seconds=round(time.monotonic() - started, 3),
                    polls=polls,
                    transitions=transitions,
                    transitions_truncated=truncated,
                    snapshot=observed.snapshot,
                )

            await anyio.sleep(WAIT_POLL_INTERVAL_SECONDS)

    @server.tool(
        annotations=MUTATING,
        description=(
            "Dispatch a whisper command to a bot from its real player master. Reports dispatch "
            "only. The bot may still silently reject the command, so confirm the effect through "
            "the returned baseline sequences."
        ),
    )
    async def send_bot_command(bot_guid: int, master_guid: int, command: str) -> CommandResult:
        # Validate every caller supplied field before even the read only preflight leaves the process.
        CommandRequest(bot_guid=bot_guid, master_guid=master_guid, command=command)
        snapshot = await anyio.to_thread.run_sync(partial(client.inspect, bot_guid=bot_guid))
        master = snapshot.master
        if (
            not master.available
            or not master.relationship_valid
            or not master.guid
            or _player_guid_low(master.guid) != master_guid
        ):
            raise ValueError("master_guid is not the bot's current real player master.")
        return await anyio.to_thread.run_sync(
            partial(client.command, bot_guid=bot_guid, master_guid=master_guid, command=command)
        )

    @server.tool(
        annotations=IDEMPOTENT_MUTATING,
        description=(
            "Verification staging: overwrite one skill the bot already knows (value and rank cap). "
            "Refuses skills the bot never learned. Use it to stage a capped gatherer or a grey node "
            "scenario without waiting for it to happen naturally, and restore the previous values "
            "from the result afterwards."
        ),
    )
    async def set_bot_skill(bot_guid: int, skill_id: int, value: int, maximum: int) -> SetSkillResult:
        SetSkillRequest(bot_guid=bot_guid, skill_id=skill_id, value=value, maximum=maximum)
        return await anyio.to_thread.run_sync(
            partial(client.set_skill, bot_guid=bot_guid, skill_id=skill_id, value=value, maximum=maximum)
        )

    @server.tool(
        annotations=MUTATING,
        description=(
            "Verification staging: teleport one online bot beside the nearest currently spawned "
            "gameobject of the given entry on its own map (for example 1731 for a Copper Vein). "
            "Resets the bot's stuck state like recover_bot does. Fails with gameobject_not_found "
            "when no spawn of that entry is active on the map."
        ),
    )
    async def teleport_bot_to_gameobject(bot_guid: int, game_object_entry: int) -> TeleportToGameObjectResult:
        TeleportToGameObjectRequest(bot_guid=bot_guid, game_object_entry=game_object_entry)
        return await anyio.to_thread.run_sync(
            partial(client.teleport_to_gameobject, bot_guid=bot_guid, game_object_entry=game_object_entry)
        )

    @server.tool(
        annotations=MUTATING,
        description=(
            "GM tooling: run one worldserver console command with console authority (for example "
            ".tele name <bot> <location>, .go xyz <x> <y> <z> <map>, .npc info) and return its "
            "captured output. Server lifecycle and account administration commands are refused."
        ),
    )
    async def run_gm_command(command: str) -> GmCommandResult:
        GmCommandRequest(command=command)
        return await anyio.to_thread.run_sync(partial(client.gm_command, command=command))

    @server.tool(
        annotations=IDEMPOTENT_MUTATING,
        description=(
            "Return one exact online managed Playerbot to its authoritative homebind. Accepts no "
            "command, coordinates, map, orientation, name, account, master, or offline target."
        ),
    )
    async def recover_bot(bot_guid: Any, destination: Any) -> RecoveryResult:
        if type(bot_guid) is not int or not 1 <= bot_guid <= UINT32_MAX:
            return _recovery_result(
                bot_guid=None,
                destination="homebind" if destination == "homebind" else "missing",
                outcome=RecoveryOutcome.INVALID_REQUEST,
                reason=RecoveryReason.INVALID_GUID,
            )
        if not isinstance(destination, str):
            return _recovery_result(
                bot_guid=bot_guid,
                destination="missing",
                outcome=RecoveryOutcome.INVALID_REQUEST,
                reason=RecoveryReason.INVALID_TOOL_INPUT,
            )
        if len(destination) > len("homebind"):
            return _recovery_result(
                bot_guid=bot_guid,
                destination="unsupported",
                outcome=RecoveryOutcome.INVALID_REQUEST,
                reason=RecoveryReason.INVALID_TOOL_INPUT,
            )
        if destination != "homebind":
            return _recovery_result(
                bot_guid=bot_guid,
                destination="unsupported",
                outcome=RecoveryOutcome.UNSUPPORTED_DESTINATION,
                reason=RecoveryReason.DESTINATION_NOT_HOMEBIND,
            )

        try:
            return await anyio.to_thread.run_sync(partial(client.recover, bot_guid=bot_guid))
        except ServerError as error:
            return _server_recovery_error(bot_guid, error)
        except VerificationTimeoutError as error:
            reasons = {
                VerificationTimeoutStage.CLIENT_LOCK: RecoveryReason.CLIENT_LOCK_TIMEOUT,
                VerificationTimeoutStage.SOCKET: RecoveryReason.SOCKET_TIMEOUT,
                VerificationTimeoutStage.RESPONSE: RecoveryReason.RESPONSE_TIMEOUT,
            }
            mutation = (
                RecoveryMutationState.UNKNOWN_AFTER_EXECUTION_STARTED
                if error.stage is VerificationTimeoutStage.RESPONSE
                else RecoveryMutationState.NOT_STARTED
            )
            return _recovery_result(
                bot_guid=bot_guid,
                destination="homebind",
                outcome=RecoveryOutcome.RECOVERY_TIMED_OUT,
                reason=reasons[error.stage],
                mutation_state=mutation,
            )
        except ConfigurationError:
            return _recovery_result(
                bot_guid=bot_guid,
                destination="homebind",
                outcome=RecoveryOutcome.RECOVERY_FAILED,
                reason=RecoveryReason.ADAPTER_CONFIGURATION,
            )
        except VerificationConnectionError as error:
            return _recovery_result(
                bot_guid=bot_guid,
                destination="homebind",
                outcome=RecoveryOutcome.RECOVERY_FAILED,
                reason=RecoveryReason.SERVER_UNREACHABLE,
                mutation_state=(
                    RecoveryMutationState.UNKNOWN_AFTER_EXECUTION_STARTED
                    if error.request_sent
                    else RecoveryMutationState.NOT_STARTED
                ),
            )
        except ProtocolMismatchError:
            return _recovery_result(
                bot_guid=bot_guid,
                destination="homebind",
                outcome=RecoveryOutcome.RECOVERY_FAILED,
                reason=RecoveryReason.PROTOCOL_MISMATCH,
                mutation_state=RecoveryMutationState.UNKNOWN_AFTER_EXECUTION_STARTED,
            )
        except ValidationError:
            return _recovery_result(
                bot_guid=bot_guid,
                destination="homebind",
                outcome=RecoveryOutcome.RECOVERY_FAILED,
                reason=RecoveryReason.INVALID_SERVER_RESPONSE,
                mutation_state=RecoveryMutationState.UNKNOWN_AFTER_EXECUTION_STARTED,
            )
        except VerificationError:
            return _recovery_result(
                bot_guid=bot_guid,
                destination="homebind",
                outcome=RecoveryOutcome.RECOVERY_FAILED,
                reason=RecoveryReason.SERVER_UNREACHABLE,
                mutation_state=RecoveryMutationState.UNKNOWN_AFTER_EXECUTION_STARTED,
            )

    # Referenced so linters see the registrations as used. The decorator already registered them.
    _ = (
        server_status,
        list_bots,
        inspect_bot,
        inspect_bot_loops,
        wait_for_bot,
        send_bot_command,
        recover_bot,
    )
    return server


def main() -> None:
    """Entry point for the console script. Reads the deployed secrets.env variables."""
    client = VerificationClient(VerificationSettings.from_environment())
    build_server(client).run()


if __name__ == "__main__":
    main()
