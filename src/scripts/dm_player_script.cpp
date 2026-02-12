/*
 * mod-dungeon-master — dm_player_script.cpp
 * Player death handling: blocks spirit release, checks for wipe.
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

    // Reliable kill hook — fires for ALL creature kills regardless of AI.
    // Bosses from the dungeon boss pool have ScriptName-based AI that can
    // override our DungeonMasterCreatureAI, so JustDied may never fire.
    // This ensures loot and kill credit always happen.
    void OnCreatureKill(Player* player, Creature* creature) override
    {
        if (!sDMConfig->IsEnabled() || !player || !creature)
            return;

        Session* session = sDungeonMasterMgr->GetSessionByPlayer(player->GetGUID());
        if (!session || !session->IsActive())
            return;

        if (creature->GetMapId() != session->MapId)
            return;

        sDungeonMasterMgr->HandleCreatureDeath(creature, session);
    }
};

void AddSC_dm_player_script()
{
    new dm_player_script();
}
