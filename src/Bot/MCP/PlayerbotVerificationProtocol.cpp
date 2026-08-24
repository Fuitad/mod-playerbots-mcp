/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotVerificationProtocol.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

#include "CryptoHash.h"

namespace
{
using namespace PlayerbotVerification;

struct FlatJsonValue
{
    bool isString = false;
    bool isUnsigned = false;
    uint64 number = 0;
    std::string text;
};

using FlatJsonFields = std::map<std::string, FlatJsonValue>;

ProtocolError MakeError(ErrorCode code)
{
    switch (code)
    {
        case ErrorCode::MalformedFrame:
            return {code, "The frame is malformed."};
        case ErrorCode::FrameTooLarge:
            return {code, "The frame exceeds the 64 KiB limit."};
        case ErrorCode::MalformedRequest:
            return {code, "The request schema is malformed."};
        case ErrorCode::AuthenticationFailed:
            return {code, "Authentication failed."};
        case ErrorCode::UnsupportedSchemaVersion:
            return {code, "The schema version is unsupported."};
        case ErrorCode::UnknownOperation:
            return {code, "The operation is unknown."};
        case ErrorCode::InvalidGuid:
            return {code, "A player GUID is invalid."};
        case ErrorCode::InvalidLimit:
            return {code, "The list limit must be from 1 through 100."};
        case ErrorCode::InvalidCommand:
            return {code, "The command is invalid."};
        case ErrorCode::InvalidCondition:
            return {code, "The check condition is invalid."};
        case ErrorCode::UnsupportedDestination:
            return {code, "The recovery destination is unsupported."};
        case ErrorCode::ResponseTooLarge:
            return {code, "The response exceeds the aggregate budget."};
        case ErrorCode::OperationUnavailable:
            return {code, "The operation is unavailable."};
        case ErrorCode::QueueFull:
            return {code, "The world operation queue is full."};
        case ErrorCode::Timeout:
            return {code, "The world operation timed out."};
        case ErrorCode::Shutdown:
            return {code, "The verification server is shutting down."};
        case ErrorCode::BotNotFound:
            return {code, "The bot is not available."};
        case ErrorCode::BotUnavailable:
            return {code, "The bot cannot be recovered in its current state."};
        case ErrorCode::NotManagedPlayerbot:
            return {code, "The player is not a managed Playerbot."};
        case ErrorCode::RecoveryFailed:
            return {code, "The recovery operation failed."};
        case ErrorCode::MasterNotFound:
            return {code, "The master is not available."};
        case ErrorCode::MasterIsBot:
            return {code, "The master must be a real player."};
        case ErrorCode::InvalidRelationship:
            return {code, "The master relationship is invalid."};
        case ErrorCode::InvalidSkill:
            return {code, "The skill value or maximum is invalid."};
        case ErrorCode::GameObjectNotFound:
            return {code, "No spawned gameobject with that entry exists on the bot's map."};
        case ErrorCode::InternalError:
            return {code, "The operation failed internally."};
        case ErrorCode::None:
            return {};
    }
    return {ErrorCode::InternalError, "The operation failed internally."};
}

void AppendEscapedJsonString(std::string& output, std::string const& value)
{
    output += '"';
    for (unsigned char character : value)
    {
        switch (character)
        {
            case '"':
                output += "\\\"";
                break;
            case '\\':
                output += "\\\\";
                break;
            case '\n':
                output += "\\n";
                break;
            case '\r':
                output += "\\r";
                break;
            case '\t':
                output += "\\t";
                break;
            case '\b':
                output += "\\b";
                break;
            case '\f':
                output += "\\f";
                break;
            default:
                if (character < 0x20)
                {
                    static constexpr char HEX[] = "0123456789abcdef";
                    output += "\\u00";
                    output += HEX[(character >> 4) & 0x0F];
                    output += HEX[character & 0x0F];
                }
                else
                    output += static_cast<char>(character);
                break;
        }
    }
    output += '"';
}

void AppendRecoveryPosition(std::string& output, char const* field, RecoveryPosition const& position)
{
    std::ostringstream coordinates;
    coordinates << std::setprecision(std::numeric_limits<float>::max_digits10);
    coordinates << ",\"" << field << "\":{\"mapId\":" << position.mapId << ",\"zoneId\":" << position.zoneId
                << ",\"areaId\":" << position.areaId << ",\"x\":" << position.x << ",\"y\":" << position.y
                << ",\"z\":" << position.z << ",\"orientation\":" << position.orientation << '}';
    output += coordinates.str();
}

class FlatJsonParser
{
public:
    explicit FlatJsonParser(std::string const& input) : input(input) {}

    std::optional<FlatJsonFields> Parse(std::set<std::string>* duplicateFields = nullptr)
    {
        FlatJsonFields fields;
        SkipWhitespace();
        if (!Consume('{'))
            return std::nullopt;

        SkipWhitespace();
        if (Consume('}'))
            return Finish(fields);

        while (true)
        {
            SkipWhitespace();
            std::optional<std::string> key = ParseString();
            if (!key)
                return std::nullopt;

            SkipWhitespace();
            if (!Consume(':'))
                return std::nullopt;

            SkipWhitespace();
            std::optional<FlatJsonValue> value = ParseValue();
            if (!value)
                return std::nullopt;
            bool const inserted = fields.emplace(*key, std::move(*value)).second;
            if (!inserted)
            {
                if (!duplicateFields)
                    return std::nullopt;
                duplicateFields->insert(*key);
            }

            SkipWhitespace();
            if (Consume(','))
                continue;
            if (Consume('}'))
                return Finish(fields);
            return std::nullopt;
        }
    }

private:
    std::optional<FlatJsonFields> Finish(FlatJsonFields& fields)
    {
        SkipWhitespace();
        if (position != input.size())
            return std::nullopt;
        return std::move(fields);
    }

    void SkipWhitespace()
    {
        while (position < input.size())
        {
            char const character = input[position];
            if (character != ' ' && character != '\t' && character != '\n' && character != '\r')
                break;
            ++position;
        }
    }

    bool Consume(char expected)
    {
        if (position >= input.size() || input[position] != expected)
            return false;
        ++position;
        return true;
    }

    std::optional<FlatJsonValue> ParseValue()
    {
        if (position >= input.size())
            return std::nullopt;

        if (input[position] == '"')
        {
            std::optional<std::string> text = ParseString();
            if (!text)
                return std::nullopt;
            return FlatJsonValue{.isString = true, .text = std::move(*text)};
        }

        if (input[position] >= '0' && input[position] <= '9')
        {
            std::size_t const start = position;
            if (std::optional<FlatJsonValue> value = ParseUnsigned())
                return value;
            position = start;
        }

        if (!SkipJsonValue(0))
            return std::nullopt;
        return FlatJsonValue{};
    }

    std::optional<FlatJsonValue> ParseUnsigned()
    {
        uint64 result = 0;
        std::size_t digits = 0;
        while (position < input.size() && input[position] >= '0' && input[position] <= '9')
        {
            uint64 const digit = static_cast<uint64>(input[position] - '0');
            if (result > (std::numeric_limits<uint64>::max() - digit) / 10)
                return std::nullopt;
            result = result * 10 + digit;
            ++position;
            ++digits;
        }

        if (!digits || digits > 20)
            return std::nullopt;
        if (position < input.size() && (input[position] == '.' || input[position] == 'e' || input[position] == 'E'))
            return std::nullopt;

        return FlatJsonValue{.isUnsigned = true, .number = result};
    }

    bool SkipJsonValue(uint32 depth)
    {
        static constexpr uint32 MAX_NESTED_DEPTH = 32;
        if (depth > MAX_NESTED_DEPTH || position >= input.size())
            return false;

        char const character = input[position];
        if (character == '"')
            return ParseString().has_value();
        if (character == '-' || (character >= '0' && character <= '9'))
            return SkipNumber();
        if (character == 't')
            return ConsumeLiteral("true", 4);
        if (character == 'f')
            return ConsumeLiteral("false", 5);
        if (character == 'n')
            return ConsumeLiteral("null", 4);
        if (character == '[')
            return SkipArray(depth);
        if (character == '{')
            return SkipObject(depth);
        return false;
    }

    bool SkipNumber()
    {
        Consume('-');
        if (position >= input.size())
            return false;

        if (input[position] == '0')
        {
            ++position;
            if (position < input.size() && input[position] >= '0' && input[position] <= '9')
                return false;
        }
        else
        {
            if (input[position] < '1' || input[position] > '9')
                return false;
            while (position < input.size() && input[position] >= '0' && input[position] <= '9')
                ++position;
        }

        if (position < input.size() && input[position] == '.')
        {
            ++position;
            if (position >= input.size() || input[position] < '0' || input[position] > '9')
                return false;
            while (position < input.size() && input[position] >= '0' && input[position] <= '9')
                ++position;
        }

        if (position < input.size() && (input[position] == 'e' || input[position] == 'E'))
        {
            ++position;
            if (position < input.size() && (input[position] == '+' || input[position] == '-'))
                ++position;
            if (position >= input.size() || input[position] < '0' || input[position] > '9')
                return false;
            while (position < input.size() && input[position] >= '0' && input[position] <= '9')
                ++position;
        }
        return true;
    }

    bool SkipArray(uint32 depth)
    {
        Consume('[');
        SkipWhitespace();
        if (Consume(']'))
            return true;

        while (true)
        {
            if (!SkipJsonValue(depth + 1))
                return false;
            SkipWhitespace();
            if (Consume(','))
            {
                SkipWhitespace();
                continue;
            }
            return Consume(']');
        }
    }

    bool SkipObject(uint32 depth)
    {
        Consume('{');
        SkipWhitespace();
        if (Consume('}'))
            return true;

        while (true)
        {
            if (!ParseString())
                return false;
            SkipWhitespace();
            if (!Consume(':'))
                return false;
            SkipWhitespace();
            if (!SkipJsonValue(depth + 1))
                return false;
            SkipWhitespace();
            if (Consume(','))
            {
                SkipWhitespace();
                continue;
            }
            return Consume('}');
        }
    }

    bool ConsumeLiteral(char const* literal, std::size_t length)
    {
        if (input.compare(position, length, literal) != 0)
            return false;
        position += length;
        return true;
    }

    std::optional<std::string> ParseString()
    {
        if (!Consume('"'))
            return std::nullopt;

        std::string result;
        while (position < input.size())
        {
            unsigned char const character = static_cast<unsigned char>(input[position]);
            if (character == '"')
            {
                ++position;
                return result;
            }

            if (character == '\\')
            {
                ++position;
                if (position >= input.size())
                    return std::nullopt;
                char const escape = input[position++];
                switch (escape)
                {
                    case '"':
                        result += '"';
                        break;
                    case '\\':
                        result += '\\';
                        break;
                    case '/':
                        result += '/';
                        break;
                    case 'n':
                        result += '\n';
                        break;
                    case 'r':
                        result += '\r';
                        break;
                    case 't':
                        result += '\t';
                        break;
                    case 'b':
                        result += '\b';
                        break;
                    case 'f':
                        result += '\f';
                        break;
                    case 'u':
                    {
                        std::optional<uint32> codepoint = ParseHex4();
                        if (!codepoint || (*codepoint >= 0xD800 && *codepoint <= 0xDFFF))
                            return std::nullopt;
                        AppendUtf8(result, *codepoint);
                        break;
                    }
                    default:
                        return std::nullopt;
                }
                continue;
            }

            if (character < 0x20)
                return std::nullopt;
            result += static_cast<char>(character);
            ++position;
        }
        return std::nullopt;
    }

    std::optional<uint32> ParseHex4()
    {
        if (position + 4 > input.size())
            return std::nullopt;

        uint32 value = 0;
        for (std::size_t index = 0; index < 4; ++index)
        {
            char const character = input[position + index];
            value <<= 4;
            if (character >= '0' && character <= '9')
                value |= static_cast<uint32>(character - '0');
            else if (character >= 'a' && character <= 'f')
                value |= static_cast<uint32>(character - 'a' + 10);
            else if (character >= 'A' && character <= 'F')
                value |= static_cast<uint32>(character - 'A' + 10);
            else
                return std::nullopt;
        }
        position += 4;
        return value;
    }

    static void AppendUtf8(std::string& output, uint32 codepoint)
    {
        if (codepoint < 0x80)
            output += static_cast<char>(codepoint);
        else if (codepoint < 0x800)
        {
            output += static_cast<char>(0xC0 | (codepoint >> 6));
            output += static_cast<char>(0x80 | (codepoint & 0x3F));
        }
        else
        {
            output += static_cast<char>(0xE0 | (codepoint >> 12));
            output += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            output += static_cast<char>(0x80 | (codepoint & 0x3F));
        }
    }

    std::string const& input;
    std::size_t position = 0;
};

bool HasExactFields(FlatJsonFields const& fields, std::set<std::string> required, std::set<std::string> optional = {})
{
    bool const hasRequired = std::all_of(required.begin(), required.end(),
                                         [&fields](std::string const& name) { return fields.contains(name); });
    if (!hasRequired)
        return false;

    for (auto const& [name, value] : fields)
    {
        (void)value;
        if (!required.contains(name) && !optional.contains(name))
            return false;
    }
    return true;
}

std::optional<uint64> UnsignedField(FlatJsonFields const& fields, std::string const& name)
{
    auto const found = fields.find(name);
    if (found == fields.end() || !found->second.isUnsigned)
        return std::nullopt;
    return found->second.number;
}

std::optional<std::string> StringField(FlatJsonFields const& fields, std::string const& name)
{
    auto const found = fields.find(name);
    if (found == fields.end() || !found->second.isString)
        return std::nullopt;
    return found->second.text;
}

bool IsValidGuid(uint64 value) { return value > 0 && value <= std::numeric_limits<uint32>::max(); }

bool StoreUInt32(Request& request, FlatJsonFields const& fields, std::string const& name, bool allowZero = false)
{
    std::optional<uint64> value = UnsignedField(fields, name);
    if (!value || *value > std::numeric_limits<uint32>::max() || (!allowZero && !*value))
        return false;
    request.numbers[name] = *value;
    return true;
}

std::set<std::string> CommonFields(std::initializer_list<char const*> operationFields)
{
    std::set<std::string> fields = {"schemaVersion", "requestId", "token", "operation"};
    fields.insert(operationFields.begin(), operationFields.end());
    return fields;
}

RequestParseResult ParseCheck(FlatJsonFields const& fields, Request request)
{
    std::optional<std::string> condition = StringField(fields, "condition");
    if (!condition)
        return {.error = MakeError(ErrorCode::InvalidCondition)};
    request.condition = *condition;

    std::set<std::string> required = CommonFields({"botGuid", "condition"});
    std::set<std::string> optional;
    if (*condition == "transport_attached")
        optional.insert("transportEntry");
    else if (*condition == "transport_detached")
    {
    }
    else if (*condition == "map")
        required.insert("mapId");
    else if (*condition == "action")
        required.insert({"afterSequence", "actionName", "actionResult"});
    else if (*condition == "profession_skill")
        required.insert({"skillId", "minimumValue"});
    else if (*condition == "inventory")
        required.insert({"itemId", "minimumCount"});
    else if (*condition == "money_at_most")
        required.insert("maximumCopper");
    else if (*condition == "money_decrease")
        required.insert("baselineCopper");
    else if (*condition == "known_recipe")
        required.insert("spellId");
    else if (*condition == "economy")
        required.insert({"afterSequence", "economyOutcome"});
    else
        return {.error = MakeError(ErrorCode::InvalidCondition)};

    if (!HasExactFields(fields, required, optional))
        return {.error = MakeError(ErrorCode::MalformedRequest)};

    if (*condition == "transport_attached" && fields.contains("transportEntry") &&
        !StoreUInt32(request, fields, "transportEntry"))
        return {.error = MakeError(ErrorCode::InvalidCondition)};
    if (*condition == "map" && !StoreUInt32(request, fields, "mapId", true))
        return {.error = MakeError(ErrorCode::InvalidCondition)};
    if (*condition == "profession_skill" &&
        (!StoreUInt32(request, fields, "skillId") || !StoreUInt32(request, fields, "minimumValue", true)))
        return {.error = MakeError(ErrorCode::InvalidCondition)};
    if (*condition == "inventory" &&
        (!StoreUInt32(request, fields, "itemId") || !StoreUInt32(request, fields, "minimumCount", true)))
        return {.error = MakeError(ErrorCode::InvalidCondition)};
    if (*condition == "known_recipe" && !StoreUInt32(request, fields, "spellId"))
        return {.error = MakeError(ErrorCode::InvalidCondition)};

    for (char const* name : {"afterSequence", "maximumCopper", "baselineCopper"})
    {
        if (fields.contains(name))
        {
            std::optional<uint64> value = UnsignedField(fields, name);
            if (!value)
                return {.error = MakeError(ErrorCode::InvalidCondition)};
            request.numbers[name] = *value;
        }
    }

    for (char const* name : {"actionName", "actionResult", "economyOutcome"})
    {
        if (fields.contains(name))
        {
            std::optional<std::string> value = StringField(fields, name);
            if (!value || value->empty())
                return {.error = MakeError(ErrorCode::InvalidCondition)};
            request.strings[name] = *value;
        }
    }

    if (*condition == "action")
    {
        std::string const& result = request.strings["actionResult"];
        if (result != "success" && result != "failure" && result != "either")
            return {.error = MakeError(ErrorCode::InvalidCondition)};
    }
    if (*condition == "economy")
    {
        std::string const& outcome = request.strings["economyOutcome"];
        if (outcome != "scheduled" && outcome != "operation" && outcome != "no_candidate" &&
            outcome != "failed_precondition" && outcome != "released" && outcome != "blocked" &&
            outcome != "quarantined")
            return {.error = MakeError(ErrorCode::InvalidCondition)};
    }

    return {.request = std::move(request)};
}

std::string SerializeError(uint64 requestId, ErrorCode code)
{
    ProtocolError const error = MakeError(code);
    std::string output = "{\"schemaVersion\":" + std::to_string(SCHEMA_VERSION) +
                         ",\"requestId\":" + std::to_string(requestId) + ",\"ok\":false,\"error\":{\"code\":\"" +
                         ErrorCodeName(code) + "\",\"message\":";
    AppendEscapedJsonString(output, error.message);
    output += "}}";
    return output;
}
}  // namespace

char const* PlayerbotVerification::ErrorCodeName(ErrorCode code)
{
    switch (code)
    {
        case ErrorCode::None:
            return "none";
        case ErrorCode::MalformedFrame:
            return "malformed_frame";
        case ErrorCode::FrameTooLarge:
            return "frame_too_large";
        case ErrorCode::MalformedRequest:
            return "malformed_request";
        case ErrorCode::AuthenticationFailed:
            return "authentication_failed";
        case ErrorCode::UnsupportedSchemaVersion:
            return "unsupported_schema_version";
        case ErrorCode::UnknownOperation:
            return "unknown_operation";
        case ErrorCode::InvalidGuid:
            return "invalid_guid";
        case ErrorCode::InvalidLimit:
            return "invalid_limit";
        case ErrorCode::InvalidCommand:
            return "invalid_command";
        case ErrorCode::InvalidCondition:
            return "invalid_condition";
        case ErrorCode::UnsupportedDestination:
            return "unsupported_destination";
        case ErrorCode::ResponseTooLarge:
            return "response_too_large";
        case ErrorCode::OperationUnavailable:
            return "operation_unavailable";
        case ErrorCode::QueueFull:
            return "queue_full";
        case ErrorCode::Timeout:
            return "timeout";
        case ErrorCode::Shutdown:
            return "shutdown";
        case ErrorCode::BotNotFound:
            return "bot_not_found";
        case ErrorCode::BotUnavailable:
            return "bot_unavailable";
        case ErrorCode::NotManagedPlayerbot:
            return "not_managed_playerbot";
        case ErrorCode::RecoveryFailed:
            return "recovery_failed";
        case ErrorCode::MasterNotFound:
            return "master_not_found";
        case ErrorCode::MasterIsBot:
            return "master_is_bot";
        case ErrorCode::InvalidRelationship:
            return "invalid_relationship";
        case ErrorCode::InvalidSkill:
            return "invalid_skill";
        case ErrorCode::GameObjectNotFound:
            return "gameobject_not_found";
        case ErrorCode::InternalError:
            return "internal_error";
    }
    return "internal_error";
}

char const* PlayerbotVerification::RecoveryDestinationName(RecoveryDestination destination)
{
    switch (destination)
    {
        case RecoveryDestination::Missing:
            return "missing";
        case RecoveryDestination::Homebind:
            return "homebind";
        case RecoveryDestination::Unsupported:
            return "unsupported";
    }
    return "missing";
}

char const* PlayerbotVerification::RecoveryOutcomeName(RecoveryOutcome outcome)
{
    switch (outcome)
    {
        case RecoveryOutcome::Recovered:
            return "recovered";
        case RecoveryOutcome::AlreadyAtHomebind:
            return "already_at_homebind";
        case RecoveryOutcome::InvalidRequest:
            return "invalid_request";
        case RecoveryOutcome::Unauthorized:
            return "unauthorized";
        case RecoveryOutcome::BotNotFound:
            return "bot_not_found";
        case RecoveryOutcome::BotNotAvailable:
            return "bot_not_available";
        case RecoveryOutcome::NotManagedPlayerbot:
            return "not_managed_playerbot";
        case RecoveryOutcome::UnsupportedDestination:
            return "unsupported_destination";
        case RecoveryOutcome::RecoveryFailed:
            return "recovery_failed";
        case RecoveryOutcome::RecoveryTimedOut:
            return "recovery_timed_out";
    }
    return "recovery_failed";
}

char const* PlayerbotVerification::RecoveryReasonName(RecoveryReason reason)
{
    switch (reason)
    {
        case RecoveryReason::HomebindTeleportAccepted:
            return "homebind_teleport_accepted";
        case RecoveryReason::CurrentHomebind:
            return "current_homebind";
        case RecoveryReason::PendingHomebind:
            return "pending_homebind";
        case RecoveryReason::MalformedRequest:
            return "malformed_request";
        case RecoveryReason::InvalidGuid:
            return "invalid_guid";
        case RecoveryReason::UnsupportedSchema:
            return "unsupported_schema";
        case RecoveryReason::UnknownOperation:
            return "unknown_operation";
        case RecoveryReason::InvalidToolInput:
            return "invalid_tool_input";
        case RecoveryReason::AuthenticationFailed:
            return "authentication_failed";
        case RecoveryReason::CharacterNotFound:
            return "character_not_found";
        case RecoveryReason::CharacterOffline:
            return "character_offline";
        case RecoveryReason::CharacterNotInWorld:
            return "character_not_in_world";
        case RecoveryReason::Dead:
            return "dead";
        case RecoveryReason::InCombat:
            return "in_combat";
        case RecoveryReason::Rooted:
            return "rooted";
        case RecoveryReason::InFlight:
            return "in_flight";
        case RecoveryReason::BattlegroundQueue:
            return "battleground_queue";
        case RecoveryReason::Battleground:
            return "battleground";
        case RecoveryReason::Arena:
            return "arena";
        case RecoveryReason::OnTransport:
            return "on_transport";
        case RecoveryReason::TeleportInProgress:
            return "teleport_in_progress";
        case RecoveryReason::PlayerbotAiMissing:
            return "playerbot_ai_missing";
        case RecoveryReason::DestinationNotHomebind:
            return "destination_not_homebind";
        case RecoveryReason::InvalidHomebind:
            return "invalid_homebind";
        case RecoveryReason::TeleportRejected:
            return "teleport_rejected";
        case RecoveryReason::OperationUnavailable:
            return "operation_unavailable";
        case RecoveryReason::QueueFull:
            return "queue_full";
        case RecoveryReason::ShuttingDown:
            return "shutting_down";
        case RecoveryReason::InternalError:
            return "internal_error";
        case RecoveryReason::ResponseTooLarge:
            return "response_too_large";
        case RecoveryReason::AdapterConfiguration:
            return "adapter_configuration";
        case RecoveryReason::ServerUnreachable:
            return "server_unreachable";
        case RecoveryReason::ProtocolMismatch:
            return "protocol_mismatch";
        case RecoveryReason::InvalidServerResponse:
            return "invalid_server_response";
        case RecoveryReason::QueueTimeoutBeforeClaim:
            return "queue_timeout_before_claim";
        case RecoveryReason::ExecutionTimeoutAfterClaim:
            return "execution_timeout_after_claim";
        case RecoveryReason::ClientLockTimeout:
            return "client_lock_timeout";
        case RecoveryReason::SocketTimeout:
            return "socket_timeout";
        case RecoveryReason::ResponseTimeout:
            return "response_timeout";
    }
    return "internal_error";
}

char const* PlayerbotVerification::RecoveryMutationStateName(RecoveryMutationState state)
{
    switch (state)
    {
        case RecoveryMutationState::NotStarted:
            return "not_started";
        case RecoveryMutationState::Completed:
            return "completed";
        case RecoveryMutationState::UnknownAfterExecutionStarted:
            return "unknown_after_execution_started";
    }
    return "not_started";
}

char const* PlayerbotVerification::RecoveryPersistenceStateName(RecoveryPersistenceState state)
{
    switch (state)
    {
        case RecoveryPersistenceState::NotRequested:
            return "not_requested";
        case RecoveryPersistenceState::Deferred:
            return "deferred";
    }
    return "not_requested";
}

std::optional<RecoveryAuditRecord> PlayerbotVerification::BuildRejectedRecoveryAudit(std::string const& payload,
                                                                                     ErrorCode error,
                                                                                     uint64 timestampMs)
{
    std::set<std::string> duplicateFields;
    std::optional<FlatJsonFields> fields = FlatJsonParser(payload).Parse(&duplicateFields);
    if (!fields)
        return std::nullopt;

    std::optional<std::string> operation = StringField(*fields, "operation");
    if (!operation || *operation != "recover" || duplicateFields.contains("operation"))
        return std::nullopt;

    RecoveryAuditRecord record;
    record.timestampMs = timestampMs;
    std::optional<uint64> requestId = UnsignedField(*fields, "requestId");
    if (requestId && !duplicateFields.contains("requestId"))
        record.requestId = *requestId;
    std::optional<uint64> botGuid = UnsignedField(*fields, "botGuid");
    if (botGuid && IsValidGuid(*botGuid) && !duplicateFields.contains("botGuid"))
        record.botGuid = static_cast<uint32>(*botGuid);
    std::optional<std::string> destination = StringField(*fields, "destination");
    if (destination && !duplicateFields.contains("destination"))
        record.destination =
            *destination == "homebind" ? RecoveryDestination::Homebind : RecoveryDestination::Unsupported;

    switch (error)
    {
        case ErrorCode::AuthenticationFailed:
            record.outcome = RecoveryOutcome::Unauthorized;
            record.reason = RecoveryReason::AuthenticationFailed;
            break;
        case ErrorCode::UnsupportedDestination:
            record.outcome = RecoveryOutcome::UnsupportedDestination;
            record.reason = RecoveryReason::DestinationNotHomebind;
            break;
        case ErrorCode::InvalidGuid:
            record.outcome = RecoveryOutcome::InvalidRequest;
            record.reason = RecoveryReason::InvalidGuid;
            break;
        case ErrorCode::UnsupportedSchemaVersion:
            record.outcome = RecoveryOutcome::InvalidRequest;
            record.reason = RecoveryReason::UnsupportedSchema;
            break;
        case ErrorCode::UnknownOperation:
            record.outcome = RecoveryOutcome::InvalidRequest;
            record.reason = RecoveryReason::UnknownOperation;
            break;
        case ErrorCode::BotNotFound:
            record.outcome = RecoveryOutcome::BotNotFound;
            record.reason = RecoveryReason::CharacterNotFound;
            break;
        case ErrorCode::BotUnavailable:
            record.outcome = RecoveryOutcome::BotNotAvailable;
            record.reason = RecoveryReason::CharacterOffline;
            break;
        case ErrorCode::NotManagedPlayerbot:
            record.outcome = RecoveryOutcome::NotManagedPlayerbot;
            record.reason = RecoveryReason::PlayerbotAiMissing;
            break;
        case ErrorCode::OperationUnavailable:
            record.outcome = RecoveryOutcome::RecoveryFailed;
            record.reason = RecoveryReason::OperationUnavailable;
            break;
        case ErrorCode::QueueFull:
            record.outcome = RecoveryOutcome::RecoveryFailed;
            record.reason = RecoveryReason::QueueFull;
            break;
        case ErrorCode::Shutdown:
            record.outcome = RecoveryOutcome::RecoveryFailed;
            record.reason = RecoveryReason::ShuttingDown;
            break;
        case ErrorCode::ResponseTooLarge:
            record.outcome = RecoveryOutcome::RecoveryFailed;
            record.reason = RecoveryReason::ResponseTooLarge;
            break;
        case ErrorCode::Timeout:
            record.outcome = RecoveryOutcome::RecoveryTimedOut;
            record.reason = RecoveryReason::QueueTimeoutBeforeClaim;
            break;
        case ErrorCode::RecoveryFailed:
        case ErrorCode::InternalError:
            record.outcome = RecoveryOutcome::RecoveryFailed;
            record.reason = RecoveryReason::InternalError;
            break;
        default:
            record.outcome = RecoveryOutcome::InvalidRequest;
            record.reason = RecoveryReason::MalformedRequest;
            break;
    }
    return record;
}

std::string PlayerbotVerification::SerializeRecoveryAuditRecord(RecoveryAuditRecord const& record)
{
    std::string output = "{\"timestampMs\":" + std::to_string(record.timestampMs) + ",\"operation\":\"recover\"";
    if (record.requestId)
        output += ",\"requestId\":" + std::to_string(*record.requestId);
    if (record.botGuid)
        output += ",\"botGuid\":" + std::to_string(*record.botGuid);
    if (record.botName)
    {
        output += ",\"botName\":";
        AppendEscapedJsonString(output, *record.botName);
    }
    output += ",\"destination\":\"" + std::string(RecoveryDestinationName(record.destination)) + '"';
    if (record.beforePosition)
        AppendRecoveryPosition(output, "beforePosition", *record.beforePosition);
    if (record.acceptedDestination)
        AppendRecoveryPosition(output, "acceptedDestination", *record.acceptedDestination);
    if (record.observedPosition)
        AppendRecoveryPosition(output, "observedPosition", *record.observedPosition);
    output += ",\"observedAtDestination\":" + std::string(record.observedAtDestination ? "true" : "false") +
              ",\"movementReset\":" + std::string(record.movementReset ? "true" : "false") +
              ",\"travelReset\":" + std::string(record.travelReset ? "true" : "false") +
              ",\"taxiReset\":" + std::string(record.taxiReset ? "true" : "false") + ",\"outcome\":\"" +
              RecoveryOutcomeName(record.outcome) + "\",\"reason\":\"" + RecoveryReasonName(record.reason) +
              "\",\"mutationState\":\"" + RecoveryMutationStateName(record.mutationState) +
              "\",\"persistenceState\":\"" + RecoveryPersistenceStateName(record.persistenceState) + "\"}";
    return output;
}

std::optional<std::vector<uint8>> PlayerbotVerification::EncodeFrame(std::string const& payload)
{
    if (payload.size() > MAX_FRAME_PAYLOAD_BYTES)
        return std::nullopt;

    uint32 const length = static_cast<uint32>(payload.size());
    std::vector<uint8> frame;
    frame.reserve(FRAME_HEADER_BYTES + payload.size());
    frame.push_back(static_cast<uint8>((length >> 24) & 0xFF));
    frame.push_back(static_cast<uint8>((length >> 16) & 0xFF));
    frame.push_back(static_cast<uint8>((length >> 8) & 0xFF));
    frame.push_back(static_cast<uint8>(length & 0xFF));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

FrameDecodeResult PlayerbotVerification::DecodeFrame(std::span<uint8 const> frame)
{
    if (frame.size() < FRAME_HEADER_BYTES)
        return {.error = MakeError(ErrorCode::MalformedFrame)};

    uint32 const length = (static_cast<uint32>(frame[0]) << 24) | (static_cast<uint32>(frame[1]) << 16) |
                          (static_cast<uint32>(frame[2]) << 8) | static_cast<uint32>(frame[3]);
    if (length > MAX_FRAME_PAYLOAD_BYTES)
        return {.error = MakeError(ErrorCode::FrameTooLarge)};
    if (frame.size() != FRAME_HEADER_BYTES + length)
        return {.error = MakeError(ErrorCode::MalformedFrame)};

    return {.payload = std::string(frame.begin() + FRAME_HEADER_BYTES, frame.end())};
}

TokenDigest PlayerbotVerification::DigestVerificationToken(std::string_view token)
{
    static_assert(TOKEN_DIGEST_BYTES == Acore::Crypto::SHA256::DIGEST_LENGTH);
    return Acore::Crypto::SHA256::GetDigestOf(token);
}

std::optional<TokenDigest> PlayerbotVerification::VerificationTokenDigestFromEnvironment()
{
    char const* rawToken = std::getenv("PLAYERBOT_VERIFICATION_TOKEN");
    if (!rawToken)
        return std::nullopt;

    std::string_view const token(rawToken);
    if (token.size() < MIN_TOKEN_BYTES)
        return std::nullopt;
    return DigestVerificationToken(token);
}

bool PlayerbotVerification::ConstantTimeTokenDigestEquals(TokenDigest const& candidate, TokenDigest const& expected)
{
    volatile uint8 difference = 0;
    for (std::size_t index = 0; index < candidate.size(); ++index)
        difference = difference | static_cast<uint8>(candidate[index] ^ expected[index]);
    return difference == 0;
}

RequestParseResult PlayerbotVerification::ParseRequestPayload(std::string const& payload,
                                                              TokenDigest const& expectedTokenDigest)
{
    std::optional<FlatJsonFields> fields = FlatJsonParser(payload).Parse();
    if (!fields)
        return {.error = MakeError(ErrorCode::MalformedRequest)};

    std::optional<uint64> requestId = UnsignedField(*fields, "requestId");
    uint64 const responseRequestId = requestId.value_or(0);
    std::optional<std::string> token = StringField(*fields, "token");
    if (!token || !ConstantTimeTokenDigestEquals(DigestVerificationToken(*token), expectedTokenDigest))
        return {.responseRequestId = responseRequestId, .error = MakeError(ErrorCode::AuthenticationFailed)};

    std::optional<uint64> schemaVersion = UnsignedField(*fields, "schemaVersion");
    if (!schemaVersion)
        return {.error = MakeError(ErrorCode::MalformedRequest)};
    if (*schemaVersion != SCHEMA_VERSION)
        return {.error = MakeError(ErrorCode::UnsupportedSchemaVersion)};

    std::optional<std::string> operationName = StringField(*fields, "operation");
    if (!requestId || !operationName)
        return {.error = MakeError(ErrorCode::MalformedRequest)};

    Request request;
    request.requestId = *requestId;
    if (*operationName == "status")
    {
        if (!HasExactFields(*fields, CommonFields({})))
            return {.error = MakeError(ErrorCode::MalformedRequest)};
        request.operation = Operation::Status;
        return {.request = std::move(request)};
    }
    if (*operationName == "list")
    {
        if (!HasExactFields(*fields, CommonFields({"afterGuid", "limit"})))
            return {.error = MakeError(ErrorCode::MalformedRequest)};
        std::optional<uint64> afterGuid = UnsignedField(*fields, "afterGuid");
        std::optional<uint64> limit = UnsignedField(*fields, "limit");
        if (!afterGuid || *afterGuid > std::numeric_limits<uint32>::max())
            return {.error = MakeError(ErrorCode::InvalidGuid)};
        if (!limit || !*limit || *limit > MAX_LIST_LIMIT)
            return {.error = MakeError(ErrorCode::InvalidLimit)};
        request.operation = Operation::List;
        request.afterGuid = static_cast<uint32>(*afterGuid);
        request.limit = static_cast<uint32>(*limit);
        return {.request = std::move(request)};
    }
    if (*operationName == "inspect")
    {
        if (!HasExactFields(*fields, CommonFields({"botGuid"})))
            return {.error = MakeError(ErrorCode::MalformedRequest)};
        std::optional<uint64> botGuid = UnsignedField(*fields, "botGuid");
        if (!botGuid || !IsValidGuid(*botGuid))
            return {.error = MakeError(ErrorCode::InvalidGuid)};
        request.operation = Operation::Inspect;
        request.botGuid = static_cast<uint32>(*botGuid);
        return {.request = std::move(request)};
    }
    if (*operationName == "anomalies")
    {
        if (!HasExactFields(*fields, CommonFields({"limit"})))
            return {.error = MakeError(ErrorCode::MalformedRequest)};
        std::optional<uint64> limit = UnsignedField(*fields, "limit");
        if (!limit || !*limit || *limit > MAX_ANOMALY_LIMIT)
            return {.error = {ErrorCode::InvalidLimit, "The anomaly limit must be from 1 through 50."}};
        request.operation = Operation::Anomalies;
        request.limit = static_cast<uint32>(*limit);
        return {.request = std::move(request)};
    }
    if (*operationName == "command")
    {
        if (!HasExactFields(*fields, CommonFields({"botGuid", "masterGuid", "command"})))
            return {.error = MakeError(ErrorCode::MalformedRequest)};
        std::optional<uint64> botGuid = UnsignedField(*fields, "botGuid");
        std::optional<uint64> masterGuid = UnsignedField(*fields, "masterGuid");
        std::optional<std::string> command = StringField(*fields, "command");
        if (!botGuid || !masterGuid || !IsValidGuid(*botGuid) || !IsValidGuid(*masterGuid))
            return {.error = MakeError(ErrorCode::InvalidGuid)};
        if (!command || command->empty())
            return {.error = MakeError(ErrorCode::InvalidCommand)};
        request.operation = Operation::Command;
        request.botGuid = static_cast<uint32>(*botGuid);
        request.masterGuid = static_cast<uint32>(*masterGuid);
        request.command = std::move(*command);
        return {.request = std::move(request)};
    }
    if (*operationName == "recover")
    {
        if (!HasExactFields(*fields, CommonFields({"botGuid", "destination"})))
            return {.error = MakeError(ErrorCode::MalformedRequest)};
        std::optional<uint64> botGuid = UnsignedField(*fields, "botGuid");
        std::optional<std::string> destination = StringField(*fields, "destination");
        if (!botGuid || !IsValidGuid(*botGuid))
            return {.error = MakeError(ErrorCode::InvalidGuid)};
        if (!destination)
            return {.error = MakeError(ErrorCode::MalformedRequest)};
        if (*destination != "homebind")
            return {.error = MakeError(ErrorCode::UnsupportedDestination)};
        request.operation = Operation::Recover;
        request.botGuid = static_cast<uint32>(*botGuid);
        request.destination = std::move(*destination);
        return {.request = std::move(request)};
    }
    if (*operationName == "set_skill")
    {
        if (!HasExactFields(*fields, CommonFields({"botGuid", "skillId", "value", "maximum"})))
            return {.error = MakeError(ErrorCode::MalformedRequest)};
        std::optional<uint64> botGuid = UnsignedField(*fields, "botGuid");
        if (!botGuid || !IsValidGuid(*botGuid))
            return {.error = MakeError(ErrorCode::InvalidGuid)};
        if (!StoreUInt32(request, *fields, "skillId") || !StoreUInt32(request, *fields, "value") ||
            !StoreUInt32(request, *fields, "maximum"))
            return {.error = MakeError(ErrorCode::InvalidSkill)};
        uint64 const value = request.numbers["value"];
        uint64 const maximum = request.numbers["maximum"];
        if (maximum > MAX_SKILL_MAXIMUM || maximum % SKILL_RANK_STEP != 0 || value > maximum)
            return {.error = MakeError(ErrorCode::InvalidSkill)};
        request.operation = Operation::SetSkill;
        request.botGuid = static_cast<uint32>(*botGuid);
        return {.request = std::move(request)};
    }
    if (*operationName == "teleport_to_gameobject")
    {
        if (!HasExactFields(*fields, CommonFields({"botGuid", "gameObjectEntry"})))
            return {.error = MakeError(ErrorCode::MalformedRequest)};
        std::optional<uint64> botGuid = UnsignedField(*fields, "botGuid");
        if (!botGuid || !IsValidGuid(*botGuid))
            return {.error = MakeError(ErrorCode::InvalidGuid)};
        if (!StoreUInt32(request, *fields, "gameObjectEntry"))
            return {.error = MakeError(ErrorCode::GameObjectNotFound)};
        request.operation = Operation::TeleportToGameObject;
        request.botGuid = static_cast<uint32>(*botGuid);
        return {.request = std::move(request)};
    }
    if (*operationName == "gm_command")
    {
        if (!HasExactFields(*fields, CommonFields({"command"})))
            return {.error = MakeError(ErrorCode::MalformedRequest)};
        std::optional<std::string> command = StringField(*fields, "command");
        if (!command || command->empty() || IsRefusedGmCommand(*command))
            return {.error = MakeError(ErrorCode::InvalidCommand)};
        request.operation = Operation::GmCommand;
        request.command = std::move(*command);
        return {.request = std::move(request)};
    }
    if (*operationName == "check")
    {
        std::optional<uint64> botGuid = UnsignedField(*fields, "botGuid");
        if (!botGuid || !IsValidGuid(*botGuid))
            return {.error = MakeError(ErrorCode::InvalidGuid)};
        request.operation = Operation::Check;
        request.botGuid = static_cast<uint32>(*botGuid);
        return ParseCheck(*fields, std::move(request));
    }

    return {.error = MakeError(ErrorCode::UnknownOperation)};
}

bool PlayerbotVerification::IsRefusedGmCommand(std::string_view command)
{
    std::size_t begin = command.find_first_not_of(" \t.");
    if (begin == std::string_view::npos)
        return true;
    std::size_t const end = command.find_first_of(" \t", begin);
    std::string_view const word =
        command.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
    static constexpr std::string_view refused[] = {"server", "account", "quit", "exit"};
    return std::any_of(std::begin(refused), std::end(refused),
                       [word](std::string_view const entry)
                       {
                           return word.size() == entry.size() &&
                                  std::equal(word.begin(), word.end(), entry.begin(), [](char left, char right)
                                             { return std::tolower(static_cast<unsigned char>(left)) == right; });
                       });
}

Response Response::Success(std::string resultJson) { return {.ok = true, .resultJson = std::move(resultJson)}; }

Response Response::Failure(ErrorCode code, std::string message)
{
    return {.error = {.code = code, .message = std::move(message)}};
}

std::string PlayerbotVerification::SerializeResponse(uint64 requestId, Response const& response)
{
    if (!response.ok)
        return SerializeError(requestId, response.error.code);

    std::string const& result = response.resultJson.empty() ? std::string("{}") : response.resultJson;
    std::string output = "{\"schemaVersion\":" + std::to_string(SCHEMA_VERSION) +
                         ",\"requestId\":" + std::to_string(requestId) + ",\"ok\":true,\"result\":" + result + '}';
    if (output.size() <= MAX_RESPONSE_PAYLOAD_BYTES)
        return output;
    return SerializeError(requestId, ErrorCode::ResponseTooLarge);
}

std::optional<GuidPage> PlayerbotVerification::PaginateGuids(std::vector<uint32> guids, uint32 afterGuid, uint32 limit)
{
    if (!limit || limit > MAX_LIST_LIMIT)
        return std::nullopt;

    std::sort(guids.begin(), guids.end());
    guids.erase(std::unique(guids.begin(), guids.end()), guids.end());
    auto const first = std::upper_bound(guids.begin(), guids.end(), afterGuid);
    std::size_t const available = static_cast<std::size_t>(guids.end() - first);
    std::size_t const returned = std::min<std::size_t>(available, limit);

    GuidPage page;
    page.guids.assign(first, first + returned);
    page.nextAfterGuid = page.guids.empty() ? afterGuid : page.guids.back();
    page.hasMore = returned < available;
    return page;
}
