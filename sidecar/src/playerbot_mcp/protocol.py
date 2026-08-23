"""Wire protocol for the mod-playerbots verification server.

Every model mirrors a shape the C++ server actually emits. Models forbid unknown fields and run
in strict mode, so a server change that this client has not been taught about fails loudly here
instead of silently producing a half parsed snapshot.

Request models never carry the token. The token is injected at payload build time so it cannot
reach a model repr, a log line, or an assertion diff.
"""

from __future__ import annotations

import json
from enum import StrEnum
from typing import Any, ClassVar, Literal

from pydantic import BaseModel, ConfigDict, Field, ValidationError, model_validator
from pydantic.alias_generators import to_camel

SCHEMA_VERSION = 2
INSPECTION_SCHEMA_VERSION = 4
FRAME_HEADER_BYTES = 4
MAX_FRAME_PAYLOAD_BYTES = 64 * 1024
MAX_RESPONSE_PAYLOAD_BYTES = 60 * 1024
MIN_TOKEN_BYTES = 32
MAX_LIST_LIMIT = 100
MAX_ANOMALY_LIMIT = 50

UINT32_MAX = 0xFFFFFFFF


class ErrorCode(StrEnum):
    """Mirrors PlayerbotVerification::ErrorCodeName in the C++ server."""

    NONE = "none"
    MALFORMED_FRAME = "malformed_frame"
    FRAME_TOO_LARGE = "frame_too_large"
    MALFORMED_REQUEST = "malformed_request"
    AUTHENTICATION_FAILED = "authentication_failed"
    UNSUPPORTED_SCHEMA_VERSION = "unsupported_schema_version"
    UNKNOWN_OPERATION = "unknown_operation"
    INVALID_GUID = "invalid_guid"
    INVALID_LIMIT = "invalid_limit"
    INVALID_COMMAND = "invalid_command"
    INVALID_CONDITION = "invalid_condition"
    UNSUPPORTED_DESTINATION = "unsupported_destination"
    RESPONSE_TOO_LARGE = "response_too_large"
    OPERATION_UNAVAILABLE = "operation_unavailable"
    QUEUE_FULL = "queue_full"
    TIMEOUT = "timeout"
    SHUTDOWN = "shutdown"
    BOT_NOT_FOUND = "bot_not_found"
    BOT_UNAVAILABLE = "bot_unavailable"
    NOT_MANAGED_PLAYERBOT = "not_managed_playerbot"
    RECOVERY_FAILED = "recovery_failed"
    MASTER_NOT_FOUND = "master_not_found"
    MASTER_IS_BOT = "master_is_bot"
    INVALID_RELATIONSHIP = "invalid_relationship"
    INVALID_SKILL = "invalid_skill"
    GAMEOBJECT_NOT_FOUND = "gameobject_not_found"
    INTERNAL_ERROR = "internal_error"


class VerificationError(Exception):
    """Base class for every failure this client raises."""


class ConfigurationError(VerificationError):
    """The environment does not describe a usable verification endpoint."""


class ProtocolMismatchError(VerificationError):
    """The peer did not speak the protocol this client was built against."""


class VerificationTimeoutStage(StrEnum):
    CLIENT_LOCK = "client_lock"
    SOCKET = "socket"
    RESPONSE = "response"


class VerificationTimeoutError(VerificationError):
    """A verification call exceeded its deadline at a known bounded stage."""

    def __init__(self, stage: VerificationTimeoutStage, message: str) -> None:
        super().__init__(message)
        self.stage = stage


class VerificationConnectionError(VerificationError):
    """The loopback connection failed, with whether the request may have left the process."""

    def __init__(self, *, request_sent: bool) -> None:
        super().__init__("The verification server is unreachable.")
        self.request_sent = request_sent


class ServerError(VerificationError):
    """The server answered with a typed refusal. The request did not take effect."""

    def __init__(self, code: ErrorCode, message: str) -> None:
        super().__init__(f"{code.value}: {message}")
        self.code = code
        self.message = message


def encode_frame(payload: bytes) -> bytes:
    """Prefixes the payload with its network order length."""
    if len(payload) > MAX_FRAME_PAYLOAD_BYTES:
        raise ProtocolMismatchError(
            f"The payload is {len(payload)} bytes, above the {MAX_FRAME_PAYLOAD_BYTES} byte ceiling."
        )
    return len(payload).to_bytes(FRAME_HEADER_BYTES, "big") + payload


def decode_frame(frame: bytes) -> bytes:
    """Returns the payload of a complete frame, refusing any length disagreement."""
    if len(frame) < FRAME_HEADER_BYTES:
        raise ProtocolMismatchError("The frame is shorter than its header.")

    declared = int.from_bytes(frame[:FRAME_HEADER_BYTES], "big")
    if declared > MAX_FRAME_PAYLOAD_BYTES:
        raise ProtocolMismatchError(f"The frame declares {declared} bytes, above the ceiling.")

    body = frame[FRAME_HEADER_BYTES:]
    if len(body) != declared:
        raise ProtocolMismatchError(f"The frame declares {declared} bytes but carries {len(body)}.")
    return body


class WireModel(BaseModel):
    """Strict camelCase model. Unknown fields are an error, and nothing is coerced."""

    model_config = ConfigDict(
        alias_generator=to_camel,
        populate_by_name=True,
        extra="forbid",
        strict=True,
        frozen=True,
    )


class VerificationRequest(WireModel):
    """Operation specific request fields. The envelope is added by build_request_payload."""

    operation: ClassVar[str]


class StatusRequest(VerificationRequest):
    operation: ClassVar[str] = "status"


class ListRequest(VerificationRequest):
    operation: ClassVar[str] = "list"

    after_guid: int = Field(ge=0, le=UINT32_MAX)
    limit: int = Field(ge=1, le=MAX_LIST_LIMIT)


class InspectRequest(VerificationRequest):
    operation: ClassVar[str] = "inspect"

    bot_guid: int = Field(ge=1, le=UINT32_MAX)


class AnomaliesRequest(VerificationRequest):
    operation: ClassVar[str] = "anomalies"

    limit: int = Field(ge=1, le=MAX_ANOMALY_LIMIT)


class CommandRequest(VerificationRequest):
    operation: ClassVar[str] = "command"

    bot_guid: int = Field(ge=1, le=UINT32_MAX)
    master_guid: int = Field(ge=1, le=UINT32_MAX)
    command: str = Field(min_length=1)


SKILL_RANK_STEP = 75
MAX_SKILL_MAXIMUM = 450


class SetSkillRequest(VerificationRequest):
    """Verification staging: overwrite one skill the bot already knows."""

    operation: ClassVar[str] = "set_skill"

    bot_guid: int = Field(ge=1, le=UINT32_MAX)
    skill_id: int = Field(ge=1, le=UINT32_MAX)
    value: int = Field(ge=1, le=MAX_SKILL_MAXIMUM)
    maximum: int = Field(ge=SKILL_RANK_STEP, le=MAX_SKILL_MAXIMUM, multiple_of=SKILL_RANK_STEP)

    @model_validator(mode="after")
    def _value_within_maximum(self) -> SetSkillRequest:
        if self.value > self.maximum:
            raise ValueError("value must not exceed maximum.")
        return self


class TeleportToGameObjectRequest(VerificationRequest):
    """Verification staging: park the bot beside the nearest spawned gameobject of one entry."""

    operation: ClassVar[str] = "teleport_to_gameobject"

    bot_guid: int = Field(ge=1, le=UINT32_MAX)
    game_object_entry: int = Field(ge=1, le=UINT32_MAX)


class GmCommandRequest(VerificationRequest):
    """Run one console command with console authority. Server and account administration are refused."""

    operation: ClassVar[str] = "gm_command"

    command: str = Field(min_length=1)


class RecoverRequest(VerificationRequest):
    operation: ClassVar[str] = "recover"

    bot_guid: int = Field(ge=1, le=UINT32_MAX)
    destination: Literal["homebind"] = "homebind"


class CheckRequest(VerificationRequest):
    operation: ClassVar[str] = "check"

    bot_guid: int = Field(ge=1, le=UINT32_MAX)


class TransportAttachedCheck(CheckRequest):
    condition: Literal["transport_attached"] = "transport_attached"
    transport_entry: int | None = Field(default=None, ge=1, le=UINT32_MAX)


class TransportDetachedCheck(CheckRequest):
    condition: Literal["transport_detached"] = "transport_detached"


class MapCheck(CheckRequest):
    condition: Literal["map"] = "map"
    map_id: int = Field(ge=0, le=UINT32_MAX)


class ActionCheck(CheckRequest):
    condition: Literal["action"] = "action"
    after_sequence: int = Field(ge=0)
    action_name: str = Field(min_length=1)
    action_result: Literal["success", "failure", "either"]


class ProfessionSkillCheck(CheckRequest):
    condition: Literal["profession_skill"] = "profession_skill"
    skill_id: int = Field(ge=1, le=UINT32_MAX)
    minimum_value: int = Field(ge=0, le=UINT32_MAX)


class InventoryCheck(CheckRequest):
    condition: Literal["inventory"] = "inventory"
    item_id: int = Field(ge=1, le=UINT32_MAX)
    minimum_count: int = Field(ge=0, le=UINT32_MAX)


class MoneyAtMostCheck(CheckRequest):
    condition: Literal["money_at_most"] = "money_at_most"
    maximum_copper: int = Field(ge=0)


class MoneyDecreaseCheck(CheckRequest):
    condition: Literal["money_decrease"] = "money_decrease"
    baseline_copper: int = Field(ge=0)


class KnownRecipeCheck(CheckRequest):
    condition: Literal["known_recipe"] = "known_recipe"
    spell_id: int = Field(ge=1, le=UINT32_MAX)


class EconomyCheck(CheckRequest):
    condition: Literal["economy"] = "economy"
    after_sequence: int = Field(ge=0)
    economy_outcome: Literal[
        "scheduled",
        "operation",
        "no_candidate",
        "failed_precondition",
        "released",
        "blocked",
        "quarantined",
    ]


AnyCheck = (
    TransportAttachedCheck
    | TransportDetachedCheck
    | MapCheck
    | ActionCheck
    | ProfessionSkillCheck
    | InventoryCheck
    | MoneyAtMostCheck
    | MoneyDecreaseCheck
    | KnownRecipeCheck
    | EconomyCheck
)


CHECK_MODELS: dict[str, type[CheckRequest]] = {
    "transport_attached": TransportAttachedCheck,
    "transport_detached": TransportDetachedCheck,
    "map": MapCheck,
    "action": ActionCheck,
    "profession_skill": ProfessionSkillCheck,
    "inventory": InventoryCheck,
    "money_at_most": MoneyAtMostCheck,
    "money_decrease": MoneyDecreaseCheck,
    "known_recipe": KnownRecipeCheck,
    "economy": EconomyCheck,
}


def build_check(condition: str, *, bot_guid: int, **params: object) -> AnyCheck:
    """Builds the request model for one condition from loosely supplied parameters.

    Each model requires exactly the fields its condition needs and forbids the rest, so a field
    borrowed from another condition, or a missing required one, raises here rather than being
    sent and refused by the server.
    """
    model = CHECK_MODELS.get(condition)
    if model is None:
        raise ValueError(f"Unknown condition {condition!r}. Known: {', '.join(sorted(CHECK_MODELS))}.")

    supplied = {name: value for name, value in params.items() if value is not None}
    built = model(bot_guid=bot_guid, **supplied)
    assert isinstance(built, AnyCheck)  # every CHECK_MODELS entry is a member of the union
    return built


def build_request_payload(request: VerificationRequest, *, request_id: int, token: str) -> bytes:
    """Builds the JSON payload, adding the envelope the server requires.

    An unset optional condition field is omitted rather than sent as null, because the server
    validates the exact field set for each condition and rejects anything extra.
    """
    payload: dict[str, Any] = {
        "schemaVersion": SCHEMA_VERSION,
        "requestId": request_id,
        "token": token,
        "operation": request.operation,
    }
    payload.update(request.model_dump(by_alias=True, exclude_none=True))
    return json.dumps(payload, separators=(",", ":")).encode()


class Completeness(WireModel):
    total_count: int
    returned_count: int
    truncated: bool


class StatusResult(WireModel):
    protocol_schema_version: int
    inspection_schema_version: int
    module_enabled: bool
    queue_available: bool
    queue_size: int
    bot_count: int


class MasterSummary(WireModel):
    available: bool
    guid: str
    name: str
    relationship_valid: bool


class ListedBot(WireModel):
    guid: str
    guid_low: int
    name: str
    state: Literal["combat", "dead", "non-combat", "unknown"]
    master: MasterSummary
    map_id: int
    transport_attached: bool


class ListResult(WireModel):
    bots: list[ListedBot]
    next_after_guid: int
    has_more: bool
    completeness: Completeness


class AnomalyBotIdentity(WireModel):
    guid: str
    guid_low: int
    name: str
    level: int


class AnomalyObjective(WireModel):
    kind: Literal["none", "quest", "grind", "profession"]
    key: int
    title: str


class AnomalyEvidence(WireModel):
    first_timestamp_ms: int
    last_timestamp_ms: int
    count: int


class BotLoopAnomaly(WireModel):
    bot: AnomalyBotIdentity
    classifier: Literal[
        "stationary_movement",
        "movement_oscillation",
        "repeated_action",
        "death_relapse",
        "recovery_relapse",
    ]
    objective: AnomalyObjective
    action: str
    evidence: AnomalyEvidence
    progress_delta: float
    death_count: int
    recovery_count: int


class AnomalyCompleteness(WireModel):
    total_bot_count: int
    total_anomaly_count: int
    returned_count: int
    truncated: bool


class AnomaliesResult(WireModel):
    anomalies: list[BotLoopAnomaly]
    completeness: AnomalyCompleteness


class Identity(WireModel):
    guid: str
    name: str
    level: int
    race_id: int
    class_id: int


class GroupMember(WireModel):
    guid: str
    name: str
    subgroup: int
    leader: bool


class GroupSummary(WireModel):
    available: bool
    guid: str
    leader_guid: str
    members: list[GroupMember]
    completeness: Completeness


class Position(WireModel):
    map_id: int
    zone_id: int
    area_id: int
    x: float
    y: float
    z: float
    orientation: float
    movement_flags: int
    moving: bool
    movement_state: Literal["teleporting", "moving", "stationary"]


class Transport(WireModel):
    attached: bool
    guid: str
    entry: int


class TravelPoint(WireModel):
    available: bool
    map_id: int
    x: float
    y: float
    z: float
    distance_yards: float


class TravelDestination(WireModel):
    type: str
    title: str
    distance_yards: float


class TravelRoute(WireModel):
    point_count: int
    next_path_type: Literal["none", "walk", "portal", "transport", "flight_path", "teleport_spell"]
    next_entry: int
    next_point: TravelPoint


class LastMovement(WireModel):
    point: TravelPoint
    age_ms: int
    delay_ms: int
    priority: Literal["idle", "wander", "normal", "combat", "forced"]


class Travel(WireModel):
    available: bool
    status: Literal["unavailable", "none", "prepare", "travel", "work", "cooldown", "expired", "unknown"]
    destination: TravelDestination
    forced: bool
    can_move: bool
    route: TravelRoute
    last_movement: LastMovement


class RpgTarget(WireModel):
    available: bool
    type: Literal["creature", "player", "gameObject", "unknown"] | None
    guid: str | None = Field(min_length=1)
    entry: int | None = Field(ge=0, le=UINT32_MAX)
    name: str | None = Field(min_length=1)
    npc_flags: int | None = Field(ge=0, le=UINT32_MAX)
    distance_yards: float | None = Field(ge=0)
    moving: bool | None

    @model_validator(mode="after")
    def availability_matches_details(self) -> RpgTarget:
        details = (
            self.type,
            self.guid,
            self.entry,
            self.name,
            self.npc_flags,
            self.distance_yards,
            self.moving,
        )
        if self.available and any(value is None for value in details):
            raise ValueError("An available RPG target requires complete details.")
        if not self.available and any(value is not None for value in details):
            raise ValueError("An unavailable RPG target requires null details.")
        return self


class ActionAttempt(WireModel):
    sequence: int
    timestamp_ms: int
    age_ms: int
    success: bool
    action_name: str
    name_truncated: bool


class LatestActionAttempt(ActionAttempt):
    available: bool


class ActionSection(WireModel):
    last_executed_action: str
    latest_attempt: LatestActionAttempt
    attempts: list[ActionAttempt]
    completeness: Completeness


class Finance(WireModel):
    money_copper: int
    free_tradeskill_copper: int
    free_spells_copper: int


class Career(WireModel):
    status: Literal["unavailable", "pending", "valid"]
    version: int
    candidate_token: str
    primary_skill_ids: list[int]
    secondary_skill_ids: list[int]
    spending_style: Literal["none", "minimal", "progression", "completionist"]
    market_eligible: bool
    engagement: int
    source: Literal["none", "loaded", "saved"]


class Economy(WireModel):
    available: bool
    sequence: int
    phase: Literal[
        "none",
        "collect_auction_mail",
        "craft",
        "buy_reagent",
        "buy_recipe",
        "buy_finished_good",
        "use_finished_good",
        "recover_finished_good",
        "sell_surplus",
        "gather",
        "market_making",
    ]
    outcome: Literal[
        "unavailable",
        "scheduled",
        "operation",
        "no_candidate",
        "failed_precondition",
        "released",
        "blocked",
        "quarantined",
    ]
    chain_public_id: str
    operation_identity: str
    market_id: int
    item_family: str
    work_order_spell_id: int
    remaining_quantity: int
    claim_age_seconds: int
    blocker_code: str
    consecutive_failures: int
    cooldown_seconds: int
    next_eligible_time: int
    quarantined: bool


class RecipeCollection(WireModel):
    items: list[int]
    completeness: Completeness


class EquipmentItem(WireModel):
    slot: int
    item_id: int
    name: str
    count: int


class InventoryItem(WireModel):
    item_id: int
    name: str
    count: int


class Skill(WireModel):
    id: int
    name: str
    value: int
    maximum: int


class EquipmentCollection(WireModel):
    items: list[EquipmentItem]
    completeness: Completeness


class InventoryCollection(WireModel):
    items: list[InventoryItem]
    completeness: Completeness


class SkillCollection(WireModel):
    items: list[Skill]
    completeness: Completeness


class InspectResult(WireModel):
    schema_version: int
    ok: bool
    identity: Identity
    master: MasterSummary
    group: GroupSummary
    position: Position
    transport: Transport
    travel: Travel
    rpg_target: RpgTarget
    action: ActionSection
    finance: Finance
    career: Career
    known_recipe_spell_ids: RecipeCollection
    economy: Economy
    equipment: EquipmentCollection
    inventory: InventoryCollection
    skills: SkillCollection
    professions: SkillCollection


class CheckResult(WireModel):
    matched: bool
    condition: str
    snapshot: InspectResult


class CommandResult(WireModel):
    dispatched: bool
    bot_guid: str
    master_guid: str
    command: str
    baseline_action_sequence: int
    baseline_economy_sequence: int


class SetSkillResult(WireModel):
    bot_guid: str
    skill_id: int
    previous_value: int
    previous_maximum: int
    value: int
    maximum: int


class TeleportToGameObjectResult(WireModel):
    bot_guid: str
    game_object_entry: int
    spawn_id: int
    map_id: int
    distance_before: float


class GmCommandResult(WireModel):
    command: str
    succeeded: bool
    output: str


class RecoveryOutcome(StrEnum):
    RECOVERED = "recovered"
    ALREADY_AT_HOMEBIND = "already_at_homebind"
    INVALID_REQUEST = "invalid_request"
    UNAUTHORIZED = "unauthorized"
    BOT_NOT_FOUND = "bot_not_found"
    BOT_NOT_AVAILABLE = "bot_not_available"
    NOT_MANAGED_PLAYERBOT = "not_managed_playerbot"
    UNSUPPORTED_DESTINATION = "unsupported_destination"
    RECOVERY_FAILED = "recovery_failed"
    RECOVERY_TIMED_OUT = "recovery_timed_out"


class RecoveryReason(StrEnum):
    HOMEBIND_TELEPORT_ACCEPTED = "homebind_teleport_accepted"
    CURRENT_HOMEBIND = "current_homebind"
    PENDING_HOMEBIND = "pending_homebind"
    MALFORMED_REQUEST = "malformed_request"
    INVALID_GUID = "invalid_guid"
    UNSUPPORTED_SCHEMA = "unsupported_schema"
    UNKNOWN_OPERATION = "unknown_operation"
    INVALID_TOOL_INPUT = "invalid_tool_input"
    AUTHENTICATION_FAILED = "authentication_failed"
    CHARACTER_NOT_FOUND = "character_not_found"
    CHARACTER_OFFLINE = "character_offline"
    CHARACTER_NOT_IN_WORLD = "character_not_in_world"
    DEAD = "dead"
    IN_COMBAT = "in_combat"
    ROOTED = "rooted"
    IN_FLIGHT = "in_flight"
    BATTLEGROUND_QUEUE = "battleground_queue"
    BATTLEGROUND = "battleground"
    ARENA = "arena"
    ON_TRANSPORT = "on_transport"
    TELEPORT_IN_PROGRESS = "teleport_in_progress"
    PLAYERBOT_AI_MISSING = "playerbot_ai_missing"
    DESTINATION_NOT_HOMEBIND = "destination_not_homebind"
    INVALID_HOMEBIND = "invalid_homebind"
    TELEPORT_REJECTED = "teleport_rejected"
    OPERATION_UNAVAILABLE = "operation_unavailable"
    QUEUE_FULL = "queue_full"
    SHUTTING_DOWN = "shutting_down"
    INTERNAL_ERROR = "internal_error"
    RESPONSE_TOO_LARGE = "response_too_large"
    ADAPTER_CONFIGURATION = "adapter_configuration"
    SERVER_UNREACHABLE = "server_unreachable"
    PROTOCOL_MISMATCH = "protocol_mismatch"
    INVALID_SERVER_RESPONSE = "invalid_server_response"
    QUEUE_TIMEOUT_BEFORE_CLAIM = "queue_timeout_before_claim"
    EXECUTION_TIMEOUT_AFTER_CLAIM = "execution_timeout_after_claim"
    CLIENT_LOCK_TIMEOUT = "client_lock_timeout"
    SOCKET_TIMEOUT = "socket_timeout"
    RESPONSE_TIMEOUT = "response_timeout"


class RecoveryMutationState(StrEnum):
    NOT_STARTED = "not_started"
    COMPLETED = "completed"
    UNKNOWN_AFTER_EXECUTION_STARTED = "unknown_after_execution_started"


class RecoveryPersistenceState(StrEnum):
    NOT_REQUESTED = "not_requested"
    DEFERRED = "deferred"


RECOVERY_REASONS_BY_OUTCOME: dict[RecoveryOutcome, frozenset[RecoveryReason]] = {
    RecoveryOutcome.RECOVERED: frozenset({RecoveryReason.HOMEBIND_TELEPORT_ACCEPTED}),
    RecoveryOutcome.ALREADY_AT_HOMEBIND: frozenset(
        {RecoveryReason.CURRENT_HOMEBIND, RecoveryReason.PENDING_HOMEBIND}
    ),
    RecoveryOutcome.INVALID_REQUEST: frozenset(
        {
            RecoveryReason.MALFORMED_REQUEST,
            RecoveryReason.INVALID_GUID,
            RecoveryReason.UNSUPPORTED_SCHEMA,
            RecoveryReason.UNKNOWN_OPERATION,
            RecoveryReason.INVALID_TOOL_INPUT,
        }
    ),
    RecoveryOutcome.UNAUTHORIZED: frozenset({RecoveryReason.AUTHENTICATION_FAILED}),
    RecoveryOutcome.BOT_NOT_FOUND: frozenset({RecoveryReason.CHARACTER_NOT_FOUND}),
    RecoveryOutcome.BOT_NOT_AVAILABLE: frozenset(
        {
            RecoveryReason.CHARACTER_OFFLINE,
            RecoveryReason.CHARACTER_NOT_IN_WORLD,
            RecoveryReason.DEAD,
            RecoveryReason.IN_COMBAT,
            RecoveryReason.ROOTED,
            RecoveryReason.IN_FLIGHT,
            RecoveryReason.BATTLEGROUND_QUEUE,
            RecoveryReason.BATTLEGROUND,
            RecoveryReason.ARENA,
            RecoveryReason.ON_TRANSPORT,
            RecoveryReason.TELEPORT_IN_PROGRESS,
        }
    ),
    RecoveryOutcome.NOT_MANAGED_PLAYERBOT: frozenset({RecoveryReason.PLAYERBOT_AI_MISSING}),
    RecoveryOutcome.UNSUPPORTED_DESTINATION: frozenset({RecoveryReason.DESTINATION_NOT_HOMEBIND}),
    RecoveryOutcome.RECOVERY_FAILED: frozenset(
        {
            RecoveryReason.INVALID_HOMEBIND,
            RecoveryReason.TELEPORT_REJECTED,
            RecoveryReason.OPERATION_UNAVAILABLE,
            RecoveryReason.QUEUE_FULL,
            RecoveryReason.SHUTTING_DOWN,
            RecoveryReason.INTERNAL_ERROR,
            RecoveryReason.RESPONSE_TOO_LARGE,
            RecoveryReason.ADAPTER_CONFIGURATION,
            RecoveryReason.SERVER_UNREACHABLE,
            RecoveryReason.PROTOCOL_MISMATCH,
            RecoveryReason.INVALID_SERVER_RESPONSE,
        }
    ),
    RecoveryOutcome.RECOVERY_TIMED_OUT: frozenset(
        {
            RecoveryReason.QUEUE_TIMEOUT_BEFORE_CLAIM,
            RecoveryReason.EXECUTION_TIMEOUT_AFTER_CLAIM,
            RecoveryReason.CLIENT_LOCK_TIMEOUT,
            RecoveryReason.SOCKET_TIMEOUT,
            RecoveryReason.RESPONSE_TIMEOUT,
        }
    ),
}


class RecoveryPosition(WireModel):
    map_id: int = Field(ge=0, le=UINT32_MAX)
    zone_id: int = Field(ge=0, le=UINT32_MAX)
    area_id: int = Field(ge=0, le=UINT32_MAX)
    x: float
    y: float
    z: float
    orientation: float


class RecoveryResult(WireModel):
    timestamp_ms: int = Field(ge=0)
    operation: Literal["recover"]
    request_id: int | None = Field(default=None, ge=0)
    bot_guid: int | None = Field(default=None, ge=1, le=UINT32_MAX)
    bot_name: str | None = None
    destination: Literal["missing", "homebind", "unsupported"]
    before_position: RecoveryPosition | None = None
    accepted_destination: RecoveryPosition | None = None
    observed_position: RecoveryPosition | None = None
    observed_at_destination: bool
    movement_reset: bool
    travel_reset: bool
    taxi_reset: bool
    outcome: RecoveryOutcome = Field(strict=False)
    reason: RecoveryReason = Field(strict=False)
    mutation_state: RecoveryMutationState = Field(strict=False)
    persistence_state: RecoveryPersistenceState = Field(strict=False)

    @model_validator(mode="after")
    def reason_matches_outcome(self) -> RecoveryResult:
        if self.reason not in RECOVERY_REASONS_BY_OUTCOME[self.outcome]:
            raise ValueError(f"Reason {self.reason.value} is invalid for outcome {self.outcome.value}.")
        return self


class ResponseError(WireModel):
    # Strict mode would demand an ErrorCode instance, but the wire carries the name as a string.
    # Membership is still enforced, so a code this client does not know is a protocol mismatch.
    code: ErrorCode = Field(strict=False)
    message: str


class ResponseEnvelope(WireModel):
    schema_version: int
    request_id: int
    ok: bool
    result: dict[str, Any] | None = None
    error: ResponseError | None = None


def parse_envelope(payload: bytes, *, expected_id: int) -> ResponseEnvelope:
    """Parses one response envelope, refusing anything that is not the answer we asked for.

    Failures here never quote the payload. A request payload carries the token, and keeping raw
    bytes out of exception text is what guarantees it can never surface in a traceback.
    """
    try:
        document = json.loads(payload)
    except (json.JSONDecodeError, UnicodeDecodeError) as error:
        raise ProtocolMismatchError("The response body is not valid JSON.") from error

    if not isinstance(document, dict):
        raise ProtocolMismatchError("The response body is not a JSON object.")

    try:
        envelope = ResponseEnvelope.model_validate(document)
    except ValidationError as error:
        raise ProtocolMismatchError(
            f"The response envelope is not recognised: {error.error_count()} problem(s)."
        ) from error

    if envelope.schema_version != SCHEMA_VERSION:
        raise ProtocolMismatchError(
            f"The server speaks schema {envelope.schema_version}, this client speaks {SCHEMA_VERSION}."
        )
    if envelope.request_id != expected_id:
        raise ProtocolMismatchError(f"The response answers request {envelope.request_id}, not {expected_id}.")
    if envelope.ok and envelope.result is None:
        raise ProtocolMismatchError("A successful response carried no result.")
    if not envelope.ok and envelope.error is None:
        raise ProtocolMismatchError("A failure response carried no error.")
    return envelope
