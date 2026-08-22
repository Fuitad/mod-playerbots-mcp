/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <atomic>
#include <barrier>
#include <boost/asio.hpp>
#include <chrono>
#include <cstdlib>
#include <future>
#include <stdexcept>
#include <string>
#include <vector>

#include "Bot/MCP/PlayerbotVerificationProtocol.h"
#include "Bot/MCP/PlayerbotVerificationServer.h"
#include "Bot/Telemetry/PlayerbotInspector.h"
#include "Bot/Telemetry/PlayerbotVerificationState.h"
#include "gtest/gtest.h"

using namespace PlayerbotVerification;

namespace
{
std::string const TEST_TOKEN(32, 't');

TokenDigest TestTokenDigest() { return DigestVerificationToken(TEST_TOKEN); }

class EnvironmentValueGuard
{
public:
    explicit EnvironmentValueGuard(char const* name) : name(name)
    {
        if (char const* value = std::getenv(name))
            previousValue = value;
    }

    ~EnvironmentValueGuard()
    {
        if (previousValue)
            setenv(name.c_str(), previousValue->c_str(), 1);
        else
            unsetenv(name.c_str());
    }

private:
    std::string name;
    std::optional<std::string> previousValue;
};

std::string StatusRequest(std::string const& token = TEST_TOKEN)
{
    return R"({"schemaVersion":2,"requestId":7,"token":")" + token + R"(","operation":"status"})";
}

std::string RecoveryRequest(std::string const& token = TEST_TOKEN)
{
    return R"({"schemaVersion":2,"requestId":12,"token":")" + token +
           R"(","operation":"recover","botGuid":42,"destination":"homebind"})";
}

uint16 ReserveLoopbackPort()
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acceptor(io,
                                            boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    return acceptor.local_endpoint().port();
}

std::string Exchange(uint16 port, std::string const& payload)
{
    std::optional<std::vector<uint8>> const frame = EncodeFrame(payload);
    if (!frame)
        return {};

    boost::asio::io_context io;
    boost::asio::ip::tcp::socket socket(io);
    socket.connect({boost::asio::ip::address_v4::loopback(), port});
    boost::asio::write(socket, boost::asio::buffer(*frame));

    std::array<uint8, FRAME_HEADER_BYTES> header{};
    boost::asio::read(socket, boost::asio::buffer(header));
    uint32 const length = (static_cast<uint32>(header[0]) << 24) | (static_cast<uint32>(header[1]) << 16) |
                          (static_cast<uint32>(header[2]) << 8) | static_cast<uint32>(header[3]);
    std::string response(length, '\0');
    boost::asio::read(socket, boost::asio::buffer(response.data(), response.size()));
    return response;
}
}  // namespace

TEST(PlayerbotVerificationProtocolTest, FrameUsesNetworkOrderAndEnforces64KiBCeiling)
{
    std::optional<std::vector<uint8>> const frame = EncodeFrame("abc");
    ASSERT_TRUE(frame);
    EXPECT_EQ(*frame, (std::vector<uint8>{0, 0, 0, 3, 'a', 'b', 'c'}));
    EXPECT_TRUE(EncodeFrame(std::string(MAX_FRAME_PAYLOAD_BYTES, 'x')));
    EXPECT_FALSE(EncodeFrame(std::string(MAX_FRAME_PAYLOAD_BYTES + 1, 'x')));
}

TEST(PlayerbotVerificationProtocolTest, FrameRejectsTruncationOversizeAndTrailingBytesWithStableErrors)
{
    FrameDecodeResult const truncated = DecodeFrame(std::vector<uint8>{0, 0, 0});
    EXPECT_EQ(ErrorCodeName(truncated.error.code), std::string("malformed_frame"));

    FrameDecodeResult const oversized = DecodeFrame(std::vector<uint8>{0, 1, 0, 1});
    EXPECT_EQ(ErrorCodeName(oversized.error.code), std::string("frame_too_large"));

    FrameDecodeResult const trailing = DecodeFrame(std::vector<uint8>{0, 0, 0, 1, 'x', 'y'});
    EXPECT_EQ(ErrorCodeName(trailing.error.code), std::string("malformed_frame"));
}

TEST(PlayerbotVerificationProtocolTest, TokenAcquisitionFailsClosedAndComparisonMatchesOnlyExactToken)
{
    EnvironmentValueGuard guard("PLAYERBOT_VERIFICATION_TOKEN");
    unsetenv("PLAYERBOT_VERIFICATION_TOKEN");
    EXPECT_FALSE(VerificationTokenDigestFromEnvironment());

    setenv("PLAYERBOT_VERIFICATION_TOKEN", "short", 1);
    EXPECT_FALSE(VerificationTokenDigestFromEnvironment());

    setenv("PLAYERBOT_VERIFICATION_TOKEN", TEST_TOKEN.c_str(), 1);
    ASSERT_EQ(VerificationTokenDigestFromEnvironment(), TestTokenDigest());
    EXPECT_TRUE(ConstantTimeTokenDigestEquals(TestTokenDigest(), TestTokenDigest()));
    EXPECT_FALSE(ConstantTimeTokenDigestEquals(DigestVerificationToken(std::string(32, 'x')), TestTokenDigest()));
    EXPECT_FALSE(ConstantTimeTokenDigestEquals(DigestVerificationToken(TEST_TOKEN.substr(1)), TestTokenDigest()));
    EXPECT_FALSE(ConstantTimeTokenDigestEquals(DigestVerificationToken(TEST_TOKEN + "x"), TestTokenDigest()));
}

TEST(PlayerbotVerificationProtocolTest, StrictParserAcceptsOnlyApprovedOperationShapes)
{
    EXPECT_TRUE(ParseRequestPayload(StatusRequest(), TestTokenDigest()).request);
    EXPECT_TRUE(ParseRequestPayload(R"({"schemaVersion":2,"requestId":8,"token":")" + TEST_TOKEN +
                                        R"(","operation":"list","afterGuid":0,"limit":100})",
                                    TestTokenDigest())
                    .request);
    EXPECT_TRUE(ParseRequestPayload(R"({"schemaVersion":2,"requestId":9,"token":")" + TEST_TOKEN +
                                        R"(","operation":"inspect","botGuid":42})",
                                    TestTokenDigest())
                    .request);
    EXPECT_TRUE(ParseRequestPayload(R"({"schemaVersion":2,"requestId":10,"token":")" + TEST_TOKEN +
                                        R"(","operation":"check","botGuid":42,"condition":"map","mapId":0})",
                                    TestTokenDigest())
                    .request);
    EXPECT_TRUE(ParseRequestPayload(R"({"schemaVersion":2,"requestId":11,"token":")" + TEST_TOKEN +
                                        R"(","operation":"command","botGuid":42,"masterGuid":7,"command":"follow"})",
                                    TestTokenDigest())
                    .request);
}

TEST(PlayerbotVerificationProtocolTest, StrictParserAcceptsOnlyHomebindRecovery)
{
    RequestParseResult const parsed =
        ParseRequestPayload(R"({"schemaVersion":2,"requestId":12,"token":")" + TEST_TOKEN +
                                R"(","operation":"recover","botGuid":42,"destination":"homebind"})",
                            TestTokenDigest());

    ASSERT_TRUE(parsed.request);
    EXPECT_EQ(parsed.request->operation, Operation::Recover);
    EXPECT_EQ(parsed.request->botGuid, 42U);
    EXPECT_EQ(parsed.request->destination, "homebind");
}

TEST(PlayerbotVerificationProtocolTest, StrictParserAcceptsSkillAndGameObjectStagingShapesWithinTheirBounds)
{
    auto parse = [](std::string const& body)
    {
        return ParseRequestPayload(R"({"schemaVersion":2,"requestId":13,"token":")" + TEST_TOKEN + R"(",)" + body + '}',
                                   TestTokenDigest());
    };

    RequestParseResult const skill =
        parse(R"("operation":"set_skill","botGuid":42,"skillId":186,"value":75,"maximum":75)");
    ASSERT_TRUE(skill.request);
    EXPECT_EQ(skill.request->operation, Operation::SetSkill);
    EXPECT_EQ(skill.request->botGuid, 42U);
    EXPECT_EQ(skill.request->numbers.at("skillId"), 186U);
    EXPECT_EQ(skill.request->numbers.at("value"), 75U);
    EXPECT_EQ(skill.request->numbers.at("maximum"), 75U);

    // A maximum that is not a rank cap, a value above it, or a zero skill are all refused before the
    // world thread sees them.
    EXPECT_EQ(parse(R"("operation":"set_skill","botGuid":42,"skillId":186,"value":76,"maximum":75)").error.code,
              ErrorCode::InvalidSkill);
    EXPECT_EQ(parse(R"("operation":"set_skill","botGuid":42,"skillId":186,"value":1,"maximum":80)").error.code,
              ErrorCode::InvalidSkill);
    EXPECT_EQ(parse(R"("operation":"set_skill","botGuid":42,"skillId":186,"value":1,"maximum":525)").error.code,
              ErrorCode::InvalidSkill);
    EXPECT_EQ(parse(R"("operation":"set_skill","botGuid":42,"skillId":0,"value":1,"maximum":75)").error.code,
              ErrorCode::InvalidSkill);
    EXPECT_EQ(parse(R"("operation":"set_skill","botGuid":42,"skillId":186,"value":1)").error.code,
              ErrorCode::MalformedRequest);

    RequestParseResult const teleport =
        parse(R"("operation":"teleport_to_gameobject","botGuid":42,"gameObjectEntry":1731)");
    ASSERT_TRUE(teleport.request);
    EXPECT_EQ(teleport.request->operation, Operation::TeleportToGameObject);
    EXPECT_EQ(teleport.request->numbers.at("gameObjectEntry"), 1731U);
    EXPECT_EQ(parse(R"("operation":"teleport_to_gameobject","botGuid":42,"gameObjectEntry":0)").error.code,
              ErrorCode::GameObjectNotFound);
    EXPECT_EQ(parse(R"("operation":"teleport_to_gameobject","botGuid":42,"gameObjectEntry":1731,"x":1)").error.code,
              ErrorCode::MalformedRequest);
    EXPECT_STREQ(ErrorCodeName(ErrorCode::InvalidSkill), "invalid_skill");
    EXPECT_STREQ(ErrorCodeName(ErrorCode::GameObjectNotFound), "gameobject_not_found");
}

TEST(PlayerbotVerificationProtocolTest, StrictParserRejectsUnsafeRecoveryShapes)
{
    auto parse = [](std::string const& suffix)
    {
        return ParseRequestPayload(R"({"schemaVersion":2,"requestId":12,"token":")" + TEST_TOKEN +
                                       R"(","operation":"recover","botGuid":42,)" + suffix + '}',
                                   TestTokenDigest());
    };

    EXPECT_EQ(parse(R"("destination":"graveyard")").error.code, ErrorCode::UnsupportedDestination);
    EXPECT_EQ(parse(R"("destination":"homebind","mapId":0)").error.code, ErrorCode::MalformedRequest);
    EXPECT_EQ(parse(R"("destination":"homebind","x":1)").error.code, ErrorCode::MalformedRequest);
    EXPECT_EQ(parse(R"("destination":"homebind","command":"follow")").error.code, ErrorCode::MalformedRequest);
    EXPECT_EQ(parse(R"("destination":"homebind","extra":1)").error.code, ErrorCode::MalformedRequest);
    EXPECT_EQ(parse(R"("destination":"homebind","destination":"homebind")").error.code, ErrorCode::MalformedRequest);
    EXPECT_EQ(ParseRequestPayload(R"({"schemaVersion":2,"requestId":12,"token":")" + TEST_TOKEN +
                                      R"(","operation":"recover","botGuid":42})",
                                  TestTokenDigest())
                  .error.code,
              ErrorCode::MalformedRequest);
}

TEST(PlayerbotVerificationProtocolTest, RecoveryErrorNamesAreStable)
{
    EXPECT_STREQ(ErrorCodeName(ErrorCode::BotUnavailable), "bot_unavailable");
    EXPECT_STREQ(ErrorCodeName(ErrorCode::NotManagedPlayerbot), "not_managed_playerbot");
    EXPECT_STREQ(ErrorCodeName(ErrorCode::RecoveryFailed), "recovery_failed");
    EXPECT_STREQ(ErrorCodeName(ErrorCode::UnsupportedDestination), "unsupported_destination");
}

TEST(PlayerbotVerificationProtocolTest, StrictParserRejectsMalformedDuplicateAndExtraFields)
{
    EXPECT_EQ(ParseRequestPayload("not json", TestTokenDigest()).error.code, ErrorCode::MalformedRequest);
    EXPECT_EQ(ParseRequestPayload(R"({"schemaVersion":2,"schemaVersion":2,"requestId":7,"token":")" + TEST_TOKEN +
                                      R"(","operation":"status"})",
                                  TestTokenDigest())
                  .error.code,
              ErrorCode::MalformedRequest);
    EXPECT_EQ(ParseRequestPayload(
                  R"({"schemaVersion":2,"requestId":7,"token":")" + TEST_TOKEN + R"(","operation":"status","extra":1})",
                  TestTokenDigest())
                  .error.code,
              ErrorCode::MalformedRequest);
}

TEST(PlayerbotVerificationProtocolTest, StrictParserRejectsInvalidVersionAuthenticationOperationAndInputs)
{
    EXPECT_EQ(
        ParseRequestPayload(R"({"schemaVersion":1,"requestId":7,"token":")" + TEST_TOKEN + R"(","operation":"status"})",
                            TestTokenDigest())
            .error.code,
        ErrorCode::UnsupportedSchemaVersion);
    EXPECT_EQ(ParseRequestPayload(StatusRequest(std::string(32, 'x')), TestTokenDigest()).error.code,
              ErrorCode::AuthenticationFailed);
    EXPECT_EQ(
        ParseRequestPayload(R"({"schemaVersion":2,"requestId":7,"token":")" + TEST_TOKEN + R"(","operation":"delete"})",
                            TestTokenDigest())
            .error.code,
        ErrorCode::UnknownOperation);
    EXPECT_EQ(ParseRequestPayload(R"({"schemaVersion":2,"requestId":7,"token":")" + TEST_TOKEN +
                                      R"(","operation":"inspect","botGuid":0})",
                                  TestTokenDigest())
                  .error.code,
              ErrorCode::InvalidGuid);
    EXPECT_EQ(ParseRequestPayload(R"({"schemaVersion":2,"requestId":7,"token":")" + TEST_TOKEN +
                                      R"(","operation":"list","afterGuid":0,"limit":101})",
                                  TestTokenDigest())
                  .error.code,
              ErrorCode::InvalidLimit);
    EXPECT_EQ(ParseRequestPayload(R"({"schemaVersion":2,"requestId":7,"token":")" + TEST_TOKEN +
                                      R"(","operation":"command","botGuid":42,"masterGuid":7,"command":""})",
                                  TestTokenDigest())
                  .error.code,
              ErrorCode::InvalidCommand);
}

TEST(PlayerbotVerificationProtocolTest, GmCommandParsesOneCommandAndRefusesServerAdministration)
{
    auto const parse = [](std::string const& body)
    {
        return ParseRequestPayload(R"({"schemaVersion":2,"requestId":13,"token":")" + TEST_TOKEN + R"(",)" + body + '}',
                                   TestTokenDigest());
    };
    RequestParseResult const tele = parse(R"("operation":"gm_command","command":".tele name Leporaitceau brill")");
    ASSERT_TRUE(tele.request);
    EXPECT_EQ(tele.request->operation, Operation::GmCommand);
    EXPECT_EQ(tele.request->command, ".tele name Leporaitceau brill");
    EXPECT_EQ(parse(R"("operation":"gm_command","command":"")").error.code, ErrorCode::InvalidCommand);
    EXPECT_EQ(parse(R"("operation":"gm_command","command":".server shutdown 1")").error.code,
              ErrorCode::InvalidCommand);
    EXPECT_EQ(parse(R"("operation":"gm_command","command":"account set gmlevel x 3")").error.code,
              ErrorCode::InvalidCommand);
    EXPECT_EQ(parse(R"("operation":"gm_command","command":".tele brill","botGuid":3)").error.code,
              ErrorCode::MalformedRequest);

    EXPECT_TRUE(IsRefusedGmCommand(" .Server restart"));
    EXPECT_TRUE(IsRefusedGmCommand("quit"));
    EXPECT_TRUE(IsRefusedGmCommand("."));
    EXPECT_FALSE(IsRefusedGmCommand(".servername"));
    EXPECT_FALSE(IsRefusedGmCommand("tele name Foo brill"));
    EXPECT_FALSE(IsRefusedGmCommand(".go xyz 1 2 3 0"));
}

TEST(PlayerbotVerificationProtocolTest, PaginationIsGuidOrderedBoundedAndExclusive)
{
    std::optional<GuidPage> const page = PaginateGuids({9, 1, 7, 3, 5}, 3, 2);
    ASSERT_TRUE(page);
    EXPECT_EQ(page->guids, (std::vector<uint32>{5, 7}));
    EXPECT_TRUE(page->hasMore);
    EXPECT_EQ(page->nextAfterGuid, 7U);
    EXPECT_FALSE(PaginateGuids({1}, 0, 0));
    EXPECT_FALSE(PaginateGuids({1}, 0, 101));
}

TEST(PlayerbotVerificationProtocolTest, ResponseBudgetReturnsTypedErrorWithoutPartialPayload)
{
    std::string const marker = "must_not_leak";
    std::string const oversizedResult =
        R"({"marker":")" + marker + R"(","data":")" + std::string(MAX_RESPONSE_PAYLOAD_BYTES, 'x') + R"("})";
    std::string const response = SerializeResponse(7, Response::Success(oversizedResult));

    EXPECT_LE(response.size(), MAX_RESPONSE_PAYLOAD_BYTES);
    EXPECT_NE(response.find(R"("code":"response_too_large")"), std::string::npos);
    EXPECT_EQ(response.find(marker), std::string::npos);
    EXPECT_EQ(response.find(TEST_TOKEN), std::string::npos);
}

TEST(PlayerbotVerificationProtocolTest, WorstCaseInspectionIsWholeOrResponseTooLarge)
{
    PlayerbotVerificationState state;
    std::string const longName(PLAYERBOT_VERIFICATION_ACTION_NAME_CAPACITY - 1, 'a');
    for (uint64 index = 0; index < PLAYERBOT_VERIFICATION_ACTION_HISTORY_CAPACITY; ++index)
        state.RecordActionAttempt(longName, index % 2 == 0, index);

    PlayerbotVerificationInspection inspection;
    inspection.actionHistory = state.CopyActionHistory();
    for (uint32 index = 0; index < 32; ++index)
        inspection.equipment.push_back({.slot = index, .itemId = index, .name = longName, .count = 1});
    for (uint32 index = 0; index < 128; ++index)
        inspection.inventory.push_back({.itemId = index, .name = longName, .count = 1});
    for (uint32 index = 0; index < 128; ++index)
        inspection.skills.push_back({.id = index, .name = longName, .value = 1, .maximum = 1});
    for (uint32 index = 0; index < 16; ++index)
        inspection.professions.push_back({.id = index, .name = longName, .value = 1, .maximum = 1});
    inspection.career = {
        .status = PlayerbotVerificationCareerStatus::Valid,
        .source = PlayerbotVerificationCareerSource::Saved,
        .version = 1u,
        .candidateToken = longName,
        .primarySkills = {164u, 165u},
        .secondarySkills = {185u, 129u, 356u},
        .spendingStyle = 3u,
        .marketEligible = true,
        .engagement = 100u,
    };
    for (uint32 index = 0; index < 1024; ++index)
        inspection.knownRecipeSpellIds.push_back(1000u + index);
    inspection.knownRecipeCompleteness = {
        .totalCount = inspection.knownRecipeSpellIds.size(),
        .returnedCount = inspection.knownRecipeSpellIds.size(),
        .truncated = false,
    };
    inspection.economy = {
        .sequence = 1u,
        .outcome = PlayerbotVerificationEconomyOutcome::FailedPrecondition,
        .phase = PlayerbotVerificationEconomyPhase::SellSurplus,
        .workOrderSpellId = 1001u,
        .consecutiveFailures = 5u,
        .nextEligibleTime = 4294967396ULL,
    };

    std::string const result = PlayerbotInspector::SerializeVerification(inspection);
    std::string const response = SerializeResponse(17, Response::Success(result));
    EXPECT_LE(response.size(), MAX_RESPONSE_PAYLOAD_BYTES);
    EXPECT_TRUE(response.find(R"("ok":true)") != std::string::npos ||
                response.find(R"("code":"response_too_large")") != std::string::npos);
}

TEST(PlayerbotVerificationProtocolTest, ServerFailsClosedAndEndpointIsLoopback)
{
    EXPECT_EQ(MakePlayerbotVerificationEndpoint(12345).address().to_string(), "127.0.0.1");

    EnvironmentValueGuard guard("PLAYERBOT_VERIFICATION_TOKEN");
    unsetenv("PLAYERBOT_VERIFICATION_TOKEN");
    PlayerbotVerificationServer missingTokenServer;
    EXPECT_FALSE(missingTokenServer.Start(ReserveLoopbackPort()));

    setenv("PLAYERBOT_VERIFICATION_TOKEN", "short", 1);
    PlayerbotVerificationServer shortTokenServer;
    EXPECT_FALSE(shortTokenServer.Start(ReserveLoopbackPort()));

    setenv("PLAYERBOT_VERIFICATION_TOKEN", TEST_TOKEN.c_str(), 1);
    PlayerbotVerificationServer disabledServer;
    EXPECT_FALSE(disabledServer.Start(0));
}

TEST(PlayerbotVerificationProtocolTest, AuthenticationFailureDoesNotReachHandlerOrExposeToken)
{
    EnvironmentValueGuard guard("PLAYERBOT_VERIFICATION_TOKEN");
    setenv("PLAYERBOT_VERIFICATION_TOKEN", TEST_TOKEN.c_str(), 1);
    std::atomic<uint32> handlerCalls = 0;
    PlayerbotVerificationServer server(
        [&handlerCalls](Request const&)
        {
            ++handlerCalls;
            return Response::Success(R"({"ready":true})");
        });
    uint16 const port = ReserveLoopbackPort();
    ASSERT_TRUE(server.Start(port));

    std::string const response = Exchange(port, StatusRequest(std::string(32, 'x')));
    server.Stop();

    EXPECT_EQ(handlerCalls.load(), 0U);
    EXPECT_NE(response.find(R"("code":"authentication_failed")"), std::string::npos);
    EXPECT_EQ(response.find(TEST_TOKEN), std::string::npos);
}

TEST(PlayerbotVerificationProtocolTest, UnauthorizedRecoveryIsCorrelatedAndAuditedOnceWithoutRawInput)
{
    EnvironmentValueGuard guard("PLAYERBOT_VERIFICATION_TOKEN");
    setenv("PLAYERBOT_VERIFICATION_TOKEN", TEST_TOKEN.c_str(), 1);
    std::atomic<uint32> handlerCalls = 0;
    std::vector<std::string> audits;
    PlayerbotVerificationServer server(
        [&handlerCalls](Request const&)
        {
            ++handlerCalls;
            return Response::Success(R"({"recovered":true})");
        },
        [&audits](std::string const& record) { audits.push_back(record); });
    uint16 const port = ReserveLoopbackPort();
    ASSERT_TRUE(server.Start(port));

    std::string const wrongToken(32, 'x');
    std::string const response = Exchange(port, RecoveryRequest(wrongToken));
    server.Stop();

    EXPECT_EQ(handlerCalls.load(), 0U);
    EXPECT_NE(response.find(R"("requestId":12)"), std::string::npos);
    EXPECT_NE(response.find(R"("code":"authentication_failed")"), std::string::npos);
    ASSERT_EQ(audits.size(), 1U);
    EXPECT_NE(audits[0].find(R"("operation":"recover")"), std::string::npos);
    EXPECT_NE(audits[0].find(R"("requestId":12)"), std::string::npos);
    EXPECT_NE(audits[0].find(R"("botGuid":42)"), std::string::npos);
    EXPECT_NE(audits[0].find(R"("outcome":"unauthorized")"), std::string::npos);
    EXPECT_NE(audits[0].find(R"("reason":"authentication_failed")"), std::string::npos);
    EXPECT_EQ(audits[0].find(wrongToken), std::string::npos);
    EXPECT_EQ(audits[0].find(RecoveryRequest(wrongToken)), std::string::npos);
}

TEST(PlayerbotVerificationProtocolTest, RejectedRecoveryShapesEachEmitOneSanitizedAudit)
{
    EnvironmentValueGuard guard("PLAYERBOT_VERIFICATION_TOKEN");
    setenv("PLAYERBOT_VERIFICATION_TOKEN", TEST_TOKEN.c_str(), 1);
    std::atomic<uint32> handlerCalls = 0;
    std::vector<std::string> audits;
    PlayerbotVerificationServer server(
        [&handlerCalls](Request const&)
        {
            ++handlerCalls;
            return Response::Success(R"({"recovered":true})");
        },
        [&audits](std::string const& record) { audits.push_back(record); });
    uint16 const port = ReserveLoopbackPort();
    ASSERT_TRUE(server.Start(port));

    std::vector<std::string> const requests = {
        R"({"schemaVersion":2,"requestId":20,"token":")" + TEST_TOKEN +
            R"(","operation":"recover","botGuid":42,"destination":"graveyard"})",
        R"({"schemaVersion":2,"requestId":21,"token":")" + TEST_TOKEN + R"(","operation":"recover","botGuid":42})",
        R"({"schemaVersion":2,"requestId":22,"token":")" + TEST_TOKEN +
            R"(","operation":"recover","botGuid":42,"destination":"homebind","x":1})",
        R"({"schemaVersion":2,"requestId":23,"token":")" + TEST_TOKEN +
            R"(","operation":"recover","botGuid":42,"destination":"homebind","destination":"homebind"})",
        R"({"schemaVersion":2,"requestId":24,"requestId":25,"token":")" + TEST_TOKEN +
            R"(","operation":"recover","botGuid":42,"destination":"homebind"})",
        R"({"schemaVersion":2,"requestId":26,"token":")" + TEST_TOKEN +
            R"(","operation":"recover","botGuid":42,"destination":"homebind","command":"caller_secret"})",
        R"({"schemaVersion":2,"requestId":27,"token":")" + TEST_TOKEN +
            R"(","operation":"recover","botGuid":42,"destination":"homebind","x":1.25})",
        R"({"schemaVersion":2,"requestId":28,"token":")" + TEST_TOKEN +
            R"(","operation":"recover","botGuid":42,"destination":"homebind","extra":true})",
        R"({"schemaVersion":2,"requestId":29,"token":")" + TEST_TOKEN +
            R"(","operation":"recover","botGuid":42,"destination":"homebind","extra":{"nested":[1,false,null]}})",
    };
    for (std::string const& request : requests)
        EXPECT_NE(Exchange(port, request).find(R"("ok":false)"), std::string::npos);
    server.Stop();

    EXPECT_EQ(handlerCalls.load(), 0U);
    ASSERT_EQ(audits.size(), requests.size());
    for (std::string const& audit : audits)
    {
        EXPECT_NE(audit.find(R"("operation":"recover")"), std::string::npos);
        EXPECT_EQ(audit.find(TEST_TOKEN), std::string::npos);
        EXPECT_EQ(audit.find(R"("x":1)"), std::string::npos);
        EXPECT_EQ(audit.find(R"("x":1.25)"), std::string::npos);
        EXPECT_EQ(audit.find("graveyard"), std::string::npos);
        EXPECT_EQ(audit.find("caller_secret"), std::string::npos);
    }
    EXPECT_EQ(audits[4].find(R"("requestId":)"), std::string::npos);
}

TEST(PlayerbotVerificationProtocolTest, SuccessfulRecoveryEmitsExactlyItsInternalAuditRecord)
{
    EnvironmentValueGuard guard("PLAYERBOT_VERIFICATION_TOKEN");
    setenv("PLAYERBOT_VERIFICATION_TOKEN", TEST_TOKEN.c_str(), 1);
    std::vector<std::string> audits;
    PlayerbotVerificationServer server(
        [](Request const& request)
        {
            EXPECT_EQ(request.operation, Operation::Recover);
            Response response = Response::Success(R"({"outcome":"recovered"})");
            response.recoveryAudit = RecoveryAuditRecord{
                .timestampMs = 123,
                .requestId = request.requestId,
                .botGuid = request.botGuid,
                .destination = RecoveryDestination::Homebind,
                .outcome = RecoveryOutcome::Recovered,
                .reason = RecoveryReason::HomebindTeleportAccepted,
                .mutationState = RecoveryMutationState::Completed,
                .persistenceState = RecoveryPersistenceState::Deferred,
            };
            return response;
        },
        [&audits](std::string const& record) { audits.push_back(record); });
    uint16 const port = ReserveLoopbackPort();
    ASSERT_TRUE(server.Start(port));

    std::string const response = Exchange(port, RecoveryRequest());
    server.Stop();

    EXPECT_NE(response.find(R"("outcome":"recovered")"), std::string::npos);
    ASSERT_EQ(audits.size(), 1U);
    EXPECT_NE(audits[0].find(R"("timestampMs":123)"), std::string::npos);
    EXPECT_NE(audits[0].find(R"("outcome":"recovered")"), std::string::npos);
    EXPECT_NE(audits[0].find(R"("persistenceState":"deferred")"), std::string::npos);
    EXPECT_EQ(response.find(R"("persistenceState":"deferred")"), std::string::npos);
}

TEST(PlayerbotVerificationProtocolTest, RecoveryHandlerFailureStillEmitsOneClosedAudit)
{
    EnvironmentValueGuard guard("PLAYERBOT_VERIFICATION_TOKEN");
    setenv("PLAYERBOT_VERIFICATION_TOKEN", TEST_TOKEN.c_str(), 1);
    std::vector<std::string> audits;
    PlayerbotVerificationServer server([](Request const&) -> Response { throw std::runtime_error("caller_secret"); },
                                       [&audits](std::string const& record) { audits.push_back(record); });
    uint16 const port = ReserveLoopbackPort();
    ASSERT_TRUE(server.Start(port));

    std::string const response = Exchange(port, RecoveryRequest());
    server.Stop();

    EXPECT_NE(response.find(R"("code":"internal_error")"), std::string::npos);
    ASSERT_EQ(audits.size(), 1U);
    EXPECT_NE(audits[0].find(R"("outcome":"recovery_failed")"), std::string::npos);
    EXPECT_NE(audits[0].find(R"("reason":"internal_error")"), std::string::npos);
    EXPECT_EQ(audits[0].find("caller_secret"), std::string::npos);
}

TEST(PlayerbotVerificationProtocolTest, PartialUnauthenticatedFrameExpiresAndNextRequestIsServiced)
{
    EnvironmentValueGuard guard("PLAYERBOT_VERIFICATION_TOKEN");
    setenv("PLAYERBOT_VERIFICATION_TOKEN", TEST_TOKEN.c_str(), 1);
    std::atomic<uint32> handlerCalls = 0;
    PlayerbotVerificationServer server(
        [&handlerCalls](Request const&)
        {
            ++handlerCalls;
            return Response::Success(R"({"ready":true})");
        });
    uint16 const port = ReserveLoopbackPort();
    ASSERT_TRUE(server.Start(port));

    boost::asio::io_context io;
    boost::asio::ip::tcp::socket stalledSocket(io);
    stalledSocket.connect({boost::asio::ip::address_v4::loopback(), port});
    std::array<uint8, 1> const partialHeader{0};
    boost::asio::write(stalledSocket, boost::asio::buffer(partialHeader));

    std::future<std::string> response =
        std::async(std::launch::async, [port] { return Exchange(port, StatusRequest()); });
    std::future_status const completion = response.wait_for(std::chrono::seconds(6));
    boost::system::error_code closeError;
    stalledSocket.close(closeError);
    if (completion != std::future_status::ready)
        response.wait_for(std::chrono::seconds(2));
    std::string const payload =
        response.wait_for(std::chrono::seconds(0)) == std::future_status::ready ? response.get() : std::string{};
    server.Stop();

    EXPECT_EQ(completion, std::future_status::ready);
    EXPECT_EQ(handlerCalls.load(), 1U);
    EXPECT_NE(payload.find(R"("ready":true)"), std::string::npos);
}

TEST(PlayerbotVerificationProtocolTest, StopReleasesIdleAcceptAndJoinsWithinDeadline)
{
    EnvironmentValueGuard guard("PLAYERBOT_VERIFICATION_TOKEN");
    setenv("PLAYERBOT_VERIFICATION_TOKEN", TEST_TOKEN.c_str(), 1);
    PlayerbotVerificationServer server;
    ASSERT_TRUE(server.Start(ReserveLoopbackPort()));
    ASSERT_TRUE(server.IsRunning());

    std::future<void> stopped = std::async(std::launch::async, [&server] { server.Stop(); });
    EXPECT_EQ(stopped.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_FALSE(server.IsRunning());
}

TEST(PlayerbotVerificationProtocolTest, StopDuringActiveResponseJoinsWithoutCrashing)
{
    EnvironmentValueGuard guard("PLAYERBOT_VERIFICATION_TOKEN");
    setenv("PLAYERBOT_VERIFICATION_TOKEN", TEST_TOKEN.c_str(), 1);

    for (uint32 iteration = 0; iteration < 200; ++iteration)
    {
        std::barrier responseReady(2);
        PlayerbotVerificationServer server(
            [&responseReady](Request const&)
            {
                responseReady.arrive_and_wait();
                return Response::Success(R"({"ready":true})");
            });
        uint16 const port = ReserveLoopbackPort();
        ASSERT_TRUE(server.Start(port));

        std::future<void> exchange = std::async(std::launch::async,
                                                [port]
                                                {
                                                    try
                                                    {
                                                        Exchange(port, StatusRequest());
                                                    }
                                                    catch (...)
                                                    {
                                                    }
                                                });
        responseReady.arrive_and_wait();
        server.Stop();

        EXPECT_EQ(exchange.wait_for(std::chrono::seconds(2)), std::future_status::ready);
        EXPECT_FALSE(server.IsRunning());
    }
}
