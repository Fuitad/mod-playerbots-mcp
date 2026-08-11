/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotMCPConfig.h"

#include "Config.h"

PlayerbotMCPSettings sPlayerbotMCPConfig;

void ReloadPlayerbotMCPConfig()
{
    sPlayerbotMCPConfig = LoadPlayerbotMCPSettings(
        [](std::string_view key) -> std::optional<std::string>
        {
            std::string const value = sConfigMgr->GetOption<std::string>(std::string(key), "");
            return value.empty() ? std::nullopt : std::optional<std::string>(value);
        });
}
