/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTVERIFICATIONPROTOCOL_H
#define PLAYERBOTS_PLAYERBOTVERIFICATIONPROTOCOL_H

#include <array>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Define.h"

namespace PlayerbotVerification
{
inline constexpr uint32 SCHEMA_VERSION = 2;
inline constexpr std::size_t FRAME_HEADER_BYTES = 4;
inline constexpr std::size_t MAX_FRAME_PAYLOAD_BYTES = 64 * 1024;
inline constexpr std::size_t MAX_RESPONSE_PAYLOAD_BYTES = 60 * 1024;
inline constexpr std::size_t MIN_TOKEN_BYTES = 32;
inline constexpr std::size_t TOKEN_DIGEST_BYTES = 32;
inline constexpr uint32 MAX_LIST_LIMIT = 100;
inline constexpr uint32 MAX_ANOMALY_LIMIT = 50;
// Profession rank caps are multiples of 75 up to Grand Master (450); SetSkill derives the step from it.
inline constexpr uint32 SKILL_RANK_STEP = 75;
inline constexpr uint32 MAX_SKILL_MAXIMUM = 450;

enum class ErrorCode
{
    None,
    MalformedFrame,
    FrameTooLarge,
    MalformedRequest,
    AuthenticationFailed,
    UnsupportedSchemaVersion,
    UnknownOperation,
    InvalidGuid,
    InvalidLimit,
    InvalidCommand,
    InvalidCondition,
    UnsupportedDestination,
    ResponseTooLarge,
    OperationUnavailable,
    QueueFull,
    Timeout,
    Shutdown,
    BotNotFound,
    BotUnavailable,
    NotManagedPlayerbot,
    RecoveryFailed,
    MasterNotFound,
    MasterIsBot,
    InvalidRelationship,
    InvalidSkill,
    GameObjectNotFound,
    InternalError
};

struct ProtocolError
{
    ErrorCode code = ErrorCode::None;
    std::string message;
};

char const* ErrorCodeName(ErrorCode code);

struct FrameDecodeResult
{
    std::optional<std::string> payload;
    ProtocolError error;
};

std::optional<std::vector<uint8>> EncodeFrame(std::string const& payload);
FrameDecodeResult DecodeFrame(std::span<uint8 const> frame);

enum class Operation
{
    Status,
    List,
    Inspect,
    Anomalies,
    Check,
    Command,
    Recover,
    SetSkill,
    TeleportToGameObject,
    // Runs one console command with console authority on the world thread and returns its output.
    GmCommand
};

// The console command families the bridge refuses outright: process lifecycle and account
// administration are not verification tooling. The check ignores a leading dot and surrounding
// whitespace and matches the first word only.
bool IsRefusedGmCommand(std::string_view command);

enum class RecoveryDestination
{
    Missing,
    Homebind,
    Unsupported
};

enum class RecoveryOutcome
{
    Recovered,
    AlreadyAtHomebind,
    InvalidRequest,
    Unauthorized,
    BotNotFound,
    BotNotAvailable,
    NotManagedPlayerbot,
    UnsupportedDestination,
    RecoveryFailed,
    RecoveryTimedOut
};

enum class RecoveryReason
{
    HomebindTeleportAccepted,
    CurrentHomebind,
    PendingHomebind,
    MalformedRequest,
    InvalidGuid,
    UnsupportedSchema,
    UnknownOperation,
    InvalidToolInput,
    AuthenticationFailed,
    CharacterNotFound,
    CharacterOffline,
    CharacterNotInWorld,
    Dead,
    InCombat,
    Rooted,
    InFlight,
    BattlegroundQueue,
    Battleground,
    Arena,
    OnTransport,
    TeleportInProgress,
    PlayerbotAiMissing,
    DestinationNotHomebind,
    InvalidHomebind,
    TeleportRejected,
    OperationUnavailable,
    QueueFull,
    ShuttingDown,
    InternalError,
    ResponseTooLarge,
    AdapterConfiguration,
    ServerUnreachable,
    ProtocolMismatch,
    InvalidServerResponse,
    QueueTimeoutBeforeClaim,
    ExecutionTimeoutAfterClaim,
    ClientLockTimeout,
    SocketTimeout,
    ResponseTimeout
};

enum class RecoveryMutationState
{
    NotStarted,
    Completed,
    UnknownAfterExecutionStarted
};

enum class RecoveryPersistenceState
{
    NotRequested,
    Deferred
};

struct RecoveryPosition
{
    uint32 mapId = 0;
    uint32 zoneId = 0;
    uint32 areaId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float orientation = 0.0f;
};

struct RecoveryAuditRecord
{
    uint64 timestampMs = 0;
    std::optional<uint64> requestId;
    std::optional<uint32> botGuid;
    std::optional<std::string> botName;
    RecoveryDestination destination = RecoveryDestination::Missing;
    std::optional<RecoveryPosition> beforePosition;
    std::optional<RecoveryPosition> acceptedDestination;
    std::optional<RecoveryPosition> observedPosition;
    bool observedAtDestination = false;
    bool movementReset = false;
    bool travelReset = false;
    bool taxiReset = false;
    RecoveryOutcome outcome = RecoveryOutcome::InvalidRequest;
    RecoveryReason reason = RecoveryReason::MalformedRequest;
    RecoveryMutationState mutationState = RecoveryMutationState::NotStarted;
    RecoveryPersistenceState persistenceState = RecoveryPersistenceState::NotRequested;
};

char const* RecoveryDestinationName(RecoveryDestination destination);
char const* RecoveryOutcomeName(RecoveryOutcome outcome);
char const* RecoveryReasonName(RecoveryReason reason);
char const* RecoveryMutationStateName(RecoveryMutationState state);
char const* RecoveryPersistenceStateName(RecoveryPersistenceState state);
std::optional<RecoveryAuditRecord> BuildRejectedRecoveryAudit(std::string const& payload, ErrorCode error,
                                                              uint64 timestampMs);
std::string SerializeRecoveryAuditRecord(RecoveryAuditRecord const& record);

struct Request
{
    uint64 requestId = 0;
    Operation operation = Operation::Status;
    uint32 botGuid = 0;
    uint32 masterGuid = 0;
    uint32 afterGuid = 0;
    uint32 limit = 0;
    std::string command;
    std::string condition;
    std::string destination;
    std::map<std::string, uint64> numbers;
    std::map<std::string, std::string> strings;
};

struct RequestParseResult
{
    std::optional<Request> request;
    uint64 responseRequestId = 0;
    ProtocolError error;
};

using TokenDigest = std::array<uint8, TOKEN_DIGEST_BYTES>;

TokenDigest DigestVerificationToken(std::string_view token);
std::optional<TokenDigest> VerificationTokenDigestFromEnvironment();
bool ConstantTimeTokenDigestEquals(TokenDigest const& candidate, TokenDigest const& expected);
RequestParseResult ParseRequestPayload(std::string const& payload, TokenDigest const& expectedTokenDigest);

struct Response
{
    bool ok = false;
    std::string resultJson;
    ProtocolError error;
    std::optional<RecoveryAuditRecord> recoveryAudit;

    static Response Success(std::string resultJson);
    static Response Failure(ErrorCode code, std::string message);
};

std::string SerializeResponse(uint64 requestId, Response const& response);

struct GuidPage
{
    std::vector<uint32> guids;
    uint32 nextAfterGuid = 0;
    bool hasMore = false;
};

std::optional<GuidPage> PaginateGuids(std::vector<uint32> guids, uint32 afterGuid, uint32 limit);
}  // namespace PlayerbotVerification

#endif
