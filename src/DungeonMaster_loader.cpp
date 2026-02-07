/*
 * Copyright (C) 2025 AzerothCore - mod-dungeon-master
 *
 * DungeonMaster_loader.cpp — Module entry point.
 *
 * Function name MUST match AC_ADD_SCRIPT_LOADER's expectation:
 *   For module "mod-dungeon-master" → Addmod_dungeon_masterScripts()
 */

#include "ScriptMgr.h"
#include "Log.h"

void AddSC_npc_dungeon_master();
void AddSC_dm_player_script();
void AddSC_dm_world_script();
void AddSC_dm_allmap_script();
void AddSC_dm_command_script();

void Addmod_dungeon_masterScripts()
{
    LOG_INFO("module", "DungeonMaster: Registering scripts...");

    AddSC_npc_dungeon_master();
    AddSC_dm_player_script();
    AddSC_dm_world_script();
    AddSC_dm_allmap_script();
    AddSC_dm_command_script();
}
