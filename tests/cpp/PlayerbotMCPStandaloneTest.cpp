#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "Bot/MCP/PlayerbotMCPConfig.h"

namespace
{
void Require(bool condition, std::string_view message)
{
    if (condition)
        return;

    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}
}  // namespace

int main()
{
    std::unordered_map<std::string, std::string> const values = {
        {"PlayerbotsMCP.Port", "24601"},
    };
    PlayerbotMCPSettings const settings = LoadPlayerbotMCPSettings(
        [&values](std::string_view key) -> std::optional<std::string>
        {
            auto const found = values.find(std::string(key));
            return found == values.end() ? std::nullopt : std::optional<std::string>(found->second);
        });
    Require(settings.port == 24601, "nondefault MCP port was not loaded");
    Require(settings.enable, "MCP must stay enabled when PlayerbotsMCP.Enable is absent");

    std::unordered_map<std::string, std::string> const disabledValues = {
        {"PlayerbotsMCP.Enable", "0"},
        {"PlayerbotsMCP.Port", "24601"},
    };
    PlayerbotMCPSettings const disabled = LoadPlayerbotMCPSettings(
        [&disabledValues](std::string_view key) -> std::optional<std::string>
        {
            auto const found = disabledValues.find(std::string(key));
            return found == disabledValues.end() ? std::nullopt : std::optional<std::string>(found->second);
        });
    Require(!disabled.enable, "PlayerbotsMCP.Enable = 0 did not disable the module");
    return EXIT_SUCCESS;
}
