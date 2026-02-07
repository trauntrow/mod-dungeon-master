/*
 * Copyright (C) 2025 AzerothCore - mod-dungeon-master
 *
 * dm_player_script.cpp — Hooks player death in DM sessions.
 *
 * On death: blocks spirit release, checks for wipe.
 * Auto-resurrect is handled in the Update() loop when combat ends.
 */

#include "ScriptMgr.h"
#include "Player.h"
#include "Creature.h"
#include "DungeonMasterMgr.h"
#include "DMConfig.h"

using namespace DungeonMaster;

class dm_player_script : public PlayerScript
{
public:
    dm_player_script() : PlayerScript("dm_player_script") {}

    void OnPlayerKilledByCreature(Creature* /*killer*/, Player* player) override
    {
        if (!sDMConfig->IsEnabled() || !player)
            return;

        Session* session = sDungeonMasterMgr->GetSessionByPlayer(player->GetGUID());
        if (!session || !session->IsActive())
            return;

        if (player->GetMapId() != session->MapId)
            return;

        sDungeonMasterMgr->HandlePlayerDeath(player, session);
    }
};

void AddSC_dm_player_script()
{
    new dm_player_script();
}
