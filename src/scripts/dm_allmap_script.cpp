/*
 * Copyright (C) 2025 AzerothCore - mod-dungeon-master
 *
 * dm_allmap_script.cpp — Triggers dungeon population when the session
 * leader enters the instance map for the first time.
 */

#include "ScriptMgr.h"
#include "Map.h"
#include "Player.h"
#include "DungeonMasterMgr.h"
#include "DMConfig.h"
#include "Chat.h"
#include "Log.h"
#include <cstdio>

using namespace DungeonMaster;

class dm_allmap_script : public AllMapScript
{
public:
    dm_allmap_script() : AllMapScript("dm_allmap_script") {}

    void OnPlayerEnterAll(Map* map, Player* player) override
    {
        if (!sDMConfig->IsEnabled() || !map || !player)
            return;

        Session* session = sDungeonMasterMgr->GetSessionByPlayer(player->GetGUID());
        if (!session || session->State != SessionState::InProgress)
            return;

        if (map->GetId() != session->MapId)
            return;

        // Only populate once — when leader enters and no mobs exist yet.
        if (player->GetGUID() != session->LeaderGuid || session->TotalMobs > 0)
            return;

        if (!map->IsDungeon())
            return;

        InstanceMap* instance = map->ToInstanceMap();
        if (!instance)
            return;

        session->InstanceId = instance->GetInstanceId();

        ChatHandler(player->GetSession()).SendSysMessage(
            "|cFF00FF00[Dungeon Master]|r Preparing the challenge...");

        sDungeonMasterMgr->PopulateDungeon(session, instance);

        char buf[256];
        snprintf(buf, sizeof(buf),
            "|cFF00FF00[Dungeon Master]|r |cFFFFFFFF%u|r enemies and |cFFFFFFFF%u|r boss(es) spawned. "
            "Creature levels: |cFFFFFFFF%u-%u|r. Good luck!",
            session->TotalMobs, session->TotalBosses,
            session->LevelBandMin, session->LevelBandMax);
        ChatHandler(player->GetSession()).SendSysMessage(buf);
    }
};

void AddSC_dm_allmap_script()
{
    new dm_allmap_script();
}
