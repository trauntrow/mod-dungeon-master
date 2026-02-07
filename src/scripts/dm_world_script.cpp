/*
 * Copyright (C) 2025 AzerothCore - mod-dungeon-master
 *
 * dm_world_script.cpp — Hooks server lifecycle events:
 *   OnAfterConfigLoad → loads/reloads config
 *   OnStartup         → initializes the manager
 *   OnUpdate           → ticks the session update loop
 *   OnShutdown         → logs active sessions
 */

#include "ScriptMgr.h"
#include "DungeonMasterMgr.h"
#include "DMConfig.h"
#include "Log.h"

using namespace DungeonMaster;

class dm_world_script : public WorldScript
{
public:
    dm_world_script() : WorldScript("dm_world_script") {}

    void OnAfterConfigLoad(bool reload) override
    {
        sDMConfig->LoadConfig(reload);
    }

    void OnStartup() override
    {
        if (!sDMConfig->IsEnabled())
        {
            LOG_INFO("module", "DungeonMaster: Disabled in configuration.");
            return;
        }

        sDungeonMasterMgr->Initialize();

        LOG_INFO("module", "===============================================");
        LOG_INFO("module", " Dungeon Master Module — Ready");
        LOG_INFO("module", " {} difficulties | {} themes | {} dungeons",
            sDMConfig->GetDifficulties().size(),
            sDMConfig->GetThemes().size(),
            sDMConfig->GetDungeons().size());
        LOG_INFO("module", " Level band: +/-{} | Max concurrent: {}",
            sDMConfig->GetLevelBand(), sDMConfig->GetMaxConcurrentRuns());
        LOG_INFO("module", "===============================================");
    }

    void OnShutdown() override
    {
        if (!sDMConfig->IsEnabled()) return;
        LOG_INFO("module", "DungeonMaster: Shutdown — {} sessions active.",
            sDungeonMasterMgr->GetActiveSessionCount());
    }

    void OnUpdate(uint32 diff) override
    {
        if (sDMConfig->IsEnabled())
            sDungeonMasterMgr->Update(diff);
    }
};

void AddSC_dm_world_script()
{
    new dm_world_script();
}
