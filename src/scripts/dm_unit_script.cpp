/*
 * mod-dungeon-master — dm_unit_script.cpp
 * Scales ALL incoming damage for session players:
 *   - Session boss spells/melee: scaled by level ratio (template level → session level)
 *   - Session trash: already scaled by custom AI melee, passed through
 *   - Environmental (non-session): capped at 3% max HP
 */

#include "ScriptMgr.h"
#include "Player.h"
#include "Creature.h"
#include "SpellInfo.h"
#include "DungeonMasterMgr.h"
#include "DMConfig.h"

using namespace DungeonMaster;

static constexpr float ENV_DAMAGE_MAX_PCT = 0.03f;

class dm_unit_script : public UnitScript
{
public:
    dm_unit_script() : UnitScript("dm_unit_script") {}

    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage, SpellInfo const* /*spellInfo*/) override
    {
        ScaleDamage(target, attacker, damage);
    }

    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* /*spellInfo*/) override
    {
        if (damage <= 0) return;
        uint32 udmg = static_cast<uint32>(damage);
        ScaleDamage(target, attacker, udmg);
        damage = static_cast<int32>(udmg);
    }

    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override
    {
        ScaleDamage(target, attacker, damage);
    }

    void OnUnitDeath(Unit* unit, Unit* killer) override
    {
        if (!sDMConfig->IsEnabled() || !unit)
            return;

        Creature* creature = unit->ToCreature();
        if (!creature)
            return;

        Player* player = nullptr;
        if (killer)
        {
            player = killer->ToPlayer();
            if (!player && killer->GetOwner())
                player = killer->GetOwner()->ToPlayer();
        }

        Session* session = nullptr;
        if (player)
            session = sDungeonMasterMgr->GetSessionByPlayer(player->GetGUID());

        if (!session || !session->IsActive())
            return;

        if (creature->GetMapId() != session->MapId)
            return;

        sDungeonMasterMgr->HandleCreatureDeath(creature, session);
    }

private:
    void ScaleDamage(Unit* target, Unit* attacker, uint32& damage)
    {
        if (!sDMConfig->IsEnabled() || damage == 0)
            return;

        Player* player = target ? target->ToPlayer() : nullptr;
        if (!player)
            return;

        if (attacker && attacker->ToPlayer())
            return;

        ObjectGuid playerGuid = player->GetGUID();

        if (!sDungeonMasterMgr->GetSessionByPlayer(playerGuid))
            return;

        if (attacker)
        {
            ObjectGuid attackerGuid = attacker->GetGUID();

            // Session creature damage — scale bosses, pass through trash
            if (sDungeonMasterMgr->IsSessionCreature(playerGuid, attackerGuid))
            {
                float scale = sDungeonMasterMgr->GetSessionCreatureDamageScale(
                    playerGuid, attackerGuid);

                if (scale < 1.0f)
                    damage = std::max(1u, static_cast<uint32>(damage * scale));

                return;
            }
        }

        // Non-session attacker (environmental hazards, traps, etc.)
        float envScale = sDungeonMasterMgr->GetEnvironmentalDamageScale(playerGuid);
        if (envScale < 1.0f)
            damage = static_cast<uint32>(damage * envScale);

        uint32 maxHp = player->GetMaxHealth();
        uint32 cap   = std::max(1u, static_cast<uint32>(maxHp * ENV_DAMAGE_MAX_PCT));

        if (damage > cap)
            damage = cap;

        if (damage == 0)
            damage = 1;
    }
};

void AddSC_dm_unit_script()
{
    new dm_unit_script();
}
