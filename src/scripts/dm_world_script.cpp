/*
 * mod-dungeon-master — dm_world_script.cpp
 * Server lifecycle hooks: config load, startup, update tick, shutdown.
 */

#include "ScriptMgr.h"
#include "DungeonMasterMgr.h"
#include "RoguelikeMgr.h"
#include "DMConfig.h"
#include "SpellMgr.h"
#include "SpellInfo.h"
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

        // Patch Greater Blessing of Kings (25898) to allow stacking.
        // The base DBC has StackAmount=0 which prevents the client from
        // showing a stack count on the buff icon.  Setting it server-side
        // to 20 lets SetStackAmount() work and the client renders "2", "3"
        // etc. on the icon during roguelike runs.
        SpellInfo* bokInfo = const_cast<SpellInfo*>(
            sSpellMgr->GetSpellInfo(25898));
        if (bokInfo)
        {
            bokInfo->StackAmount = 255;
            LOG_INFO("module", "DungeonMaster: Patched BoK (25898) StackAmount → 255 for roguelike buff stacking.");
        }

        sDungeonMasterMgr->Initialize();
        sRoguelikeMgr->Initialize();

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
        {
            sDungeonMasterMgr->Update(diff);
            sRoguelikeMgr->Update(diff);
        }
    }
};

void AddSC_dm_world_script()
{
    new dm_world_script();
}
