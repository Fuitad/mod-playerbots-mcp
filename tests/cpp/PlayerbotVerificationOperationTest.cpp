/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "AreaDefines.h"
#include "Bot/Engine/AiObjectContext.h"
#include "Bot/MCP/PlayerbotVerificationOperation.h"
#include "Bot/MCP/PlayerbotVerificationProtocol.h"
#include "Bot/PlayerbotAI.h"
#include "Bot/PlayerbotMgr.h"
#include "Bot/Telemetry/PlayerbotInspector.h"
#include "Bot/Telemetry/PlayerbotTelemetryState.h"
#include "Bot/Telemetry/PlayerbotVerificationState.h"
#include "CharacterCache.h"
#include "DBCStores.h"
#include "IntegrationTestFixture.h"
#include "ObjectAccessor.h"
#include "Script/WorldThr/PlayerbotWorldThreadProcessor.h"
#include "gtest/gtest.h"

using namespace PlayerbotVerification;

namespace
{
PlayerbotVerificationState& VerificationState(PlayerbotAI* botAI)
{
    return GetPlayerbotTelemetryStateStore().Get(botAI->GetBot()->GetGUID().GetCounter())->verification;
}

constexpr std::chrono::milliseconds TEST_DEADLINE{2000};
constexpr std::chrono::milliseconds SHORT_DEADLINE{50};

class NoopOperation : public PlayerbotOperation
{
public:
    bool Execute() override { return true; }
    std::string GetName() const override { return "Queue filler"; }
};

class PlayerbotVerificationOperationTest : public IntegrationTestFixture
{
protected:
    void SetUp() override
    {
        IntegrationTestFixture::SetUp();
        previousEnabled = sPlayerbotAIConfig.enabled;
        previousCommandPrefix = sPlayerbotAIConfig.commandPrefix;
        previousCommandSeparator = sPlayerbotAIConfig.commandSeparator;
        sPlayerbotAIConfig.enabled = true;

        // The fixture never runs PlayerbotAIConfig::Initialize, so these carry the production
        // defaults explicitly. An empty separator would make HandleCommand's split loop forever.
        sPlayerbotAIConfig.commandSeparator = "\\\\";

        // A configured prefix makes every unprefixed whisper hit HandleCommand's silent rejection
        // path. That keeps the bridge contract testable without the live world state that full
        // command parsing needs, and it is exactly the case the bridge must not call "accepted".
        sPlayerbotAIConfig.commandPrefix = "bot ";

        static bool contextsBuilt = false;
        if (!contextsBuilt)
        {
            AiObjectContext::BuildAllSharedContexts();
            contextsBuilt = true;
        }
        PlayerbotWorldThreadProcessor::instance().ClearQueue();
        SetPlayerbotVerificationAcceptingRequests(true);
    }

    void TearDown() override
    {
        SetPlayerbotVerificationAcceptingRequests(false);
        PlayerbotWorldThreadProcessor::instance().ClearQueue();

        for (TestPlayer* player : registeredPlayers)
        {
            sPlayerbotsMgr.RemovePlayerBotData(player->GetGUID(), true);
            ObjectAccessor::RemoveObject(static_cast<Player*>(player));
        }
        registeredPlayers.clear();
        for (auto const& [guid, name] : cachedCharacters)
            sCharacterCache->DeleteCharacterCacheEntry(guid, name);
        cachedCharacters.clear();

        sPlayerbotAIConfig.commandPrefix = previousCommandPrefix;
        sPlayerbotAIConfig.commandSeparator = previousCommandSeparator;
        sPlayerbotAIConfig.enabled = previousEnabled;
        IntegrationTestFixture::TearDown();
    }

    TestPlayer* AddRealPlayer(ObjectGuid::LowType guid, std::string const& name)
    {
        TestPlayer* player = CreateTestPlayer(guid, name);
        ObjectAccessor::AddObject(static_cast<Player*>(player));
        registeredPlayers.push_back(player);
        return player;
    }

    PlayerbotAI* AddBot(TestPlayer* bot, Player* master)
    {
        sPlayerbotsMgr.AddPlayerbotData(bot, true);
        PlayerbotAI* botAI = sPlayerbotsMgr.GetPlayerbotAI(bot);
        EXPECT_NE(botAI, nullptr);
        if (botAI)
            botAI->SetMaster(master);
        return botAI;
    }

    // Pumps the world thread while the socket side waits for its bounded result.
    static Response DispatchWithPump(Request const& request)
    {
        std::future<Response> pending =
            std::async(std::launch::async, [request] { return DispatchVerificationRequest(request, TEST_DEADLINE); });

        while (pending.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready)
            PlayerbotWorldThreadProcessor::instance().Update(100);

        return pending.get();
    }

    static Request CommandRequest(uint32 botGuid, uint32 masterGuid, std::string command)
    {
        Request request;
        request.requestId = 1;
        request.operation = Operation::Command;
        request.botGuid = botGuid;
        request.masterGuid = masterGuid;
        request.command = std::move(command);
        return request;
    }

    static Request SimpleRequest(Operation operation, uint32 botGuid = 0)
    {
        Request request;
        request.requestId = 2;
        request.operation = operation;
        request.botGuid = botGuid;
        return request;
    }

    static Request RecoveryRequest(uint32 botGuid, uint64 requestId = 3)
    {
        Request request;
        request.requestId = requestId;
        request.operation = Operation::Recover;
        request.botGuid = botGuid;
        request.destination = "homebind";
        return request;
    }

    void AddCachedCharacter(ObjectGuid::LowType guid, std::string const& name)
    {
        ObjectGuid const playerGuid = ObjectGuid::Create<HighGuid::Player>(guid);
        sCharacterCache->AddCharacterCacheEntry(playerGuid, guid, name, GENDER_MALE, RACE_HUMAN, CLASS_WARRIOR, 1);
        cachedCharacters.emplace_back(playerGuid, name);
    }

    std::vector<TestPlayer*> registeredPlayers;
    std::vector<std::pair<ObjectGuid, std::string>> cachedCharacters;
    bool previousEnabled = false;
    std::string previousCommandPrefix;
    std::string previousCommandSeparator;
};
}  // namespace

TEST_F(PlayerbotVerificationOperationTest, RecoveryDistinguishesMissingOfflineOutOfWorldAndUnmanagedTargets)
{
    Response const missing = ExecuteVerificationOnWorldThread(RecoveryRequest(999));
    ASSERT_TRUE(missing.ok);
    EXPECT_NE(missing.resultJson.find(R"("outcome":"bot_not_found")"), std::string::npos);
    EXPECT_NE(missing.resultJson.find(R"("reason":"character_not_found")"), std::string::npos);

    AddCachedCharacter(998, "OfflineRecoveryBot");
    Response const offline = ExecuteVerificationOnWorldThread(RecoveryRequest(998));
    ASSERT_TRUE(offline.ok);
    EXPECT_NE(offline.resultJson.find(R"("outcome":"bot_not_available")"), std::string::npos);
    EXPECT_NE(offline.resultJson.find(R"("reason":"character_offline")"), std::string::npos);

    TestPlayer* transitional = AddRealPlayer(997, "TransitionalRecoveryBot");
    transitional->RemoveFromWorld();
    Response const outOfWorld = ExecuteVerificationOnWorldThread(RecoveryRequest(997));
    ASSERT_TRUE(outOfWorld.ok);
    EXPECT_NE(outOfWorld.resultJson.find(R"("reason":"character_not_in_world")"), std::string::npos);

    AddRealPlayer(996, "UnmanagedRecoveryPlayer");
    Response const notManaged = ExecuteVerificationOnWorldThread(RecoveryRequest(996));
    ASSERT_TRUE(notManaged.ok);
    EXPECT_NE(notManaged.resultJson.find(R"("outcome":"not_managed_playerbot")"), std::string::npos);
    EXPECT_NE(notManaged.resultJson.find(R"("reason":"playerbot_ai_missing")"), std::string::npos);
}

TEST_F(PlayerbotVerificationOperationTest, ManagedBotWithoutMasterRecoversAndReportsTruthfulAuditFields)
{
    TestPlayer* bot = AddRealPlayer(30, "RecoveryBot");
    bot->SetName("RecoveryBot");
    PlayerbotAI* botAI = AddBot(bot, nullptr);
    ASSERT_NE(botAI, nullptr);
    ASSERT_EQ(botAI->GetMaster(), nullptr);

    uint32 const mapId = GetTestMap()->GetId();
    bot->Relocate(100.0f, 110.0f, 120.0f, 1.0f);
    bot->m_homebindMapId = mapId;
    bot->m_homebindX = 20.0f;
    bot->m_homebindY = 30.0f;
    bot->m_homebindZ = 40.0f;

    uint32 saveRequests = 0;
    Response const response = ExecuteVerificationOnWorldThread(RecoveryRequest(30, 77),
                                                               [&saveRequests, bot](Player* saved)
                                                               {
                                                                   EXPECT_EQ(saved, bot);
                                                                   ++saveRequests;
                                                               });

    ASSERT_TRUE(response.ok);
    ASSERT_TRUE(response.recoveryAudit.has_value());
    EXPECT_EQ(saveRequests, 1U);
    EXPECT_NE(response.resultJson.find(R"("operation":"recover")"), std::string::npos);
    EXPECT_NE(response.resultJson.find(R"("requestId":77)"), std::string::npos);
    EXPECT_NE(response.resultJson.find(R"("botGuid":30)"), std::string::npos);
    EXPECT_NE(response.resultJson.find(R"("botName":"RecoveryBot")"), std::string::npos);
    EXPECT_NE(response.resultJson.find(R"("outcome":"recovered")"), std::string::npos);
    EXPECT_NE(response.resultJson.find(R"("reason":"homebind_teleport_accepted")"), std::string::npos);
    EXPECT_NE(response.resultJson.find(R"("beforePosition":{"mapId":)"), std::string::npos);
    EXPECT_NE(response.resultJson.find(R"("acceptedDestination":{"mapId":)"), std::string::npos);
    EXPECT_NE(response.resultJson.find(R"("observedPosition":{"mapId":)"), std::string::npos);
    EXPECT_NE(response.resultJson.find(R"("observedAtDestination":false)"), std::string::npos);
    EXPECT_NE(response.resultJson.find(R"("movementReset":true)"), std::string::npos);
    EXPECT_NE(response.resultJson.find(R"("travelReset":true)"), std::string::npos);
    EXPECT_NE(response.resultJson.find(R"("taxiReset":true)"), std::string::npos);
    EXPECT_NE(response.resultJson.find(R"("mutationState":"completed")"), std::string::npos);
    EXPECT_NE(response.resultJson.find(R"("persistenceState":"deferred")"), std::string::npos);

    Response const repeated =
        ExecuteVerificationOnWorldThread(RecoveryRequest(30, 78), [&saveRequests](Player*) { ++saveRequests; });
    ASSERT_TRUE(repeated.ok);
    EXPECT_EQ(saveRequests, 1U);
    EXPECT_NE(repeated.resultJson.find(R"("outcome":"already_at_homebind")"), std::string::npos);
    EXPECT_NE(repeated.resultJson.find(R"("reason":"pending_homebind")"), std::string::npos);
    EXPECT_NE(repeated.resultJson.find(R"("movementReset":false)"), std::string::npos);
    EXPECT_NE(repeated.resultJson.find(R"("persistenceState":"not_requested")"), std::string::npos);
}

TEST_F(PlayerbotVerificationOperationTest, RecoveryMapsEveryProtectedStateWithoutMutation)
{
    using ConfigureState = std::function<void(TestPlayer*)>;
    struct Case
    {
        char const* name = nullptr;
        char const* reason = nullptr;
        ConfigureState configure;
    };
    std::vector<Case> const cases = {
        {"dead", "dead", [](TestPlayer* bot) { bot->setDeathState(DeathState::Dead); }},
        {"combat", "in_combat", [this](TestPlayer* bot) { bot->SetInCombatWith(AddRealPlayer(890, "RecoveryEnemy")); }},
        {"rooted", "rooted", [](TestPlayer* bot) { bot->AddUnitMovementFlag(MOVEMENTFLAG_ROOT); }},
        {"flight", "in_flight", [](TestPlayer* bot) { bot->AddUnitState(UNIT_STATE_IN_FLIGHT); }},
        {"battleground queue", "battleground_queue",
         [](TestPlayer* bot) { bot->AddBattlegroundQueueId(BATTLEGROUND_QUEUE_WS); }},
        {"battleground", "battleground",
         [](TestPlayer* bot) { bot->SetBattlegroundId(99, BATTLEGROUND_WS, 0, false, false, TEAM_ALLIANCE); }},
        {"arena", "arena",
         [](TestPlayer* bot) { bot->SetBattlegroundId(100, BATTLEGROUND_AA, 0, false, false, TEAM_ALLIANCE); }},
        {"transport", "on_transport", [](TestPlayer* bot) { bot->AddUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT); }},
        {"teleport", "teleport_in_progress",
         [](TestPlayer* bot)
         {
             bot->GetTeleportDest().WorldRelocate(bot->m_homebindMapId, 60.0f, 70.0f, 80.0f, 0.0f);
             bot->SetSemaphoreTeleportNear(1);
         }},
    };

    ObjectGuid::LowType guid = 100;
    for (Case const& testCase : cases)
    {
        TestPlayer* bot = AddRealPlayer(guid++, testCase.name);
        ASSERT_NE(AddBot(bot, nullptr), nullptr);
        bot->Relocate(100.0f, 110.0f, 120.0f, 1.0f);
        bot->m_homebindMapId = GetTestMap()->GetId();
        bot->m_homebindX = 20.0f;
        bot->m_homebindY = 30.0f;
        bot->m_homebindZ = 40.0f;
        bot->m_taxi.AddTaxiDestination(1);
        testCase.configure(bot);

        uint32 saveRequests = 0;
        Response const response = ExecuteVerificationOnWorldThread(RecoveryRequest(bot->GetGUID().GetCounter()),
                                                                   [&saveRequests](Player*) { ++saveRequests; });

        SCOPED_TRACE(testCase.name);
        ASSERT_TRUE(response.ok);
        ASSERT_TRUE(response.recoveryAudit.has_value());
        EXPECT_NE(response.resultJson.find(R"("outcome":"bot_not_available")"), std::string::npos);
        EXPECT_NE(response.resultJson.find(std::string(R"("reason":")") + testCase.reason + '"'), std::string::npos);
        EXPECT_NE(response.resultJson.find(R"("mutationState":"not_started")"), std::string::npos);
        EXPECT_NE(response.resultJson.find(R"("persistenceState":"not_requested")"), std::string::npos);
        EXPECT_EQ(saveRequests, 0U);
        EXPECT_FALSE(bot->m_taxi.empty());
    }
}

TEST_F(PlayerbotVerificationOperationTest, RecoveryReportsInvalidHomebindAndTeleportRejection)
{
    TestPlayer* invalid = AddRealPlayer(210, "InvalidHomebindBot");
    ASSERT_NE(AddBot(invalid, nullptr), nullptr);
    invalid->Relocate(100.0f, 110.0f, 120.0f, 1.0f);
    invalid->m_homebindMapId = MAPID_INVALID;

    uint32 saveRequests = 0;
    Response const invalidResponse =
        ExecuteVerificationOnWorldThread(RecoveryRequest(210), [&saveRequests](Player*) { ++saveRequests; });
    ASSERT_TRUE(invalidResponse.ok);
    ASSERT_TRUE(invalidResponse.recoveryAudit.has_value());
    EXPECT_NE(invalidResponse.resultJson.find(R"("outcome":"recovery_failed")"), std::string::npos);
    EXPECT_NE(invalidResponse.resultJson.find(R"("reason":"invalid_homebind")"), std::string::npos);
    EXPECT_NE(invalidResponse.resultJson.find(R"("mutationState":"not_started")"), std::string::npos);

    TestPlayer* rejected = AddRealPlayer(211, "RejectedHomebindBot");
    ASSERT_NE(AddBot(rejected, nullptr), nullptr);
    rejected->Relocate(100.0f, 110.0f, 120.0f, 1.0f);
    rejected->m_homebindMapId = MAP_WARSONG_GULCH;
    rejected->m_homebindX = 1500.0f;
    rejected->m_homebindY = 1500.0f;
    rejected->m_homebindZ = 30.0f;
    rejected->m_taxi.AddTaxiDestination(1);

    bool const installedBattlegroundEntry = sMapStore.LookupEntry(MAP_WARSONG_GULCH) == nullptr;
    if (installedBattlegroundEntry)
    {
        auto* battlegroundEntry = new MapEntry{};
        battlegroundEntry->MapID = MAP_WARSONG_GULCH;
        battlegroundEntry->map_type = MAP_BATTLEGROUND;
        sMapStore.SetEntry(MAP_WARSONG_GULCH, battlegroundEntry);
    }

    Response const rejectedResponse =
        ExecuteVerificationOnWorldThread(RecoveryRequest(211), [&saveRequests](Player*) { ++saveRequests; });
    if (installedBattlegroundEntry)
        sMapStore.SetEntry(MAP_WARSONG_GULCH, nullptr);
    ASSERT_TRUE(rejectedResponse.ok);
    ASSERT_TRUE(rejectedResponse.recoveryAudit.has_value());
    EXPECT_NE(rejectedResponse.resultJson.find(R"("outcome":"recovery_failed")"), std::string::npos);
    EXPECT_NE(rejectedResponse.resultJson.find(R"("reason":"teleport_rejected")"), std::string::npos);
    EXPECT_NE(rejectedResponse.resultJson.find(R"("mutationState":"completed")"), std::string::npos);
    EXPECT_NE(rejectedResponse.resultJson.find(R"("movementReset":true)"), std::string::npos);
    EXPECT_TRUE(rejected->m_taxi.empty());
    EXPECT_EQ(saveRequests, 0U);
}

TEST_F(PlayerbotVerificationOperationTest, RecoveryInfrastructureFailuresReturnAuditableTypedResults)
{
    TestPlayer* bot = AddRealPlayer(220, "TimedRecoveryBot");
    ASSERT_NE(AddBot(bot, nullptr), nullptr);
    bot->Relocate(100.0f, 110.0f, 120.0f, 1.0f);
    bot->m_homebindMapId = GetTestMap()->GetId();
    bot->m_homebindX = 20.0f;
    bot->m_homebindY = 30.0f;
    bot->m_homebindZ = 40.0f;

    SetPlayerbotVerificationAcceptingRequests(false);
    Response const shutdown = DispatchVerificationRequest(RecoveryRequest(220, 80), SHORT_DEADLINE);
    SetPlayerbotVerificationAcceptingRequests(true);
    ASSERT_TRUE(shutdown.ok);
    ASSERT_TRUE(shutdown.recoveryAudit.has_value());
    EXPECT_NE(shutdown.resultJson.find(R"("reason":"shutting_down")"), std::string::npos);
    EXPECT_EQ(PlayerbotWorldThreadProcessor::instance().GetQueueSize(), 0U);

    Response const timeout = DispatchVerificationRequest(RecoveryRequest(220, 81), SHORT_DEADLINE);
    ASSERT_TRUE(timeout.ok);
    ASSERT_TRUE(timeout.recoveryAudit.has_value());
    EXPECT_NE(timeout.resultJson.find(R"("outcome":"recovery_timed_out")"), std::string::npos);
    EXPECT_NE(timeout.resultJson.find(R"("reason":"queue_timeout_before_claim")"), std::string::npos);
    EXPECT_NE(timeout.resultJson.find(R"("mutationState":"not_started")"), std::string::npos);
    EXPECT_FALSE(bot->IsBeingTeleported());
    PlayerbotWorldThreadProcessor::instance().Update(100);
    EXPECT_FALSE(bot->IsBeingTeleported());

    for (uint32 index = 0; index < 10000; ++index)
        PlayerbotWorldThreadProcessor::instance().QueueOperation(std::make_unique<NoopOperation>());
    Response const queueFull = DispatchVerificationRequest(RecoveryRequest(220, 82), SHORT_DEADLINE);
    ASSERT_TRUE(queueFull.ok);
    ASSERT_TRUE(queueFull.recoveryAudit.has_value());
    EXPECT_NE(queueFull.resultJson.find(R"("reason":"queue_full")"), std::string::npos);
    PlayerbotWorldThreadProcessor::instance().ClearQueue();
}

TEST_F(PlayerbotVerificationOperationTest, ClaimedRecoveryTimeoutReportsUnknownAndMayCompleteLater)
{
    TestPlayer* bot = AddRealPlayer(230, "ClaimedRecoveryBot");
    ASSERT_NE(AddBot(bot, nullptr), nullptr);
    bot->Relocate(100.0f, 110.0f, 120.0f, 1.0f);
    bot->m_homebindMapId = GetTestMap()->GetId();
    bot->m_homebindX = 20.0f;
    bot->m_homebindY = 30.0f;
    bot->m_homebindZ = 40.0f;

    std::promise<void> persistenceStarted;
    std::future<void> started = persistenceStarted.get_future();
    std::promise<void> releasePersistence;
    std::shared_future<void> release = releasePersistence.get_future().share();
    std::atomic<uint32> saveRequests{0};
    Request const request = RecoveryRequest(230, 90);
    std::future<Response> pending =
        std::async(std::launch::async,
                   [&, request]
                   {
                       return DispatchVerificationRequest(request, SHORT_DEADLINE,
                                                          [&](Player* saved)
                                                          {
                                                              EXPECT_EQ(saved, bot);
                                                              ++saveRequests;
                                                              persistenceStarted.set_value();
                                                              release.wait();
                                                          });
                   });

    while (PlayerbotWorldThreadProcessor::instance().GetQueueSize() == 0 &&
           pending.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready)
    {
    }
    std::future<void> pump =
        std::async(std::launch::async, [] { PlayerbotWorldThreadProcessor::instance().Update(100); });
    bool const didStart = started.wait_for(TEST_DEADLINE) == std::future_status::ready;
    Response const timeout = pending.get();
    releasePersistence.set_value();
    pump.get();

    ASSERT_TRUE(didStart);
    ASSERT_TRUE(timeout.ok);
    ASSERT_TRUE(timeout.recoveryAudit.has_value());
    EXPECT_NE(timeout.resultJson.find(R"("outcome":"recovery_timed_out")"), std::string::npos);
    EXPECT_NE(timeout.resultJson.find(R"("reason":"execution_timeout_after_claim")"), std::string::npos);
    EXPECT_NE(timeout.resultJson.find(R"("mutationState":"unknown_after_execution_started")"), std::string::npos);
    EXPECT_EQ(timeout.resultJson.find("acceptedDestination"), std::string::npos);
    EXPECT_EQ(timeout.resultJson.find("observedPosition"), std::string::npos);
    EXPECT_NE(timeout.resultJson.find(R"("persistenceState":"not_requested")"), std::string::npos);
    EXPECT_EQ(saveRequests.load(), 1U);
    EXPECT_TRUE(bot->IsBeingTeleported());
}

TEST_F(PlayerbotVerificationOperationTest, ConcurrentDuplicateRecoveryRequestsAcceptAtMostOneTeleport)
{
    TestPlayer* bot = AddRealPlayer(240, "DuplicateRecoveryBot");
    ASSERT_NE(AddBot(bot, nullptr), nullptr);
    bot->Relocate(100.0f, 110.0f, 120.0f, 1.0f);
    bot->m_homebindMapId = GetTestMap()->GetId();
    bot->m_homebindX = 20.0f;
    bot->m_homebindY = 30.0f;
    bot->m_homebindZ = 40.0f;

    std::atomic<uint32> saveRequests{0};
    auto firstResult = std::make_shared<PlayerbotVerificationResult>();
    auto secondResult = std::make_shared<PlayerbotVerificationResult>();
    PlayerbotRecoveryPersistence persistence = [&](Player* saved)
    {
        EXPECT_EQ(saved, bot);
        ++saveRequests;
    };
    ASSERT_TRUE(PlayerbotWorldThreadProcessor::instance().QueueOperation(
        std::make_unique<PlayerbotVerificationOperation>(RecoveryRequest(240, 100), firstResult, persistence)));
    ASSERT_TRUE(PlayerbotWorldThreadProcessor::instance().QueueOperation(
        std::make_unique<PlayerbotVerificationOperation>(RecoveryRequest(240, 101), secondResult, persistence)));

    PlayerbotWorldThreadProcessor::instance().Update(100);
    std::optional<Response> const first = firstResult->Wait(TEST_DEADLINE);
    std::optional<Response> const second = secondResult->Wait(TEST_DEADLINE);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(first->recoveryAudit.has_value());
    ASSERT_TRUE(second->recoveryAudit.has_value());
    EXPECT_NE(first->resultJson.find(R"("outcome":"recovered")"), std::string::npos);
    EXPECT_NE(second->resultJson.find(R"("outcome":"already_at_homebind")"), std::string::npos);
    EXPECT_NE(second->resultJson.find(R"("reason":"pending_homebind")"), std::string::npos);
    EXPECT_EQ(saveRequests.load(), 1U);
}

TEST_F(PlayerbotVerificationOperationTest, QueuedOperationAnswersAndMasterAuthorizationIsExact)
{
    TestPlayer* master = AddRealPlayer(1, "VerificationMaster");
    TestPlayer* stranger = AddRealPlayer(2, "VerificationStranger");
    TestPlayer* bot = AddRealPlayer(3, "VerificationBot");
    ASSERT_NE(AddBot(bot, master), nullptr);

    Response const wrongMaster = DispatchWithPump(CommandRequest(3, stranger->GetGUID().GetCounter(), "follow"));
    EXPECT_FALSE(wrongMaster.ok);
    EXPECT_EQ(wrongMaster.error.code, ErrorCode::InvalidRelationship);

    Response const dispatched = DispatchWithPump(CommandRequest(3, master->GetGUID().GetCounter(), "follow"));
    EXPECT_TRUE(dispatched.ok);
    EXPECT_NE(dispatched.resultJson.find(R"("dispatched":true)"), std::string::npos);
}

TEST_F(PlayerbotVerificationOperationTest, TypedErrorsDistinguishMissingBotMissingMasterAndBotMaster)
{
    TestPlayer* master = AddRealPlayer(1, "VerificationMaster");
    TestPlayer* bot = AddRealPlayer(3, "VerificationBot");
    TestPlayer* otherBot = AddRealPlayer(4, "VerificationOtherBot");
    ASSERT_NE(AddBot(bot, master), nullptr);
    ASSERT_NE(AddBot(otherBot, master), nullptr);

    // A player without a PlayerbotAI is not a bot.
    EXPECT_EQ(DispatchWithPump(CommandRequest(1, 1, "follow")).error.code, ErrorCode::BotNotFound);
    EXPECT_EQ(DispatchWithPump(SimpleRequest(Operation::Inspect, 999)).error.code, ErrorCode::BotNotFound);
    EXPECT_EQ(DispatchWithPump(CommandRequest(3, 999, "follow")).error.code, ErrorCode::MasterNotFound);
    EXPECT_EQ(DispatchWithPump(CommandRequest(3, 4, "follow")).error.code, ErrorCode::MasterIsBot);
}

TEST_F(PlayerbotVerificationOperationTest, DispatchedCommandReportsBaselineWithoutClaimingAcceptance)
{
    TestPlayer* master = AddRealPlayer(1, "VerificationMaster");
    TestPlayer* bot = AddRealPlayer(3, "VerificationBot");
    PlayerbotAI* botAI = AddBot(bot, master);
    ASSERT_NE(botAI, nullptr);

    VerificationState(botAI).RecordActionAttempt("follow", true, 10);
    VerificationState(botAI).RecordActionAttempt("greet", false, 20);
    uint64 const baseline = VerificationState(botAI).CopyActionHistory().totalCount;
    ASSERT_EQ(baseline, 2U);

    // The configured prefix makes HandleCommand reject this silently. The bridge still reports only
    // that it dispatched the whisper.
    Response const response = DispatchWithPump(CommandRequest(3, 1, "follow"));
    ASSERT_TRUE(response.ok);
    EXPECT_NE(response.resultJson.find(R"("dispatched":true)"), std::string::npos);
    EXPECT_NE(response.resultJson.find(R"("baselineActionSequence":2)"), std::string::npos);
    EXPECT_NE(response.resultJson.find(R"("baselineEconomySequence":0)"), std::string::npos);
    EXPECT_EQ(response.resultJson.find("accepted"), std::string::npos);
    EXPECT_EQ(response.resultJson.find("success"), std::string::npos);

    // A silently rejected command produces no action attempt of its own.
    EXPECT_EQ(VerificationState(botAI).CopyActionHistory().totalCount, baseline);
}

TEST_F(PlayerbotVerificationOperationTest, RejectedCommandLeavesActionBaselineUntouched)
{
    TestPlayer* master = AddRealPlayer(1, "VerificationMaster");
    TestPlayer* stranger = AddRealPlayer(2, "VerificationStranger");
    TestPlayer* bot = AddRealPlayer(3, "VerificationBot");
    PlayerbotAI* botAI = AddBot(bot, master);
    ASSERT_NE(botAI, nullptr);
    VerificationState(botAI).RecordActionAttempt("follow", true, 10);

    PlayerbotVerificationSnapshot const before = VerificationState(botAI).CopySnapshot();
    EXPECT_EQ(DispatchWithPump(CommandRequest(3, stranger->GetGUID().GetCounter(), "follow")).error.code,
              ErrorCode::InvalidRelationship);
    EXPECT_EQ(DispatchWithPump(CommandRequest(3, 999, "follow")).error.code, ErrorCode::MasterNotFound);

    EXPECT_EQ(VerificationState(botAI).CopySnapshot(), before);
}

TEST_F(PlayerbotVerificationOperationTest, ShutdownQueueFullAndTimeoutProduceDistinctErrors)
{
    TestPlayer* master = AddRealPlayer(1, "VerificationMaster");
    TestPlayer* bot = AddRealPlayer(3, "VerificationBot");
    PlayerbotAI* botAI = AddBot(bot, master);
    ASSERT_NE(botAI, nullptr);
    PlayerbotVerificationSnapshot const before = VerificationState(botAI).CopySnapshot();

    SetPlayerbotVerificationAcceptingRequests(false);
    Response const shutdown = DispatchVerificationRequest(CommandRequest(3, 1, "follow"), SHORT_DEADLINE);
    EXPECT_EQ(shutdown.error.code, ErrorCode::Shutdown);
    EXPECT_EQ(PlayerbotWorldThreadProcessor::instance().GetQueueSize(), 0U);
    SetPlayerbotVerificationAcceptingRequests(true);

    // Queued but never pumped.
    Response const timeout = DispatchVerificationRequest(SimpleRequest(Operation::Status), SHORT_DEADLINE);
    EXPECT_EQ(timeout.error.code, ErrorCode::Timeout);
    PlayerbotWorldThreadProcessor::instance().ClearQueue();

    for (uint32 index = 0; index < 10000; ++index)
        PlayerbotWorldThreadProcessor::instance().QueueOperation(std::make_unique<NoopOperation>());
    Response const queueFull = DispatchVerificationRequest(SimpleRequest(Operation::Status), SHORT_DEADLINE);
    EXPECT_EQ(queueFull.error.code, ErrorCode::QueueFull);
    PlayerbotWorldThreadProcessor::instance().ClearQueue();

    // None of the refused paths reached the bot.
    EXPECT_EQ(VerificationState(botAI).CopySnapshot(), before);
}

TEST_F(PlayerbotVerificationOperationTest, AbandonedCommandNeverReachesTheBot)
{
    TestPlayer* master = AddRealPlayer(1, "VerificationMaster");
    TestPlayer* stranger = AddRealPlayer(2, "VerificationStranger");
    TestPlayer* bot = AddRealPlayer(3, "VerificationBot");
    PlayerbotAI* botAI = AddBot(bot, master);
    ASSERT_NE(botAI, nullptr);
    PlayerbotVerificationSnapshot const before = VerificationState(botAI).CopySnapshot();

    // This request would answer with a typed relationship error if its body ever ran, so an
    // uncompleted result proves the abandoned operation never executed.
    auto abandonedResult = std::make_shared<PlayerbotVerificationResult>();
    PlayerbotVerificationOperation abandoned(CommandRequest(3, stranger->GetGUID().GetCounter(), "follow"),
                                             abandonedResult);
    EXPECT_TRUE(abandonedResult->Abandon());
    EXPECT_FALSE(abandoned.Execute());
    EXPECT_FALSE(abandonedResult->IsCompleted());

    // Claiming first models the world thread already executing, so abandonment must fail.
    auto claimedResult = std::make_shared<PlayerbotVerificationResult>();
    EXPECT_TRUE(claimedResult->TryClaim());
    EXPECT_FALSE(claimedResult->Abandon());

    // End to end: a timed out command stays queued, then must not dispatch when the pump catches up.
    Response const timedOut = DispatchVerificationRequest(CommandRequest(3, 1, "follow"), SHORT_DEADLINE);
    EXPECT_EQ(timedOut.error.code, ErrorCode::Timeout);
    PlayerbotWorldThreadProcessor::instance().Update(100);
    EXPECT_EQ(PlayerbotWorldThreadProcessor::instance().GetQueueSize(), 0U);
    EXPECT_EQ(VerificationState(botAI).CopySnapshot(), before);
}

TEST_F(PlayerbotVerificationOperationTest, TruncatedActionNameNeverSatisfiesAnExactCondition)
{
    std::string const storedPrefix(PLAYERBOT_VERIFICATION_ACTION_NAME_CAPACITY - 1, 'a');

    PlayerbotVerificationInspection inspection;
    inspection.actionHistory.count = 1;
    inspection.actionHistory.totalCount = 1;
    inspection.actionHistory.attempts[0].sequence = 5;
    inspection.actionHistory.attempts[0].success = true;
    inspection.actionHistory.attempts[0].nameTruncated = true;
    std::copy(storedPrefix.begin(), storedPrefix.end(), inspection.actionHistory.attempts[0].actionName.data());

    Request request;
    request.condition = "action";
    request.numbers["afterSequence"] = 4;
    request.strings["actionResult"] = "either";

    // Two different long names share the surviving prefix, so neither may be reported as an exact match.
    request.strings["actionName"] = storedPrefix + "b";
    EXPECT_FALSE(EvaluateVerificationCondition(inspection, request));
    request.strings["actionName"] = storedPrefix + "c";
    EXPECT_FALSE(EvaluateVerificationCondition(inspection, request));
    request.strings["actionName"] = storedPrefix;
    EXPECT_FALSE(EvaluateVerificationCondition(inspection, request));

    // An untruncated record of the same length still matches exactly.
    inspection.actionHistory.attempts[0].nameTruncated = false;
    EXPECT_TRUE(EvaluateVerificationCondition(inspection, request));
}

TEST_F(PlayerbotVerificationOperationTest, InspectionLeavesCareerAndEconomyObservationsUnchanged)
{
    TestPlayer* master = AddRealPlayer(1, "VerificationMaster");
    TestPlayer* bot = AddRealPlayer(3, "VerificationBot");
    PlayerbotAI* botAI = AddBot(bot, master);
    ASSERT_NE(botAI, nullptr);

    VerificationState(botAI).PublishCareer({
        .status = PlayerbotVerificationCareerStatus::Valid,
        .source = PlayerbotVerificationCareerSource::Saved,
        .version = 3u,
        .candidateToken = "candidate",
        .primarySkills = {164u, 165u},
        .secondarySkills = {185u},
        .spendingStyle = 2u,
        .marketEligible = true,
        .engagement = 70u,
    });
    VerificationState(botAI).PublishEconomy({
        .outcome = PlayerbotVerificationEconomyOutcome::Quarantined,
        .phase = PlayerbotVerificationEconomyPhase::Gather,
        .chainPublicId = "chn_0123456789abcdef",
        .operationIdentity = "gather:2770:42",
        .marketId = 2u,
        .itemFamily = "exact_reagent:2770",
        .workOrderSpellId = 1001u,
        .remainingQuantity = 4u,
        .claimAgeSeconds = 9u,
        .blockerCode = "missing_path",
        .consecutiveFailures = 5u,
        .cooldownSeconds = 320u,
        .nextEligibleTime = 12345u,
        .quarantined = true,
    });

    PlayerbotVerificationSnapshot const before = VerificationState(botAI).CopySnapshot();
    Response const first = DispatchWithPump(SimpleRequest(Operation::Inspect, 3));
    Response const second = DispatchWithPump(SimpleRequest(Operation::Inspect, 3));

    ASSERT_TRUE(first.ok);
    ASSERT_TRUE(second.ok);
    EXPECT_NE(first.resultJson.find(R"("status":"valid")"), std::string::npos);
    EXPECT_NE(first.resultJson.find(R"("outcome":"quarantined")"), std::string::npos);
    EXPECT_NE(first.resultJson.find(R"("phase":"gather")"), std::string::npos);
    EXPECT_NE(first.resultJson.find(R"("chainPublicId":"chn_0123456789abcdef")"), std::string::npos);
    EXPECT_NE(first.resultJson.find(R"("operationIdentity":"gather:2770:42")"), std::string::npos);
    EXPECT_NE(first.resultJson.find(R"("marketId":2)"), std::string::npos);
    EXPECT_NE(first.resultJson.find(R"("itemFamily":"exact_reagent:2770")"), std::string::npos);
    EXPECT_NE(first.resultJson.find(R"("remainingQuantity":4)"), std::string::npos);
    EXPECT_NE(first.resultJson.find(R"("claimAgeSeconds":9)"), std::string::npos);
    EXPECT_NE(first.resultJson.find(R"("blockerCode":"missing_path")"), std::string::npos);
    EXPECT_NE(first.resultJson.find(R"("consecutiveFailures":5)"), std::string::npos);
    EXPECT_NE(first.resultJson.find(R"("cooldownSeconds":320)"), std::string::npos);
    EXPECT_NE(first.resultJson.find(R"("quarantined":true)"), std::string::npos);
    EXPECT_NE(first.resultJson.find(R"("nextEligibleTime":12345)"), std::string::npos);
    EXPECT_EQ(VerificationState(botAI).CopySnapshot(), before);

    PlayerbotVerificationInspection inspection = PlayerbotInspector::BuildVerification(bot, botAI);
    Request request;
    request.condition = "economy";
    request.numbers["afterSequence"] = 0u;
    request.strings["economyOutcome"] = "quarantined";
    EXPECT_TRUE(EvaluateVerificationCondition(inspection, request));
    request.strings["economyOutcome"] = "unknown";
    EXPECT_FALSE(EvaluateVerificationCondition(inspection, request));
    EXPECT_EQ(VerificationState(botAI).CopySnapshot(), before);
}

TEST_F(PlayerbotVerificationOperationTest, ExactConditionsMatchDespiteDisplayTruncation)
{
    PlayerbotVerificationInspection inspection;
    inspection.moneyCopper = 500;
    inspection.position.mapId = 369;
    inspection.transport.attached = true;
    inspection.transport.entry = 176231;
    inspection.economy = {
        .sequence = 9,
        .outcome = PlayerbotVerificationEconomyOutcome::NoCandidate,
    };

    // The matching entries sit beyond every display cap.
    for (uint32 index = 0; index < 200; ++index)
        inspection.inventory.push_back({.itemId = 1000 + index, .name = "item", .count = index});
    for (uint32 index = 0; index < 200; ++index)
        inspection.skills.push_back({.id = 2000 + index, .name = "skill", .value = 300, .maximum = 450});
    for (uint32 index = 0; index < 2000; ++index)
        inspection.knownRecipeSpellIds.push_back(3000 + index);
    inspection.actionHistory.count = 1;
    inspection.actionHistory.totalCount = 1;
    inspection.actionHistory.attempts[0].sequence = 7;
    inspection.actionHistory.attempts[0].success = false;
    std::string const actionName = "craft item";
    std::copy(actionName.begin(), actionName.end(), inspection.actionHistory.attempts[0].actionName.data());

    std::string const serialized = PlayerbotInspector::SerializeVerification(inspection);
    EXPECT_NE(serialized.find(R"("truncated":true)"), std::string::npos);

    Request request;
    request.condition = "inventory";
    request.numbers["itemId"] = 1199;
    request.numbers["minimumCount"] = 199;
    EXPECT_TRUE(EvaluateVerificationCondition(inspection, request));
    request.numbers["minimumCount"] = 200;
    EXPECT_FALSE(EvaluateVerificationCondition(inspection, request));

    request = {};
    request.condition = "profession_skill";
    request.numbers["skillId"] = 2199;
    request.numbers["minimumValue"] = 300;
    EXPECT_TRUE(EvaluateVerificationCondition(inspection, request));

    request = {};
    request.condition = "known_recipe";
    request.numbers["spellId"] = 4999;
    EXPECT_TRUE(EvaluateVerificationCondition(inspection, request));
    request.numbers["spellId"] = 5000;
    EXPECT_FALSE(EvaluateVerificationCondition(inspection, request));

    request = {};
    request.condition = "money_decrease";
    request.numbers["baselineCopper"] = 501;
    EXPECT_TRUE(EvaluateVerificationCondition(inspection, request));
    request.numbers["baselineCopper"] = 500;
    EXPECT_FALSE(EvaluateVerificationCondition(inspection, request));

    request = {};
    request.condition = "money_at_most";
    request.numbers["maximumCopper"] = 500;
    EXPECT_TRUE(EvaluateVerificationCondition(inspection, request));

    request = {};
    request.condition = "action";
    request.numbers["afterSequence"] = 6;
    request.strings["actionName"] = "craft item";
    request.strings["actionResult"] = "failure";
    EXPECT_TRUE(EvaluateVerificationCondition(inspection, request));
    request.strings["actionResult"] = "success";
    EXPECT_FALSE(EvaluateVerificationCondition(inspection, request));
    request.strings["actionResult"] = "either";
    EXPECT_TRUE(EvaluateVerificationCondition(inspection, request));
    request.numbers["afterSequence"] = 7;
    EXPECT_FALSE(EvaluateVerificationCondition(inspection, request));

    request = {};
    request.condition = "economy";
    request.numbers["afterSequence"] = 8;
    request.strings["economyOutcome"] = "no_candidate";
    EXPECT_TRUE(EvaluateVerificationCondition(inspection, request));
    request.strings["economyOutcome"] = "operation";
    EXPECT_FALSE(EvaluateVerificationCondition(inspection, request));

    request = {};
    request.condition = "transport_attached";
    EXPECT_TRUE(EvaluateVerificationCondition(inspection, request));
    request.numbers["transportEntry"] = 176231;
    EXPECT_TRUE(EvaluateVerificationCondition(inspection, request));
    request.numbers["transportEntry"] = 1;
    EXPECT_FALSE(EvaluateVerificationCondition(inspection, request));

    request = {};
    request.condition = "transport_detached";
    EXPECT_FALSE(EvaluateVerificationCondition(inspection, request));

    request = {};
    request.condition = "map";
    request.numbers["mapId"] = 369;
    EXPECT_TRUE(EvaluateVerificationCondition(inspection, request));
    request.numbers["mapId"] = 0;
    EXPECT_FALSE(EvaluateVerificationCondition(inspection, request));
}

TEST_F(PlayerbotVerificationOperationTest, StatusAndListReportReadinessAndGuidOrderedPagination)
{
    TestPlayer* master = AddRealPlayer(1, "VerificationMaster");
    ASSERT_NE(AddBot(AddRealPlayer(5, "BotFive"), master), nullptr);
    ASSERT_NE(AddBot(AddRealPlayer(3, "BotThree"), master), nullptr);
    ASSERT_NE(AddBot(AddRealPlayer(7, "BotSeven"), master), nullptr);

    Response const status = DispatchWithPump(SimpleRequest(Operation::Status));
    ASSERT_TRUE(status.ok);
    EXPECT_NE(status.resultJson.find(R"("protocolSchemaVersion":2)"), std::string::npos);
    EXPECT_NE(status.resultJson.find(R"("inspectionSchemaVersion":3)"), std::string::npos);
    EXPECT_NE(status.resultJson.find(R"("moduleEnabled":true)"), std::string::npos);
    EXPECT_NE(status.resultJson.find(R"("queueAvailable":true)"), std::string::npos);
    EXPECT_NE(status.resultJson.find(R"("botCount":3)"), std::string::npos);

    Request listRequest = SimpleRequest(Operation::List);
    listRequest.afterGuid = 0;
    listRequest.limit = 2;
    Response const firstPage = DispatchWithPump(listRequest);
    ASSERT_TRUE(firstPage.ok);
    // TestPlayer carries no player name, so the GUID low part is the stable identity to assert on.
    std::size_t const thirdBot = firstPage.resultJson.find(R"("guidLow":3)");
    std::size_t const fifthBot = firstPage.resultJson.find(R"("guidLow":5)");
    ASSERT_NE(thirdBot, std::string::npos);
    ASSERT_NE(fifthBot, std::string::npos);
    EXPECT_LT(thirdBot, fifthBot);
    EXPECT_EQ(firstPage.resultJson.find(R"("guidLow":7)"), std::string::npos);
    EXPECT_NE(firstPage.resultJson.find(R"("nextAfterGuid":5)"), std::string::npos);
    EXPECT_NE(firstPage.resultJson.find(R"("hasMore":true)"), std::string::npos);
    EXPECT_NE(firstPage.resultJson.find(R"("totalCount":3)"), std::string::npos);

    listRequest.afterGuid = 5;
    Response const secondPage = DispatchWithPump(listRequest);
    ASSERT_TRUE(secondPage.ok);
    EXPECT_NE(secondPage.resultJson.find(R"("guidLow":7)"), std::string::npos);
    EXPECT_EQ(secondPage.resultJson.find(R"("guidLow":3)"), std::string::npos);
    EXPECT_NE(secondPage.resultJson.find(R"("hasMore":false)"), std::string::npos);
    EXPECT_NE(secondPage.resultJson.find(R"("relationshipValid":)"), std::string::npos);
    EXPECT_NE(secondPage.resultJson.find(R"("transportAttached":false)"), std::string::npos);
}

TEST_F(PlayerbotVerificationOperationTest, ListReplacesInvalidUtf8WithoutCorruptingValidNames)
{
    TestPlayer* master = AddRealPlayer(1, "VerificationMaster");
    TestPlayer* bot = AddRealPlayer(3, "TemporaryBotName");
    std::string name = "Bjorn";
    name.append("\xC3\xB6");
    name.push_back(static_cast<char>(0xFF));
    name.append("Bot");
    bot->SetName(name);
    ASSERT_NE(AddBot(bot, master), nullptr);

    Request request = SimpleRequest(Operation::List);
    request.limit = 1;
    Response const response = DispatchWithPump(request);

    ASSERT_TRUE(response.ok);
    EXPECT_NE(response.resultJson.find("Bjorn\xC3\xB6\xEF\xBF\xBD"
                                       "Bot"),
              std::string::npos);
    EXPECT_EQ(response.resultJson.find(static_cast<char>(0xFF)), std::string::npos);
}
