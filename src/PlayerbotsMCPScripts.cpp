/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Bot/MCP/PlayerbotMCPConfig.h"
#include "Bot/MCP/PlayerbotVerificationServer.h"
#include "Playerbots.h"
#include "WorldScript.h"

namespace
{
class PlayerbotsMCPWorldScript final : public WorldScript
{
public:
    PlayerbotsMCPWorldScript()
        : WorldScript("PlayerbotsMCPWorldScript",
                      {WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED, WORLDHOOK_ON_SHUTDOWN})
    {
    }

    void OnAfterConfigLoad(bool) override { ReloadPlayerbotMCPConfig(); }

    void OnBeforeWorldInitialized() override
    {
        if (!sPlayerbotMCPConfig.enable)
        {
            LOG_INFO("server.loading", "Playerbots MCP server disabled by PlayerbotsMCP.Enable = 0");
            return;
        }

        if (!sPlayerbotMCPConfig.port)
            return;

        if (server.Start(sPlayerbotMCPConfig.port))
        {
            LOG_INFO("server.loading", "Playerbots MCP server listening on 127.0.0.1:{}", sPlayerbotMCPConfig.port);
            return;
        }

        LOG_ERROR("server.loading",
                  "Playerbots MCP server on port {} stayed disabled. Check that "
                  "PLAYERBOT_VERIFICATION_TOKEN is set with at least 32 bytes and the port is free.",
                  sPlayerbotMCPConfig.port);
    }

    void OnShutdown() override { server.Stop(); }

private:
    PlayerbotVerificationServer server;
};
}  // namespace

void AddPlayerbotsMCPScripts() { new PlayerbotsMCPWorldScript(); }
