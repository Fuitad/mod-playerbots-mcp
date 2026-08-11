/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTMCPCONFIG_H
#define PLAYERBOTS_PLAYERBOTMCPCONFIG_H

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

struct PlayerbotMCPSettings
{
    std::uint32_t port = 0;
};

template <class Lookup>
PlayerbotMCPSettings LoadPlayerbotMCPSettings(Lookup&& lookup)
{
    PlayerbotMCPSettings settings;
    std::optional<std::string> const value = lookup("PlayerbotsMCP.Port");
    if (!value || value->empty())
        return settings;

    std::uint32_t parsed = 0;
    auto const result = std::from_chars(value->data(), value->data() + value->size(), parsed);
    if (result.ec == std::errc() && result.ptr == value->data() + value->size())
        settings.port = parsed;
    return settings;
}

extern PlayerbotMCPSettings sPlayerbotMCPConfig;
void ReloadPlayerbotMCPConfig();

#endif
