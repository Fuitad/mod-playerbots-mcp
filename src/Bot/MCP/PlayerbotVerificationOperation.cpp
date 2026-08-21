/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotVerificationOperation.h"

#include <G3D/g3dmath.h>
#include <utf8.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iterator>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include "Bot/PlayerbotAI.h"
#include "Bot/Recovery/PlayerbotRecovery.h"
#include "Bot/Telemetry/PlayerbotTelemetryState.h"
#include "CharacterCache.h"
#include "DBCStores.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PlayerbotAIConfig.h"
#include "PoolMgr.h"
#include "Script/Playerbots.h"
#include "Script/WorldThr/PlayerbotWorldThreadProcessor.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "Transport.h"

using namespace PlayerbotVerification;

namespace
{
std::atomic<bool> acceptingRequests{false};

std::string_view VerificationStateName(BotState state)
{
    switch (state)
    {
        case BOT_STATE_COMBAT:
            return "combat";
        case BOT_STATE_DEAD:
            return "dead";
        case BOT_STATE_NON_COMBAT:
            return "non-combat";
        default:
            return "unknown";
    }
}

std::string_view LoopClassifierName(PlayerbotLoopClassifier classifier)
{
    switch (classifier)
    {
        case PlayerbotLoopClassifier::StationaryMovement:
            return "stationary_movement";
        case PlayerbotLoopClassifier::MovementOscillation:
            return "movement_oscillation";
        case PlayerbotLoopClassifier::RepeatedAction:
            return "repeated_action";
        case PlayerbotLoopClassifier::DeathRelapse:
            return "death_relapse";
        case PlayerbotLoopClassifier::RecoveryRelapse:
            return "recovery_relapse";
    }
    return "stationary_movement";
}

std::string_view LoopObjectiveKindName(PlayerbotLoopObjectiveKind kind)
{
    switch (kind)
    {
        case PlayerbotLoopObjectiveKind::None:
            return "none";
        case PlayerbotLoopObjectiveKind::Quest:
            return "quest";
        case PlayerbotLoopObjectiveKind::Grind:
            return "grind";
        case PlayerbotLoopObjectiveKind::Profession:
            return "profession";
    }
    return "none";
}

void AppendJsonString(std::ostringstream& out, std::string const& value)
{
    std::string sanitized;
    sanitized.reserve(value.size());
    utf8::replace_invalid(value.begin(), value.end(), std::back_inserter(sanitized));

    out << '"';
    for (unsigned char character : sanitized)
    {
        switch (character)
        {
            case '"':
                out << "\\\"";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            case '\b':
                out << "\\b";
                break;
            case '\f':
                out << "\\f";
                break;
            default:
                if (character < 0x20)
                {
                    static constexpr char HEX[] = "0123456789abcdef";
                    out << "\\u00" << HEX[(character >> 4) & 0x0F] << HEX[character & 0x0F];
                }
                else
                    out << static_cast<char>(character);
                break;
        }
    }
    out << '"';
}

template <std::size_t Capacity>
std::string BoundedText(std::array<char, Capacity> const& value)
{
    return std::string(value.data(), ::strnlen(value.data(), Capacity));
}

ObjectGuid MakePlayerGuid(uint32 lowGuid) { return ObjectGuid::Create<HighGuid::Player>(lowGuid); }

uint64 RecoveryTimestampMs()
{
    return static_cast<uint64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}

RecoveryAuditRecord NewRecoveryRecord(Request const& request)
{
    return {
        .timestampMs = RecoveryTimestampMs(),
        .requestId = request.requestId,
        .botGuid = request.botGuid,
        .destination = RecoveryDestination::Homebind,
    };
}

Response RecoveryResponse(RecoveryAuditRecord record)
{
    Response response = Response::Success(SerializeRecoveryAuditRecord(record));
    response.recoveryAudit = std::move(record);
    return response;
}

Response RecoveryResponse(Request const& request, RecoveryOutcome outcome, RecoveryReason reason,
                          RecoveryMutationState mutationState = RecoveryMutationState::NotStarted)
{
    RecoveryAuditRecord record = NewRecoveryRecord(request);
    record.outcome = outcome;
    record.reason = reason;
    record.mutationState = mutationState;
    return RecoveryResponse(std::move(record));
}

RecoveryPosition CurrentRecoveryPosition(Player const* player)
{
    uint32 zoneId = 0;
    uint32 areaId = 0;
    player->GetZoneAndAreaId(zoneId, areaId);
    return {
        .mapId = player->GetMapId(),
        .zoneId = zoneId,
        .areaId = areaId,
        .x = player->GetPositionX(),
        .y = player->GetPositionY(),
        .z = player->GetPositionZ(),
        .orientation = player->GetOrientation(),
    };
}

RecoveryPosition HomebindRecoveryPosition(Player const* player, WorldLocation const& destination)
{
    uint32 const areaId = player->m_homebindAreaId;
    AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId);
    uint32 const zoneId = area && area->zone ? area->zone : areaId;
    return {
        .mapId = destination.GetMapId(),
        .zoneId = zoneId,
        .areaId = areaId,
        .x = destination.GetPositionX(),
        .y = destination.GetPositionY(),
        .z = destination.GetPositionZ(),
        .orientation = destination.GetOrientation(),
    };
}

RecoveryPosition HomebindRecoveryPosition(Player const* player)
{
    WorldLocation const homebind(player->m_homebindMapId, player->m_homebindX, player->m_homebindY, player->m_homebindZ,
                                 player->GetOrientation());
    return HomebindRecoveryPosition(player, homebind);
}

bool RecoveryPositionsMatch(RecoveryPosition const& position, RecoveryPosition const& destination)
{
    return position.mapId == destination.mapId && G3D::fuzzyEq(position.x, destination.x) &&
           G3D::fuzzyEq(position.y, destination.y) && G3D::fuzzyEq(position.z, destination.z);
}

// A bot is a player that owns a PlayerbotAI. Real players never do.
PlayerbotAI* ResolveBotAI(Player* player) { return player ? GET_PLAYERBOT_AI(player) : nullptr; }

std::string ActionAttemptName(PlayerbotVerificationActionAttempt const& attempt)
{
    std::size_t const length = ::strnlen(attempt.actionName.data(), PLAYERBOT_VERIFICATION_ACTION_NAME_CAPACITY);
    return std::string(attempt.actionName.data(), length);
}

bool ActionAttemptNameMatches(PlayerbotVerificationActionAttempt const& attempt, std::string const& requested)
{
    // A truncated record only retained a prefix, which several distinct names can share. It can
    // never prove an exact name, so it never satisfies an exact wait condition.
    if (attempt.nameTruncated)
        return false;

    return ActionAttemptName(attempt) == requested;
}

char const* EconomyOutcomeToken(PlayerbotVerificationEconomyOutcome outcome)
{
    switch (outcome)
    {
        case PlayerbotVerificationEconomyOutcome::Scheduled:
            return "scheduled";
        case PlayerbotVerificationEconomyOutcome::Operation:
            return "operation";
        case PlayerbotVerificationEconomyOutcome::NoCandidate:
            return "no_candidate";
        case PlayerbotVerificationEconomyOutcome::FailedPrecondition:
            return "failed_precondition";
        case PlayerbotVerificationEconomyOutcome::Released:
            return "released";
        case PlayerbotVerificationEconomyOutcome::Blocked:
            return "blocked";
        case PlayerbotVerificationEconomyOutcome::Quarantined:
            return "quarantined";
        case PlayerbotVerificationEconomyOutcome::Unavailable:
            return "unavailable";
    }
    return "unavailable";
}

uint64 RequestNumber(Request const& request, std::string const& name, uint64 fallback = 0)
{
    auto const found = request.numbers.find(name);
    return found == request.numbers.end() ? fallback : found->second;
}

std::string RequestString(Request const& request, std::string const& name)
{
    auto const found = request.strings.find(name);
    return found == request.strings.end() ? std::string() : found->second;
}

Response BuildRecoverResponse(Request const& request, PlayerbotRecoveryPersistence const& persistence)
{
    RecoveryAuditRecord record = NewRecoveryRecord(request);
    ObjectGuid const guid = MakePlayerGuid(request.botGuid);
    Player* bot = ObjectAccessor::FindConnectedPlayer(guid);
    if (!bot)
    {
        if (sCharacterCache->HasCharacterCacheEntry(guid))
        {
            record.outcome = RecoveryOutcome::BotNotAvailable;
            record.reason = RecoveryReason::CharacterOffline;
        }
        else
        {
            record.outcome = RecoveryOutcome::BotNotFound;
            record.reason = RecoveryReason::CharacterNotFound;
        }
        return RecoveryResponse(std::move(record));
    }

    record.botName = bot->GetName();
    if (!bot->IsInWorld())
    {
        record.outcome = RecoveryOutcome::BotNotAvailable;
        record.reason = RecoveryReason::CharacterNotInWorld;
        return RecoveryResponse(std::move(record));
    }

    record.beforePosition = CurrentRecoveryPosition(bot);
    record.observedPosition = record.beforePosition;
    PlayerbotAI* botAI = ResolveBotAI(bot);
    if (!botAI)
    {
        record.outcome = RecoveryOutcome::NotManagedPlayerbot;
        record.reason = RecoveryReason::PlayerbotAiMissing;
        return RecoveryResponse(std::move(record));
    }

    PlayerbotHomebindRecoveryResult const result = PlayerbotRecoverToHomebind(botAI);
    record.observedPosition = CurrentRecoveryPosition(bot);

    switch (result)
    {
        case PlayerbotHomebindRecoveryResult::Recovered:
            record.acceptedDestination = HomebindRecoveryPosition(bot);
            record.observedAtDestination =
                RecoveryPositionsMatch(*record.observedPosition, *record.acceptedDestination);
            record.movementReset = true;
            record.travelReset = true;
            record.taxiReset = true;
            record.outcome = RecoveryOutcome::Recovered;
            record.reason = RecoveryReason::HomebindTeleportAccepted;
            record.mutationState = RecoveryMutationState::Completed;
            if (persistence)
                persistence(bot);
            else
                bot->SaveToDB(false, false);
            record.persistenceState = RecoveryPersistenceState::Deferred;
            break;
        case PlayerbotHomebindRecoveryResult::AlreadyAtCurrentHomebind:
            record.observedAtDestination = true;
            record.outcome = RecoveryOutcome::AlreadyAtHomebind;
            record.reason = RecoveryReason::CurrentHomebind;
            break;
        case PlayerbotHomebindRecoveryResult::AlreadyPendingHomebind:
            record.acceptedDestination = HomebindRecoveryPosition(bot, bot->GetTeleportDest());
            record.observedAtDestination =
                RecoveryPositionsMatch(*record.observedPosition, *record.acceptedDestination);
            record.outcome = RecoveryOutcome::AlreadyAtHomebind;
            record.reason = RecoveryReason::PendingHomebind;
            break;
        case PlayerbotHomebindRecoveryResult::NotInWorld:
            record.outcome = RecoveryOutcome::BotNotAvailable;
            record.reason = RecoveryReason::CharacterNotInWorld;
            break;
        case PlayerbotHomebindRecoveryResult::Dead:
            record.outcome = RecoveryOutcome::BotNotAvailable;
            record.reason = RecoveryReason::Dead;
            break;
        case PlayerbotHomebindRecoveryResult::InCombat:
            record.outcome = RecoveryOutcome::BotNotAvailable;
            record.reason = RecoveryReason::InCombat;
            break;
        case PlayerbotHomebindRecoveryResult::Rooted:
            record.outcome = RecoveryOutcome::BotNotAvailable;
            record.reason = RecoveryReason::Rooted;
            break;
        case PlayerbotHomebindRecoveryResult::InFlight:
            record.outcome = RecoveryOutcome::BotNotAvailable;
            record.reason = RecoveryReason::InFlight;
            break;
        case PlayerbotHomebindRecoveryResult::BattlegroundQueue:
            record.outcome = RecoveryOutcome::BotNotAvailable;
            record.reason = RecoveryReason::BattlegroundQueue;
            break;
        case PlayerbotHomebindRecoveryResult::Battleground:
            record.outcome = RecoveryOutcome::BotNotAvailable;
            record.reason = RecoveryReason::Battleground;
            break;
        case PlayerbotHomebindRecoveryResult::Arena:
            record.outcome = RecoveryOutcome::BotNotAvailable;
            record.reason = RecoveryReason::Arena;
            break;
        case PlayerbotHomebindRecoveryResult::OnTransport:
            record.outcome = RecoveryOutcome::BotNotAvailable;
            record.reason = RecoveryReason::OnTransport;
            break;
        case PlayerbotHomebindRecoveryResult::TeleportInProgress:
            record.outcome = RecoveryOutcome::BotNotAvailable;
            record.reason = RecoveryReason::TeleportInProgress;
            break;
        case PlayerbotHomebindRecoveryResult::InvalidHomebind:
            record.outcome = RecoveryOutcome::RecoveryFailed;
            record.reason = RecoveryReason::InvalidHomebind;
            break;
        case PlayerbotHomebindRecoveryResult::TeleportRejected:
            record.movementReset = true;
            record.travelReset = true;
            record.taxiReset = true;
            record.outcome = RecoveryOutcome::RecoveryFailed;
            record.reason = RecoveryReason::TeleportRejected;
            record.mutationState = RecoveryMutationState::Completed;
            break;
    }

    return RecoveryResponse(std::move(record));
}

Response BuildStatusResponse()
{
    PlayerbotWorldThreadProcessor const& processor = PlayerbotWorldThreadProcessor::instance();
    uint32 botCount = 0;
    for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
    {
        (void)guid;
        if (ResolveBotAI(player))
            ++botCount;
    }

    std::ostringstream out;
    out << "{\"protocolSchemaVersion\":" << SCHEMA_VERSION;
    out << ",\"inspectionSchemaVersion\":" << PLAYERBOT_VERIFICATION_INSPECTION_SCHEMA_VERSION;
    out << ",\"moduleEnabled\":" << (sPlayerbotAIConfig.enabled ? "true" : "false");
    out << ",\"queueAvailable\":" << (processor.IsEnabled() ? "true" : "false");
    out << ",\"queueSize\":" << processor.GetQueueSize();
    out << ",\"botCount\":" << botCount << '}';
    return Response::Success(out.str());
}

Response BuildListResponse(Request const& request)
{
    std::vector<uint32> guids;
    for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
    {
        if (ResolveBotAI(player))
            guids.push_back(guid.GetCounter());
    }

    uint64 const totalCount = guids.size();
    std::optional<GuidPage> const page = PaginateGuids(std::move(guids), request.afterGuid, request.limit);
    if (!page)
        return Response::Failure(ErrorCode::InvalidLimit, {});

    std::ostringstream out;
    out << "{\"bots\":[";
    bool first = true;
    for (uint32 lowGuid : page->guids)
    {
        Player* bot = ObjectAccessor::FindPlayer(MakePlayerGuid(lowGuid));
        PlayerbotAI* botAI = ResolveBotAI(bot);
        if (!botAI)
            continue;

        if (!first)
            out << ',';
        first = false;

        Player* master = botAI->GetMaster();
        out << "{\"guid\":";
        AppendJsonString(out, bot->GetGUID().ToString());
        out << ",\"guidLow\":" << lowGuid;
        out << ",\"name\":";
        AppendJsonString(out, bot->GetName());
        out << ",\"state\":";
        AppendJsonString(out, std::string(VerificationStateName(botAI->GetState())));
        out << ",\"master\":{\"available\":" << (master ? "true" : "false");
        out << ",\"guid\":";
        AppendJsonString(out, master ? master->GetGUID().ToString() : std::string());
        out << ",\"name\":";
        AppendJsonString(out, master ? master->GetName() : std::string());
        out << ",\"relationshipValid\":" << (botAI->HasGameClientMaster() ? "true" : "false") << '}';
        out << ",\"mapId\":" << bot->GetMapId();
        out << ",\"transportAttached\":" << (bot->GetTransport() ? "true" : "false") << '}';
    }
    out << "],\"nextAfterGuid\":" << page->nextAfterGuid;
    out << ",\"hasMore\":" << (page->hasMore ? "true" : "false");
    out << ",\"completeness\":{\"totalCount\":" << totalCount;
    out << ",\"returnedCount\":" << page->guids.size();
    out << ",\"truncated\":" << (page->hasMore ? "true" : "false") << "}}";
    return Response::Success(out.str());
}

Response BuildAnomaliesResponse(Request const& request)
{
    if (!request.limit || request.limit > MAX_ANOMALY_LIMIT)
        return Response::Failure(ErrorCode::InvalidLimit, {});

    std::vector<uint32> guids;
    for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
        if (ResolveBotAI(player))
            guids.push_back(guid.GetCounter());
    std::sort(guids.begin(), guids.end());

    struct AnomalyRecord
    {
        std::string guid;
        uint32 guidLow = 0;
        std::string name;
        uint8 level = 0;
        PlayerbotLoopAnomaly anomaly;
    };

    std::vector<AnomalyRecord> records;
    records.reserve(request.limit);
    uint64 totalAnomalyCount = 0;
    uint64 const nowMs = GetTimeMS().count();
    for (uint32 lowGuid : guids)
    {
        Player* bot = ObjectAccessor::FindPlayer(MakePlayerGuid(lowGuid));
        PlayerbotAI* botAI = ResolveBotAI(bot);
        if (!botAI)
            continue;

        PlayerbotLoopAnomalySnapshot const snapshot = PlayerbotRecoveryCopyAnomalies(botAI, nowMs);
        totalAnomalyCount += snapshot.count;
        for (std::size_t index = 0; index < snapshot.count && records.size() < request.limit; ++index)
        {
            records.push_back({
                .guid = bot->GetGUID().ToString(),
                .guidLow = lowGuid,
                .name = bot->GetName(),
                .level = bot->GetLevel(),
                .anomaly = snapshot.anomalies[index],
            });
        }
    }

    std::ostringstream out;
    out << "{\"anomalies\":[";
    for (std::size_t index = 0; index < records.size(); ++index)
    {
        if (index)
            out << ',';

        AnomalyRecord const& record = records[index];
        PlayerbotLoopAnomaly const& anomaly = record.anomaly;
        out << "{\"bot\":{\"guid\":";
        AppendJsonString(out, record.guid);
        out << ",\"guidLow\":" << record.guidLow << ",\"name\":";
        AppendJsonString(out, record.name);
        out << ",\"level\":" << static_cast<uint32>(record.level) << '}';
        out << ",\"classifier\":";
        AppendJsonString(out, std::string(LoopClassifierName(anomaly.classifier)));
        out << ",\"objective\":{\"kind\":";
        AppendJsonString(out, std::string(LoopObjectiveKindName(anomaly.objectiveKind)));
        out << ",\"key\":" << anomaly.objectiveKey << ",\"title\":";
        AppendJsonString(out, BoundedText(anomaly.objectiveTitle));
        out << "},\"action\":";
        AppendJsonString(out, BoundedText(anomaly.actionName));
        out << ",\"evidence\":{\"firstTimestampMs\":" << anomaly.firstEvidenceMs;
        out << ",\"lastTimestampMs\":" << anomaly.lastEvidenceMs;
        out << ",\"count\":" << anomaly.evidenceCount << '}';
        out << ",\"progressDelta\":" << anomaly.progressDelta;
        out << ",\"deathCount\":" << anomaly.deathCount;
        out << ",\"recoveryCount\":" << anomaly.recoveryCount << '}';
    }
    out << "],\"completeness\":{\"totalBotCount\":" << guids.size();
    out << ",\"totalAnomalyCount\":" << totalAnomalyCount;
    out << ",\"returnedCount\":" << records.size();
    out << ",\"truncated\":" << (totalAnomalyCount > records.size() ? "true" : "false") << "}}";
    return Response::Success(out.str());
}

Response BuildInspectResponse(Request const& request)
{
    Player* bot = ObjectAccessor::FindPlayer(MakePlayerGuid(request.botGuid));
    PlayerbotAI* botAI = ResolveBotAI(bot);
    if (!botAI)
        return Response::Failure(ErrorCode::BotNotFound, {});

    return Response::Success(
        PlayerbotInspector::SerializeVerification(PlayerbotInspector::BuildVerification(bot, botAI)));
}

Response BuildCheckResponse(Request const& request)
{
    Player* bot = ObjectAccessor::FindPlayer(MakePlayerGuid(request.botGuid));
    PlayerbotAI* botAI = ResolveBotAI(bot);
    if (!botAI)
        return Response::Failure(ErrorCode::BotNotFound, {});

    PlayerbotVerificationInspection const inspection = PlayerbotInspector::BuildVerification(bot, botAI);
    bool const matched = EvaluateVerificationCondition(inspection, request);

    std::ostringstream out;
    out << "{\"matched\":" << (matched ? "true" : "false") << ",\"condition\":";
    AppendJsonString(out, request.condition);
    out << ",\"snapshot\":" << PlayerbotInspector::SerializeVerification(inspection) << '}';
    return Response::Success(out.str());
}

Response BuildCommandResponse(Request const& request)
{
    Player* bot = ObjectAccessor::FindPlayer(MakePlayerGuid(request.botGuid));
    PlayerbotAI* botAI = ResolveBotAI(bot);
    if (!botAI)
        return Response::Failure(ErrorCode::BotNotFound, {});

    Player* master = ObjectAccessor::FindPlayer(MakePlayerGuid(request.masterGuid));
    if (!master)
        return Response::Failure(ErrorCode::MasterNotFound, {});
    if (ResolveBotAI(master))
        return Response::Failure(ErrorCode::MasterIsBot, {});
    if (botAI->GetMaster() != master)
        return Response::Failure(ErrorCode::InvalidRelationship, {});

    // totalCount carries the newest issued sequence, so the baseline is correct on an empty history too.
    PlayerbotVerificationSnapshot const baseline = PlayerbotTelemetryCopyVerification(botAI);
    uint64 const baselineActionSequence = baseline.actionHistory.totalCount;

    // The existing command API returns void and has silent parser and security rejection paths, so the
    // bridge can only report that it dispatched the whisper, never that the bot accepted it.
    botAI->HandleCommand(CHAT_MSG_WHISPER, request.command, master);

    std::ostringstream out;
    out << "{\"dispatched\":true,\"botGuid\":";
    AppendJsonString(out, bot->GetGUID().ToString());
    out << ",\"masterGuid\":";
    AppendJsonString(out, master->GetGUID().ToString());
    out << ",\"command\":";
    AppendJsonString(out, request.command);
    out << ",\"baselineActionSequence\":" << baselineActionSequence;
    out << ",\"baselineEconomySequence\":" << baseline.economy.sequence << '}';
    return Response::Success(out.str());
}

// Verification tooling: overwrite one already known skill so a scenario (capped gatherer, grey node)
// can be staged without waiting for the bot to reach it naturally. Unknown skills are refused so the
// tool cannot hand a bot a profession its career never planned.
Response BuildSetSkillResponse(Request const& request)
{
    Player* bot = ObjectAccessor::FindPlayer(MakePlayerGuid(request.botGuid));
    PlayerbotAI* botAI = ResolveBotAI(bot);
    if (!botAI)
        return Response::Failure(ErrorCode::BotNotFound, {});

    auto const skillId = static_cast<uint16>(RequestNumber(request, "skillId"));
    auto const value = static_cast<uint16>(RequestNumber(request, "value"));
    auto const maximum = static_cast<uint16>(RequestNumber(request, "maximum"));
    if (!bot->HasSkill(skillId))
        return Response::Failure(ErrorCode::InvalidSkill, {});

    uint16 const previousValue = bot->GetPureSkillValue(skillId);
    uint16 const previousMaximum = bot->GetPureMaxSkillValue(skillId);
    bot->SetSkill(skillId, static_cast<uint16>(maximum / SKILL_RANK_STEP), value, maximum);

    std::ostringstream out;
    out << "{\"botGuid\":";
    AppendJsonString(out, bot->GetGUID().ToString());
    out << ",\"skillId\":" << skillId << ",\"previousValue\":" << previousValue
        << ",\"previousMaximum\":" << previousMaximum << ",\"value\":" << bot->GetPureSkillValue(skillId)
        << ",\"maximum\":" << bot->GetPureMaxSkillValue(skillId) << '}';
    return Response::Success(out.str());
}

// Verification tooling: park the bot beside the nearest currently spawned gameobject of one entry on
// its own map, so a node interaction can be observed on demand. Pooled spawns that are not active are
// skipped; the bot would otherwise arrive at an empty spawn point.
Response BuildTeleportToGameObjectResponse(Request const& request)
{
    Player* bot = ObjectAccessor::FindPlayer(MakePlayerGuid(request.botGuid));
    PlayerbotAI* botAI = ResolveBotAI(bot);
    if (!botAI)
        return Response::Failure(ErrorCode::BotNotFound, {});
    if (!bot->IsInWorld() || bot->IsBeingTeleported() || bot->IsInCombat() || bot->isDead())
        return Response::Failure(ErrorCode::BotUnavailable, {});

    uint32 const entry = static_cast<uint32>(RequestNumber(request, "gameObjectEntry"));
    GameObjectData const* nearest = nullptr;
    float nearestDistance = 0.0f;
    for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
    {
        if (data.id != entry || data.mapid != bot->GetMapId())
            continue;
        if (sPoolMgr->IsPartOfAPool<GameObject>(spawnId) && !sPoolMgr->IsSpawnedObject<GameObject>(spawnId))
            continue;
        float const distance = bot->GetExactDist(data.posX, data.posY, data.posZ);
        if (!nearest || distance < nearestDistance)
        {
            nearest = &data;
            nearestDistance = distance;
        }
    }
    if (!nearest)
        return Response::Failure(ErrorCode::GameObjectNotFound, {});

    PlayerbotRecoveryResetStuckState(botAI);
    bot->m_taxi.ClearTaxiDestinations();
    // Two yards in front of the node keeps the bot inside interaction range without standing in it.
    float const x = nearest->posX + 2.0f * std::cos(nearest->orientation);
    float const y = nearest->posY + 2.0f * std::sin(nearest->orientation);
    bool const accepted = bot->TeleportTo(nearest->mapid, x, y, nearest->posZ, nearest->orientation);
    if (!accepted)
        return Response::Failure(ErrorCode::BotUnavailable, {});

    std::ostringstream out;
    out << "{\"botGuid\":";
    AppendJsonString(out, bot->GetGUID().ToString());
    out << ",\"gameObjectEntry\":" << entry << ",\"spawnId\":" << nearest->spawnId << ",\"mapId\":" << nearest->mapid
        << ",\"distanceBefore\":" << nearestDistance << '}';
    return Response::Success(out.str());
}
}  // namespace

bool PlayerbotVerificationResult::TryClaim()
{
    std::lock_guard<std::mutex> lock(mutex);
    if (abandoned || claimed)
        return false;
    claimed = true;
    return true;
}

bool PlayerbotVerificationResult::Abandon()
{
    std::lock_guard<std::mutex> lock(mutex);
    if (claimed || completed)
        return false;
    abandoned = true;
    return true;
}

void PlayerbotVerificationResult::Complete(Response completedResponse)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (completed)
            return;
        completed = true;
        response = std::move(completedResponse);
    }
    ready.notify_all();
}

bool PlayerbotVerificationResult::IsCompleted() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return completed;
}

std::optional<Response> PlayerbotVerificationResult::Wait(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(mutex);
    if (!ready.wait_for(lock, timeout, [this] { return completed; }))
        return std::nullopt;
    return response;
}

PlayerbotVerificationOperation::PlayerbotVerificationOperation(Request request,
                                                               std::shared_ptr<PlayerbotVerificationResult> result,
                                                               PlayerbotRecoveryPersistence recoveryPersistence)
    : request(std::move(request)), result(std::move(result)), recoveryPersistence(std::move(recoveryPersistence))
{
}

ObjectGuid PlayerbotVerificationOperation::GetBotGuid() const
{
    return request.botGuid ? MakePlayerGuid(request.botGuid) : ObjectGuid::Empty;
}

bool PlayerbotVerificationOperation::Execute()
{
    // A caller that already reached its deadline abandoned this operation. Running it now would
    // dispatch a command to the bot that no intervention record accounts for.
    if (!result || !result->TryClaim())
        return false;

    Response response;
    try
    {
        response = ExecuteVerificationOnWorldThread(request, recoveryPersistence);
    }
    catch (std::exception const& error)
    {
        LOG_ERROR("playerbots", "Playerbot verification operation failed: {}", error.what());
        response = request.operation == Operation::Recover
                       ? RecoveryResponse(request, RecoveryOutcome::RecoveryFailed, RecoveryReason::InternalError,
                                          RecoveryMutationState::UnknownAfterExecutionStarted)
                       : Response::Failure(ErrorCode::InternalError, {});
    }
    catch (...)
    {
        response = request.operation == Operation::Recover
                       ? RecoveryResponse(request, RecoveryOutcome::RecoveryFailed, RecoveryReason::InternalError,
                                          RecoveryMutationState::UnknownAfterExecutionStarted)
                       : Response::Failure(ErrorCode::InternalError, {});
    }

    bool const ok = response.ok;
    result->Complete(std::move(response));
    return ok;
}

void SetPlayerbotVerificationAcceptingRequests(bool accepting) { acceptingRequests.store(accepting); }

bool IsPlayerbotVerificationAcceptingRequests() { return acceptingRequests.load(); }

Response ExecuteVerificationOnWorldThread(Request const& request, PlayerbotRecoveryPersistence recoveryPersistence)
{
    switch (request.operation)
    {
        case Operation::Status:
            return BuildStatusResponse();
        case Operation::List:
            return BuildListResponse(request);
        case Operation::Inspect:
            return BuildInspectResponse(request);
        case Operation::Anomalies:
            return BuildAnomaliesResponse(request);
        case Operation::Check:
            return BuildCheckResponse(request);
        case Operation::Command:
            return BuildCommandResponse(request);
        case Operation::Recover:
            return BuildRecoverResponse(request, recoveryPersistence);
        case Operation::SetSkill:
            return BuildSetSkillResponse(request);
        case Operation::TeleportToGameObject:
            return BuildTeleportToGameObjectResponse(request);
    }
    return Response::Failure(ErrorCode::UnknownOperation, {});
}

Response DispatchVerificationRequest(Request const& request, std::chrono::milliseconds timeout,
                                     PlayerbotRecoveryPersistence recoveryPersistence)
{
    if (!IsPlayerbotVerificationAcceptingRequests())
        return request.operation == Operation::Recover
                   ? RecoveryResponse(request, RecoveryOutcome::RecoveryFailed, RecoveryReason::ShuttingDown)
                   : Response::Failure(ErrorCode::Shutdown, {});

    auto result = std::make_shared<PlayerbotVerificationResult>();
    if (!PlayerbotWorldThreadProcessor::instance().QueueOperation(
            std::make_unique<PlayerbotVerificationOperation>(request, result, std::move(recoveryPersistence))))
        return request.operation == Operation::Recover
                   ? RecoveryResponse(request, RecoveryOutcome::RecoveryFailed, RecoveryReason::QueueFull)
                   : Response::Failure(ErrorCode::QueueFull, {});

    std::optional<Response> response = result->Wait(timeout);
    if (response)
        return *response;

    // Abandon first, so a still queued operation can never dispatch after this answer is sent.
    if (result->Abandon())
        return request.operation == Operation::Recover ? RecoveryResponse(request, RecoveryOutcome::RecoveryTimedOut,
                                                                          RecoveryReason::QueueTimeoutBeforeClaim)
                                                       : Response::Failure(ErrorCode::Timeout, {});

    // Execution already started, so wait a bounded grace and report what the bot actually did.
    response = result->Wait(PLAYERBOT_VERIFICATION_COMPLETION_GRACE);
    if (!response)
        return request.operation == Operation::Recover
                   ? RecoveryResponse(request, RecoveryOutcome::RecoveryTimedOut,
                                      RecoveryReason::ExecutionTimeoutAfterClaim,
                                      RecoveryMutationState::UnknownAfterExecutionStarted)
                   : Response::Failure(ErrorCode::Timeout, {});
    return *response;
}

bool EvaluateVerificationCondition(PlayerbotVerificationInspection const& inspection, Request const& request)
{
    std::string const& condition = request.condition;

    if (condition == "transport_attached")
    {
        if (!inspection.transport.attached)
            return false;
        uint64 const entry = RequestNumber(request, "transportEntry");
        return !entry || inspection.transport.entry == entry;
    }
    if (condition == "transport_detached")
        return !inspection.transport.attached;
    if (condition == "map")
        return inspection.position.mapId == RequestNumber(request, "mapId");
    if (condition == "money_at_most")
        return inspection.moneyCopper <= RequestNumber(request, "maximumCopper");
    if (condition == "money_decrease")
        return inspection.moneyCopper < RequestNumber(request, "baselineCopper");
    if (condition == "known_recipe")
    {
        uint64 const spellId = RequestNumber(request, "spellId");
        return std::find(inspection.knownRecipeSpellIds.begin(), inspection.knownRecipeSpellIds.end(), spellId) !=
               inspection.knownRecipeSpellIds.end();
    }
    if (condition == "profession_skill")
    {
        uint64 const skillId = RequestNumber(request, "skillId");
        uint64 const minimumValue = RequestNumber(request, "minimumValue");
        auto const skill =
            std::find_if(inspection.skills.begin(), inspection.skills.end(),
                         [skillId](PlayerbotInspectionSkill const& candidate) { return candidate.id == skillId; });
        return skill != inspection.skills.end() && skill->value >= minimumValue;
    }
    if (condition == "inventory")
    {
        uint64 const itemId = RequestNumber(request, "itemId");
        uint64 const minimumCount = RequestNumber(request, "minimumCount");
        auto const item =
            std::find_if(inspection.inventory.begin(), inspection.inventory.end(),
                         [itemId](PlayerbotInspectionItem const& candidate) { return candidate.itemId == itemId; });
        return item != inspection.inventory.end() && item->count >= minimumCount;
    }
    if (condition == "action")
    {
        uint64 const afterSequence = RequestNumber(request, "afterSequence");
        std::string const actionName = RequestString(request, "actionName");
        std::string const actionResult = RequestString(request, "actionResult");
        for (std::size_t index = 0; index < inspection.actionHistory.count; ++index)
        {
            PlayerbotVerificationActionAttempt const& attempt = inspection.actionHistory.attempts[index];
            if (attempt.sequence <= afterSequence || !ActionAttemptNameMatches(attempt, actionName))
                continue;
            if (actionResult == "either" || (actionResult == "success") == attempt.success)
                return true;
        }
        return false;
    }
    if (condition == "economy")
    {
        if (inspection.economy.sequence <= RequestNumber(request, "afterSequence"))
            return false;
        return EconomyOutcomeToken(inspection.economy.outcome) == RequestString(request, "economyOutcome");
    }

    return false;
}
