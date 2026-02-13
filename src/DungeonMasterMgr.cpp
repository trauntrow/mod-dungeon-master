/*
 * mod-dungeon-master — DungeonMasterMgr.cpp
 * Central session manager: creature pools, spawning, scaling, rewards, update loop.
 */

#include "DungeonMasterMgr.h"
#include "RoguelikeMgr.h"
#include "DMConfig.h"
#include "Player.h"
#include "Group.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "Map.h"
#include "MapMgr.h"
#include "GameObject.h"
#include "ObjectMgr.h"
#include "WorldSession.h"
#include "World.h"
#include "Chat.h"
#include "GameTime.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "MotionMaster.h"
#include "Mail.h"
#include "Item.h"
#include "LootMgr.h"
#include "SpellAuras.h"
#include "SpellAuraEffects.h"
#include "InstanceScript.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include <random>
#include <algorithm>
#include <set>
#include <cstdio>
#include <cmath>

namespace DungeonMaster
{

// RNG helpers (thread-local for safety)
static thread_local std::mt19937 tRng{ std::random_device{}() };

template<typename T>
static T RandInt(T lo, T hi) { return std::uniform_int_distribution<T>(lo, hi)(tRng); }

static float RandFloat(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(tRng); }

// Aggressive AI for DM-spawned creatures; patrols 5 yd radius, active aggro, hooks JustDied for loot
class DungeonMasterCreatureAI : public CreatureAI
{
public:
    explicit DungeonMasterCreatureAI(Creature* creature)
        : CreatureAI(creature), _patrolStarted(false), _aggroScanTimer(0) {}

    // Active aggro detection — overrides the default which has many silent skips
    void MoveInLineOfSight(Unit* who) override
    {
        if (!who || !me->IsAlive() || me->IsInCombat() || me->HasReactState(REACT_PASSIVE))
            return;

        if (who->GetTypeId() != TYPEID_PLAYER)
            return;

        Player* player = who->ToPlayer();
        if (!player || !player->IsAlive() || player->IsGameMaster())
            return;

        float aggroRange = sDMConfig->GetAggroRadius();
        if (me->IsWithinDistInMap(player, aggroRange) && me->IsHostileTo(player))
        {
            me->SetInCombatWith(player);
            player->SetInCombatWith(me);
            me->AddThreat(player, 1.0f);
            AttackStart(player);
        }
    }

    void UpdateAI(uint32 diff) override
    {
        if (!UpdateVictim())
        {
            // Start random patrol movement when idle
            if (!_patrolStarted && me->IsAlive())
            {
                me->GetMotionMaster()->MoveRandom(5.0f);
                _patrolStarted = true;
            }

            // Fallback aggro scan every 1 second for cases where MoveInLineOfSight
            // doesn't fire (inactive grids, summoned creature edge cases)
            _aggroScanTimer += diff;
            if (_aggroScanTimer >= 1000 && me->IsAlive())
            {
                _aggroScanTimer = 0;
                float aggroRange = sDMConfig->GetAggroRadius();

                Map::PlayerList const& players = me->GetMap()->GetPlayers();
                float closest = aggroRange;
                Player* target = nullptr;

                for (auto const& itr : players)
                {
                    Player* p = itr.GetSource();
                    if (!p || !p->IsAlive() || p->IsGameMaster())
                        continue;

                    float dist = me->GetDistance(p);
                    if (dist < closest && me->IsHostileTo(p))
                    {
                        closest = dist;
                        target = p;
                    }
                }

                if (target)
                {
                    me->SetInCombatWith(target);
                    target->SetInCombatWith(me);
                    me->AddThreat(target, 1.0f);
                    AttackStart(target);
                }
            }
            return;
        }
        DoMeleeAttackIfReady();
    }

    void EnterEvadeMode(EvadeReason /*why*/) override
    {
        _patrolStarted = false;
        CreatureAI::EnterEvadeMode();
    }

    void JustDied(Unit* killer) override
    {
        CreatureAI::JustDied(killer);
        sDungeonMasterMgr->OnCreatureDeathHook(me);
    }

private:
    bool   _patrolStarted;
    uint32 _aggroScanTimer;
};

// Session helper implementations (declared in DMTypes.h)
uint32 Session::GetAlivePlayerCount() const
{
    uint32 n = 0;
    for (const auto& pd : Players)
    {
        Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid);
        if (p && p->IsAlive()) ++n;
    }
    return n;
}

bool Session::IsPartyWiped()    const { return GetAlivePlayerCount() == 0; }

bool Session::IsGroupInCombat() const
{
    for (const auto& pd : Players)
    {
        Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid);
        if (p && p->IsAlive() && p->IsInCombat())
            return true;
    }
    return false;
}

// Singleton
DungeonMasterMgr::DungeonMasterMgr()  = default;
DungeonMasterMgr::~DungeonMasterMgr() = default;

DungeonMasterMgr* DungeonMasterMgr::Instance()
{
    static DungeonMasterMgr inst;
    return &inst;
}

// Initialization
void DungeonMasterMgr::Initialize()
{
    LOG_INFO("module", "DungeonMaster: Initializing...");
    LoadFromDB();
    LOG_INFO("module", "DungeonMaster: Ready — {} creature types, {} bosses, {} dungeon bosses, {} reward items, {} loot items.",
        _creaturesByType.size(), _bossCreatures.size(), _dungeonBossPool.size(), _rewardItems.size(), _lootPool.size());
}

void DungeonMasterMgr::LoadFromDB()
{
    LoadCreaturePools();
    LoadDungeonBossPool();
    LoadClassLevelStats();
    LoadRewardItems();
    LoadLootPool();
    LoadAllPlayerStats();
}

// Load creature pools from world DB, split into trash (rank 0) and boss (rank 1/2/4)
void DungeonMasterMgr::LoadCreaturePools()
{
    _creaturesByType.clear();
    _bossCreatures.clear();

    // Type for theming, rank for boss/trash split, ScriptName='' to avoid scripted mobs
    QueryResult result = WorldDatabase.Query(
        "SELECT ct.entry, ct.type, ct.minlevel, ct.maxlevel, ct.`rank` "
        "FROM creature_template ct "
        "LEFT JOIN creature_template_movement ctm ON ct.entry = ctm.CreatureId "
        "WHERE ct.type > 0 AND ct.type <= 10 AND ct.type != 8 "       // combat types, skip Critter
        "AND ct.minlevel > 0 AND ct.maxlevel <= 83 "
        "AND ct.`rank` != 3 "                                          // not World Boss
        "AND (ctm.Ground IS NULL OR ctm.Ground != 0) "                // no water-only creatures
        "AND ct.VehicleId = 0 "                                        // not a vehicle/chair/cannon
        "AND ct.ScriptName = '' "                                      // no C++ scripts (they override our scaling)
        "AND ct.npcflag = 0 "                                          // no vendors/quest givers/gossip NPCs
        "AND (ct.unit_flags & 2) = 0 "                                 // no NON_ATTACKABLE
        "AND (ct.subname = '' OR ct.subname IS NULL) "                 // no guild/title text under name
        "AND ct.name NOT LIKE '%[UNUSED]%' "
        "AND ct.name NOT LIKE '%[PH]%' "
        "AND ct.name NOT LIKE '%Test %' "
        "AND ct.name NOT LIKE '%Test_%' "
        "AND ct.name NOT LIKE '%DVREF%' "
        "AND ct.name NOT LIKE '%[DNT]%' "
        "AND ct.name NOT LIKE '%Trigger%' "
        "AND ct.name NOT LIKE '%Invisible%' "
        "AND ct.name NOT LIKE '%Dummy%' "
        "AND ct.name NOT LIKE '%(%' "                                  // skip (1), (2) variant entries
        "AND ct.name NOT LIKE '%Debug%' "
        "AND ct.name NOT LIKE '%Template%' "
        "AND ct.name NOT LIKE '%Copy of%' "
        "AND ct.name NOT LIKE '% - DNT' "
        "AND ct.name NOT LIKE '%Placeholder%' "
        "AND ct.name NOT LIKE '%Visual%' "
        "AND ct.name NOT LIKE '%Server%' "
        "AND ct.name NOT LIKE '%Quest%' "                              // quest scripted mobs
        "AND ct.name NOT LIKE '%zzOLD%' "
        "ORDER BY ct.type, ct.minlevel");

    if (!result)
    {
        LOG_ERROR("module", "DungeonMaster: creature_template query returned NO results — check your world DB!");
        return;
    }

    uint32 trashCount = 0, bossCount = 0;
    if (result)
    {
        do
        {
            Field* f = result->Fetch();
            CreaturePoolEntry e;
            e.Entry    = f[0].Get<uint32>();
            e.Type     = f[1].Get<uint32>();
            e.MinLevel = f[2].Get<uint8>();
            e.MaxLevel = f[3].Get<uint8>();
            uint8 rank = f[4].Get<uint8>();

            if (rank == 1 || rank == 2 || rank == 4)        // elite / rare-elite → boss pool
            {
                _bossCreatures[e.Type].push_back(e);
                ++bossCount;
            }
            else                                              // normal (rank 0) → trash pool
            {
                _creaturesByType[e.Type].push_back(e);
                ++trashCount;
            }
        } while (result->NextRow());
    }

    LOG_INFO("module", "DungeonMaster: Loaded {} trash creatures, {} potential bosses.",
        trashCount, bossCount);


    static const char* typeNames[] = {
        "None", "Beast", "Dragonkin", "Demon", "Elemental",
        "Giant", "Undead", "Humanoid", "Critter", "Mechanical", "NotSpecified"
    };
    for (const auto& [type, vec] : _creaturesByType)
    {
        const char* name = (type <= 10) ? typeNames[type] : "Unknown";
        LOG_INFO("module", "DungeonMaster:   Trash type {} ({}): {} entries",
            type, name, vec.size());
    }
    for (const auto& [type, vec] : _bossCreatures)
    {
        const char* name = (type <= 10) ? typeNames[type] : "Unknown";
        LOG_INFO("module", "DungeonMaster:   Boss  type {} ({}): {} entries",
            type, name, vec.size());
    }
}

// Load real dungeon bosses (scripted elites from all dungeon maps)
void DungeonMasterMgr::LoadDungeonBossPool()
{
    _dungeonBossPool.clear();

    // Build comma-separated list of all dungeon map IDs
    const auto& dungeons = sDMConfig->GetDungeons();
    if (dungeons.empty())
    {
        LOG_WARN("module", "DungeonMaster: No dungeons configured — dungeon boss pool empty.");
        return;
    }

    std::string mapList;
    for (size_t i = 0; i < dungeons.size(); ++i)
    {
        if (i > 0) mapList += ",";
        mapList += std::to_string(dungeons[i].MapId);
    }

    // Query for scripted elite creatures that spawn in dungeon maps.
    char query[2048];
    snprintf(query, sizeof(query),
        "SELECT DISTINCT ct.entry, ct.name, ct.type, ct.minlevel, ct.maxlevel "
        "FROM creature_template ct "
        "JOIN creature c ON c.id1 = ct.entry "
        "LEFT JOIN creature_template_movement ctm ON ct.entry = ctm.CreatureId "
        "WHERE c.map IN (%s) "
        "AND ct.`rank` IN (1, 2) "
        "AND ct.ScriptName != '' "
        "AND ct.type > 0 AND ct.type <= 10 "
        "AND ct.minlevel > 0 "
        "AND ct.VehicleId = 0 "
        "AND (ctm.Ground IS NULL OR ctm.Ground != 0) "  // no water-only creatures
        "AND (ct.unit_flags & 2) = 0 "       // not NON_ATTACKABLE
        "AND ct.name NOT LIKE '%%Trigger%%' "
        "AND ct.name NOT LIKE '%%Invisible%%' "
        "AND ct.name NOT LIKE '%%Dummy%%' "
        "AND ct.name NOT LIKE '%%Visual%%' "
        "AND ct.name NOT LIKE '%%Server%%' "
        "ORDER BY ct.type, ct.minlevel",
        mapList.c_str());

    QueryResult result = WorldDatabase.Query(query);

    if (!result)
    {
        LOG_WARN("module", "DungeonMaster: Dungeon boss pool query returned no results.");
        return;
    }

    uint32 count = 0;
    do
    {
        Field* f = result->Fetch();
        CreaturePoolEntry e;
        e.Entry    = f[0].Get<uint32>();
        // f[1] = name (for logging only)
        e.Type     = f[2].Get<uint32>();
        e.MinLevel = f[3].Get<uint8>();
        e.MaxLevel = f[4].Get<uint8>();

        _dungeonBossPool[e.Type].push_back(e);
        ++count;

        LOG_DEBUG("module", "DungeonMaster: Dungeon boss: {} (entry {}, type {}, level {}-{})",
            f[1].Get<std::string>(), e.Entry, e.Type, e.MinLevel, e.MaxLevel);
    } while (result->NextRow());

    LOG_INFO("module", "DungeonMaster: Loaded {} real dungeon bosses into boss pool.", count);

    static const char* typeNames[] = {
        "None", "Beast", "Dragonkin", "Demon", "Elemental",
        "Giant", "Undead", "Humanoid", "Critter", "Mechanical", "NotSpecified"
    };
    for (const auto& [type, vec] : _dungeonBossPool)
    {
        const char* name = (type <= 10) ? typeNames[type] : "Unknown";
        LOG_INFO("module", "DungeonMaster:   Dungeon boss type {} ({}): {} entries",
            type, name, vec.size());
    }
}

// Cache creature_classlevelstats for force-scaling
void DungeonMasterMgr::LoadClassLevelStats()
{
    _classLevelStats.clear();

    QueryResult result = WorldDatabase.Query(
        "SELECT level, class, basehp0, damage_base, basearmor, attackpower "
        "FROM creature_classlevelstats "
        "WHERE level > 0 AND level <= 83 "
        "ORDER BY class, level");

    if (!result)
    {
        LOG_WARN("module", "DungeonMaster: creature_classlevelstats not found — "
                 "creature scaling will use template defaults.");
        return;
    }

    uint32 count = 0;
    do
    {
        Field* f = result->Fetch();
        uint8  level     = f[0].Get<uint8>();
        uint8  unitClass = f[1].Get<uint8>();
        ClassLevelStatEntry e;
        e.BaseHP       = std::max(1u, f[2].Get<uint32>());
        e.BaseDamage   = std::max(1.0f, f[3].Get<float>());
        e.BaseArmor    = f[4].Get<uint32>();
        e.AttackPower  = f[5].Get<uint32>();
        _classLevelStats[{unitClass, level}] = e;
        ++count;
    } while (result->NextRow());

    LOG_INFO("module", "DungeonMaster: {} class-level stat entries cached.", count);
}

// Look up cached base stats
const ClassLevelStatEntry* DungeonMasterMgr::GetBaseStatsForLevel(
    uint8 unitClass, uint8 level) const
{
    auto it = _classLevelStats.find({unitClass, level});
    if (it != _classLevelStats.end())
        return &it->second;

    // Fallback: try Warrior (class 1) at this level
    it = _classLevelStats.find({1, level});
    if (it != _classLevelStats.end())
        return &it->second;

    return nullptr;
}

// Cache equippable reward items (green/blue/purple)
void DungeonMasterMgr::LoadRewardItems()
{
    _rewardItems.clear();

    QueryResult result = WorldDatabase.Query(
        "SELECT entry, RequiredLevel, Quality, InventoryType, class, subclass, "
        "AllowableClass, ItemLevel "
        "FROM item_template "
        "WHERE Quality >= 2 AND Quality <= 4 "
        "AND RequiredLevel > 0 AND RequiredLevel <= 80 "
        "AND InventoryType > 0 AND InventoryType <= 26 "
        "AND InventoryType NOT IN (18, 19, 24) "
        "AND class IN (2, 4) AND (Flags & 0x8) = 0 "
        "AND AllowableClass != 0 "
        "AND name NOT LIKE '%Test%' "
        "AND name NOT LIKE '%Deprecated%' "
        "AND name NOT LIKE '%[PH]%' "
        "AND name NOT LIKE '%OLD%' "
        "AND name NOT LIKE '%Monster -%' "
        "AND name NOT LIKE '%zzOLD%' "
        "ORDER BY RequiredLevel, Quality");

    if (result)
    {
        do
        {
            Field* f = result->Fetch();
            RewardItem ri;
            ri.Entry         = f[0].Get<uint32>();
            ri.MinLevel      = f[1].Get<uint8>();
            ri.MaxLevel      = ri.MinLevel + 5;
            ri.Quality       = f[2].Get<uint8>();
            ri.InventoryType = f[3].Get<uint32>();
            ri.Class         = f[4].Get<uint32>();
            ri.SubClass      = f[5].Get<uint32>();
            ri.AllowableClass = f[6].Get<int32>();
            ri.ItemLevel     = f[7].Get<uint16>();
            _rewardItems.push_back(ri);
        } while (result->NextRow());
    }

    LOG_INFO("module", "DungeonMaster: {} reward items cached.", _rewardItems.size());
}

// Cache items for mob loot drops
void DungeonMasterMgr::LoadLootPool()
{
    _lootPool.clear();

    // Grey junk, white consumables, green/blue/purple equipment
    QueryResult result = WorldDatabase.Query(
        "SELECT entry, RequiredLevel, Quality, class, subclass, AllowableClass, ItemLevel "
        "FROM item_template "
        "WHERE Quality <= 4 "
        "AND ItemLevel <= 300 "
        "AND SellPrice > 0 "
        "AND class IN (0, 2, 4, 7, 15) "
        "AND (Flags & 0x8) = 0 "
        "AND AllowableClass != 0 "
        "AND (RequiredLevel > 0 OR class NOT IN (2, 4)) "               // equipment must have a required level
        "AND name NOT LIKE '%Test%' "
        "AND name NOT LIKE '%Deprecated%' "
        "AND name NOT LIKE '%[PH]%' "
        "AND name NOT LIKE '%OLD%' "
        "AND name NOT LIKE '%Monster -%' "
        "AND name NOT LIKE '%zzOLD%' "
        "AND name NOT LIKE '%Debug%' "
        "ORDER BY RequiredLevel, Quality");

    if (result)
    {
        do
        {
            Field* f = result->Fetch();
            LootPoolItem li;
            li.Entry          = f[0].Get<uint32>();
            li.MinLevel       = f[1].Get<uint8>();
            li.Quality        = f[2].Get<uint8>();
            li.ItemClass      = f[3].Get<uint8>();
            li.SubClass       = f[4].Get<uint8>();
            li.AllowableClass = f[5].Get<int32>();
            li.ItemLevel      = f[6].Get<uint16>();
            _lootPool.push_back(li);
        } while (result->NextRow());
    }


    uint32 counts[5] = {};
    for (const auto& li : _lootPool)
        if (li.Quality <= 4) ++counts[li.Quality];

    LOG_INFO("module", "DungeonMaster: {} mob loot items cached "
        "(grey={}, white={}, green={}, blue={}, epic={}).",
        _lootPool.size(), counts[0], counts[1], counts[2], counts[3], counts[4]);
}

// Compute group average level
uint8 DungeonMasterMgr::ComputeEffectiveLevel(Player* leader) const
{
    if (!leader)
        return 1;

    Group* group = leader->GetGroup();
    if (!group)
        return leader->GetLevel();

    uint32 totalLevel = 0;
    uint32 count      = 0;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* m = ref->GetSource();
        if (m && m->IsInWorld())
        {
            totalLevel += m->GetLevel();
            ++count;
        }
    }

    return count > 0
        ? static_cast<uint8>(totalLevel / count)
        : leader->GetLevel();
}

// SESSION LIFECYCLE

Session* DungeonMasterMgr::CreateSession(Player* leader, uint32 difficultyId,
                                          uint32 themeId, uint32 mapId,
                                          bool scaleToParty)
{
    const DifficultyTier* diff  = sDMConfig->GetDifficulty(difficultyId);
    const Theme*          theme = sDMConfig->GetTheme(themeId);
    const DungeonInfo*    dg    = sDMConfig->GetDungeon(mapId);
    if (!diff || !theme || !dg)
        return nullptr;

    std::lock_guard<std::mutex> lock(_sessionMutex);

    // Check capacity under the lock to avoid race conditions
    if (!CanCreateNewSession())
        return nullptr;

    Session s;
    s.SessionId    = _nextSessionId++;
    s.LeaderGuid   = leader->GetGUID();
    s.State        = SessionState::Preparing;
    s.DifficultyId = difficultyId;
    s.ThemeId      = themeId;
    s.MapId        = mapId;
    s.ScaleToParty = scaleToParty;
    s.StartTime    = GameTime::GetGameTime().count();

    if (sDMConfig->IsTimeLimitEnabled())
        s.TimeLimit = sDMConfig->GetTimeLimitMinutes() * 60;


    if (scaleToParty)
    {
        // Scale to party: creatures match the player/group level,
        // clamped to the difficulty tier's range.
        s.EffectiveLevel = ComputeEffectiveLevel(leader);

        uint8 band = sDMConfig->GetLevelBand();
        s.LevelBandMin = (s.EffectiveLevel > band) ? (s.EffectiveLevel - band) : 1;
        s.LevelBandMax = std::min<uint8>(s.EffectiveLevel + band, 83);

        // Clamp to tier so the correct creature templates are selected
        s.LevelBandMin = std::max(s.LevelBandMin, diff->MinLevel);
        s.LevelBandMax = std::min(s.LevelBandMax, diff->MaxLevel);
    }
    else
    {
        // Use tier's natural level range — no party scaling.
        // EffectiveLevel = midpoint of the tier; band = full tier range.
        s.EffectiveLevel = static_cast<uint8>((uint16(diff->MinLevel) + uint16(diff->MaxLevel)) / 2);
        s.LevelBandMin   = diff->MinLevel;
        s.LevelBandMax   = diff->MaxLevel;
    }

    // Ensure min <= max after clamping (edge case: player level far outside tier)
    if (s.LevelBandMin > s.LevelBandMax)
        s.LevelBandMin = s.LevelBandMax;


    PlayerSessionData ld;
    ld.PlayerGuid  = leader->GetGUID();
    ld.ReturnMapId = leader->GetMapId();
    ld.ReturnPosition = { leader->GetPositionX(), leader->GetPositionY(),
                          leader->GetPositionZ(), leader->GetOrientation() };
    s.Players.push_back(ld);


    if (Group* g = leader->GetGroup())
    {
        for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
        {
            Player* m = ref->GetSource();
            if (m && m != leader && m->IsInWorld())
            {
                PlayerSessionData md;
                md.PlayerGuid  = m->GetGUID();
                md.ReturnMapId = m->GetMapId();
                md.ReturnPosition = { m->GetPositionX(), m->GetPositionY(),
                                      m->GetPositionZ(), m->GetOrientation() };
                s.Players.push_back(md);
            }
        }
    }

    _activeSessions[s.SessionId] = s;
    for (const auto& pd : s.Players)
        _playerToSession[pd.PlayerGuid] = s.SessionId;

    LOG_INFO("module", "DungeonMaster: Session {} — leader {}, party {}, diff {}, level band {}-{}, scale={}",
        s.SessionId, leader->GetName(), s.Players.size(),
        diff->Name, s.LevelBandMin, s.LevelBandMax, scaleToParty ? "party" : "tier");

    return &_activeSessions[s.SessionId];
}

Session* DungeonMasterMgr::GetSession(uint32 id)
{
    std::lock_guard<std::mutex> lock(_sessionMutex);
    auto it = _activeSessions.find(id);
    return it != _activeSessions.end() ? &it->second : nullptr;
}

Session* DungeonMasterMgr::GetSessionByInstance(uint32 instId)
{
    std::lock_guard<std::mutex> lock(_sessionMutex);
    auto it = _instanceToSession.find(instId);
    if (it != _instanceToSession.end())
    {
        auto sit = _activeSessions.find(it->second);
        return sit != _activeSessions.end() ? &sit->second : nullptr;
    }
    return nullptr;
}

Session* DungeonMasterMgr::GetSessionByPlayer(ObjectGuid guid)
{
    std::lock_guard<std::mutex> lock(_sessionMutex);
    auto it = _playerToSession.find(guid);
    if (it != _playerToSession.end())
    {
        auto sit = _activeSessions.find(it->second);
        return sit != _activeSessions.end() ? &sit->second : nullptr;
    }
    return nullptr;
}

// StartDungeon / TeleportPartyIn / TeleportPartyOut

bool DungeonMasterMgr::StartDungeon(Session* session)
{
    if (!session) return false;

    session->EntrancePos = GetDungeonEntrance(session->MapId);
    if (session->EntrancePos.GetPositionX() == 0 &&
        session->EntrancePos.GetPositionY() == 0 &&
        session->EntrancePos.GetPositionZ() == 0)
    {
        LOG_ERROR("module", "DungeonMaster: No entrance coords for map {}", session->MapId);
        return false;
    }
    return true;
}

bool DungeonMasterMgr::TeleportPartyIn(Session* session)
{
    if (!session) return false;
    const DungeonInfo* dg = sDMConfig->GetDungeon(session->MapId);
    if (!dg) return false;

    Position ent = session->EntrancePos;
    uint32 ok = 0;

    for (auto& pd : session->Players)
    {
        Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid);
        if (!p) continue;

        pd.ReturnMapId    = p->GetMapId();
        pd.ReturnPosition = { p->GetPositionX(), p->GetPositionY(),
                              p->GetPositionZ(), p->GetOrientation() };

        // Resurrect dead players so they can teleport (roguelike floor transitions, etc.)
        if (!p->IsAlive())
        {
            p->RemoveFlag(PLAYER_FIELD_BYTES, PLAYER_FIELD_BYTE_NO_RELEASE_WINDOW);
            p->ResurrectPlayer(1.0f);
            p->SpawnCorpseBones();
        }

        if (p->TeleportTo(session->MapId, ent.GetPositionX(), ent.GetPositionY(),
                          ent.GetPositionZ(), ent.GetOrientation()))
        {
            ++ok;
            LOG_INFO("module", "DungeonMaster: TeleportTo queued for {} → map {} ({:.1f}, {:.1f}, {:.1f})",
                p->GetName(), session->MapId, ent.GetPositionX(), ent.GetPositionY(), ent.GetPositionZ());
            char buf[256];
            snprintf(buf, sizeof(buf),
                "|cFF00FF00[Dungeon Master]|r Welcome to |cFFFFFFFF%s|r! "
                "Defeat the boss to claim your reward.",
                dg->Name.c_str());
            ChatHandler(p->GetSession()).SendSysMessage(buf);

            if (session->RoguelikeRunId != 0 && sRoguelikeMgr->HasActiveAffixes(session->RoguelikeRunId))
            {
                std::string affixNames = sRoguelikeMgr->GetActiveAffixNames(session->RoguelikeRunId);
                char affixBuf[512];
                snprintf(affixBuf, sizeof(affixBuf),
                    "|cFF00FFFF[Roguelike]|r Active affixes: %s", affixNames.c_str());
                ChatHandler(p->GetSession()).SendSysMessage(affixBuf);
            }
        }
        else
        {
            LOG_ERROR("module", "DungeonMaster: TeleportTo FAILED for {} → map {} ({:.1f}, {:.1f}, {:.1f})",
                p->GetName(), session->MapId, ent.GetPositionX(), ent.GetPositionY(), ent.GetPositionZ());
            ChatHandler(p->GetSession()).SendSysMessage(
                "|cFFFF0000[Dungeon Master]|r Teleport failed! You may lack access to this dungeon.");
        }
    }

    if (ok > 0)
    {
        session->State = SessionState::InProgress;
        // InstanceId is set when a player actually arrives on the map
        // (via the allmap script or the Update tick populate logic).
        return true;
    }
    return false;
}

void DungeonMasterMgr::TeleportPartyOut(Session* session)
{
    if (!session) return;
    for (const auto& pd : session->Players)
    {
        Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid);
        if (!p || !p->IsInWorld()) continue;
        p->RemoveFlag(PLAYER_FIELD_BYTES, PLAYER_FIELD_BYTE_NO_RELEASE_WINDOW);
        if (!p->IsAlive()) { p->ResurrectPlayer(1.0f); p->SpawnCorpseBones(); }
        p->TeleportTo(pd.ReturnMapId, pd.ReturnPosition.GetPositionX(),
                      pd.ReturnPosition.GetPositionY(), pd.ReturnPosition.GetPositionZ(),
                      pd.ReturnPosition.GetOrientation());
    }
}

// Dungeon entrance lookup from areatrigger_teleport
Position DungeonMasterMgr::GetDungeonEntrance(uint32 mapId)
{
    char q[256];
    snprintf(q, sizeof(q),
        "SELECT target_position_x, target_position_y, target_position_z, target_orientation "
        "FROM areatrigger_teleport WHERE target_map = %u LIMIT 1", mapId);
    if (QueryResult r = WorldDatabase.Query(q))
    {
        Field* f = r->Fetch();
        return { f[0].Get<float>(), f[1].Get<float>(), f[2].Get<float>(), f[3].Get<float>() };
    }
    LOG_WARN("module", "DungeonMaster: No areatrigger_teleport for map {}", mapId);
    return { 0, 0, 0, 0 };
}

// Spawn-point collection
std::vector<SpawnPoint> DungeonMasterMgr::GetSpawnPointsForMap(uint32 mapId)
{
    std::vector<SpawnPoint> pts;

    char q[256];
    snprintf(q, sizeof(q),
        "SELECT position_x, position_y, position_z, orientation "
        "FROM creature WHERE map = %u", mapId);
    QueryResult result = WorldDatabase.Query(q);
    if (!result) return pts;

    Position ent = GetDungeonEntrance(mapId);
    float ex = ent.GetPositionX(), ey = ent.GetPositionY(), ez = ent.GetPositionZ();

    do
    {
        Field* f = result->Fetch();
        float x = f[0].Get<float>(), y = f[1].Get<float>(),
              z = f[2].Get<float>(), o = f[3].Get<float>();

        SpawnPoint sp;
        sp.Pos.Relocate(x, y, z, o);
        float dx = x - ex, dy = y - ey, dz = z - ez;
        sp.DistanceFromEntrance = std::sqrt(dx*dx + dy*dy + dz*dz);
        pts.push_back(sp);
    } while (result->NextRow());

    // Sort near → far
    std::sort(pts.begin(), pts.end(),
        [](const SpawnPoint& a, const SpawnPoint& b)
        { return a.DistanceFromEntrance < b.DistanceFromEntrance; });

    // Find boss positions from creature data
    bool bossFound = false;

    char bq[512];
    snprintf(bq, sizeof(bq),
        "SELECT c.position_x, c.position_y, c.position_z, c.orientation, "
        "ct.mechanic_immune_mask, ct.`rank`, ct.name "
        "FROM creature c "
        "JOIN creature_template ct ON c.id1 = ct.entry "
        "WHERE c.map = %u "
        "AND ct.mechanic_immune_mask > 0 "
        "AND ct.`rank` >= 1 "
        "ORDER BY ct.mechanic_immune_mask DESC",
        mapId);
    QueryResult bossResult = WorldDatabase.Query(bq);

    if (bossResult)
    {
        // Collect all boss candidates, pick the farthest from entrance
        struct BossCandidate {
            float x, y, z, o;
            float dist;
            uint32 immuneMask;
            std::string name;
        };
        std::vector<BossCandidate> bosses;

        do
        {
            Field* f = bossResult->Fetch();
            BossCandidate bc;
            bc.x = f[0].Get<float>();
            bc.y = f[1].Get<float>();
            bc.z = f[2].Get<float>();
            bc.o = f[3].Get<float>();
            bc.immuneMask = f[4].Get<uint32>();
            bc.name = f[6].Get<std::string>();

            float dx = bc.x - ex, dy = bc.y - ey, dz = bc.z - ez;
            bc.dist = std::sqrt(dx*dx + dy*dy + dz*dz);
            bosses.push_back(bc);
        } while (bossResult->NextRow());

        if (!bosses.empty())
        {
            // The "last boss" is the farthest boss-type creature from the entrance.
            std::sort(bosses.begin(), bosses.end(),
                [](const BossCandidate& a, const BossCandidate& b)
                { return a.dist > b.dist; });

            const BossCandidate& lastBoss = bosses[0];

            LOG_INFO("module", "DungeonMaster: Map {} — found {} boss candidate(s). "
                "Last boss: '{}' at ({:.1f}, {:.1f}, {:.1f}), immuneMask={}, dist={:.1f}",
                mapId, bosses.size(), lastBoss.name,
                lastBoss.x, lastBoss.y, lastBoss.z,
                lastBoss.immuneMask, lastBoss.dist);

            // Create boss spawn point(s) at the actual boss location(s).
            uint32 bc = sDMConfig->GetBossCount();
            for (uint32 i = 0; i < bc && i < bosses.size(); ++i)
            {
                SpawnPoint bsp;
                bsp.Pos.Relocate(bosses[i].x, bosses[i].y, bosses[i].z, bosses[i].o);
                bsp.DistanceFromEntrance = bosses[i].dist;
                bsp.IsBossPosition = true;
                pts.push_back(bsp);
            }
            bossFound = true;
        }
    }

    // Fallback: if no actual boss found in DB, use farthest spawn point(s)
    if (!bossFound)
    {
        LOG_WARN("module", "DungeonMaster: Map {} — no boss creatures found in DB, "
            "falling back to farthest spawn points.", mapId);

        // Re-sort since we may have added boss points
        std::sort(pts.begin(), pts.end(),
            [](const SpawnPoint& a, const SpawnPoint& b)
            { return a.DistanceFromEntrance < b.DistanceFromEntrance; });

        uint32 bc = sDMConfig->GetBossCount();
        for (uint32 i = 0; i < bc && i < pts.size(); ++i)
            pts[pts.size() - 1 - i].IsBossPosition = true;
    }

    return pts;
}

// Instance population
void DungeonMasterMgr::ClearDungeonCreatures(InstanceMap* map)
{
    if (!map) return;

    uint32 npcEntry = sDMConfig->GetNpcEntry();
    uint32 totalRemoved = 0;

    // Phase 1: despawn our tracked creatures
    uint32 instanceId = map->GetInstanceId();
    auto guidIt = _instanceCreatureGuids.find(instanceId);
    if (guidIt != _instanceCreatureGuids.end())
    {
        for (const ObjectGuid& guid : guidIt->second)
        {
            Creature* c = map->GetCreature(guid);
            if (c && c->IsInWorld())
            {
                c->DespawnOrUnsummon();
                ++totalRemoved;
            }
        }
        guidIt->second.clear();
    }

    uint32 dbRemoved = 0;

    // Phase 2: despawn DB-spawned creatures
    auto const& store = map->GetCreatureBySpawnIdStore();
    for (auto const& pair : store)
    {
        Creature* c = pair.second;
        if (c && c->IsInWorld() && !c->IsPet() && !c->IsGuardian()
            && !c->IsTotem() && c->GetEntry() != npcEntry)
        {
            c->SetRespawnTime(7 * DAY);
            c->DespawnOrUnsummon();
            ++dbRemoved;
        }
    }

    // Phase 3: grid sweep for script-spawned creatures
    uint32 gridRemoved = 0;
    Map::PlayerList const& players = map->GetPlayers();
    for (auto const& itr : players)
    {
        Player* p = itr.GetSource();
        if (!p || !p->IsInWorld()) continue;

        std::list<Creature*> gridCreatures;
        p->GetCreatureListWithEntryInGrid(gridCreatures, 0, 5000.0f);

        for (Creature* c : gridCreatures)
        {
            if (!c || !c->IsInWorld()) continue;
            if (c->IsPet() || c->IsGuardian() || c->IsTotem()) continue;
            if (c->GetEntry() == npcEntry) continue;

            c->SetRespawnTime(7 * DAY);
            c->DespawnOrUnsummon();
            ++gridRemoved;
        }

        // Only need one player's grid sweep (the grid search radius
        // is large enough to cover the entire dungeon)
        break;
    }

    LOG_INFO("module", "DungeonMaster: Cleared {} tracked + {} DB + {} grid creatures from map {} (inst {})",
        totalRemoved, dbRemoved, gridRemoved, map->GetId(), instanceId);
}

void DungeonMasterMgr::OpenAllDoors(InstanceMap* map)
{
    if (!map) return;

    std::vector<GameObject*> doors;
    auto const& store = map->GetGameObjectBySpawnIdStore();
    for (auto const& pair : store)
    {
        GameObject* go = pair.second;
        if (!go || !go->IsInWorld()) continue;
        if (go->GetGoType() == GAMEOBJECT_TYPE_DOOR || go->GetGoType() == GAMEOBJECT_TYPE_BUTTON)
            doors.push_back(go);
    }

    for (GameObject* go : doors)
        if (go && go->IsInWorld())
            go->Delete();

    LOG_DEBUG("module", "DungeonMaster: Removed {} doors from instance.", doors.size());
}

// Populate dungeon with themed creatures and bosses
void DungeonMasterMgr::PopulateDungeon(Session* session, InstanceMap* map)
{
    if (!session || !map) return;

    LOG_INFO("module", "DungeonMaster: PopulateDungeon ENTRY — session {} map {} instId {} mobs {} bosses {}",
        session->SessionId, session->MapId, map->GetInstanceId(),
        session->TotalMobs, session->TotalBosses);

    const DifficultyTier* diff  = sDMConfig->GetDifficulty(session->DifficultyId);
    const Theme*          theme = sDMConfig->GetTheme(session->ThemeId);
    if (!diff || !theme) return;

    ClearDungeonCreatures(map);
    OpenAllDoors(map);

    // Mark all boss encounters as DONE so scripts don't interfere
    if (InstanceScript* script = map->GetInstanceScript())
    {
        uint32 bossesNeutralized = 0;
        // Loop through boss indices.  GetBossState returns TO_BE_DECIDED (5)
        // for indices past the end of the encounter list — use as sentinel.
        for (uint32 i = 0; i < 25; ++i)
        {
            EncounterState state = script->GetBossState(i);
            if (state == TO_BE_DECIDED)
                break;   // Past the last boss — stop

            if (state != DONE)
            {
                script->SetBossState(i, DONE);
                ++bossesNeutralized;
            }
        }
        if (bossesNeutralized > 0)
            LOG_INFO("module", "DungeonMaster: Neutralized {} InstanceScript boss encounter(s) in map {}",
                bossesNeutralized, map->GetId());
    }

    // Purge lingering debuffs from despawned creatures
    for (const auto& pd : session->Players)
    {
        Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid);
        if (!p || !p->IsInWorld()) continue;

        std::vector<uint32> toRemove;
        for (auto const& pair : p->GetAppliedAuras())
        {
            if (Aura* aura = pair.second->GetBase())
            {
                // Caster is gone (despawned) and it wasn't self-cast — lingering debuff
                if (!aura->GetCaster() && aura->GetCasterGUID() != p->GetGUID())
                    toRemove.push_back(aura->GetId());
            }
        }
        for (uint32 spellId : toRemove)
            p->RemoveAura(spellId);

        if (!toRemove.empty())
            LOG_DEBUG("module", "DungeonMaster: Purged {} lingering debuff(s) from {}",
                toRemove.size(), p->GetName());
    }

    session->SpawnPoints = GetSpawnPointsForMap(session->MapId);
    if (session->SpawnPoints.empty())
    {
        LOG_ERROR("module", "DungeonMaster: No spawn points for map {}", session->MapId);
        return;
    }

    float hpMult  = CalculateHealthMultiplier(session);
    float dmgMult = CalculateDamageMultiplier(session);

    uint8 bandMin = session->LevelBandMin;
    uint8 bandMax = session->LevelBandMax;
    uint8 targetLevel = session->EffectiveLevel;


    uint32 instanceId = map->GetInstanceId();
    auto& guidList = _instanceCreatureGuids[instanceId];
    guidList.clear();

    LOG_INFO("module", "DungeonMaster: Populating session {} — theme '{}', band {}-{}, target lvl {}, HP x{:.2f}, DMG x{:.2f}",
        session->SessionId, theme->Name, bandMin, bandMax, targetLevel, hpMult, dmgMult);

    // Force-scale creature to target level
    // Compute a boss-specific damage multiplier that only includes party scaling,
    // NOT the difficulty tier's DamageMultiplier (to avoid double-stacking).
    float bossOnlyDmgMult;
    {
        uint32 n = session->Players.size();
        if (n <= 1) bossOnlyDmgMult = sDMConfig->GetSoloMultiplier();
        else        bossOnlyDmgMult = 1.0f + (n - 1) * sDMConfig->GetPerPlayerDamageMult();
        if (session->RoguelikeRunId != 0)
            bossOnlyDmgMult *= sRoguelikeMgr->GetTierDamageMultiplier(session->RoguelikeRunId);
    }

    auto applyLevelAndStats = [&](Creature* c, float extraHpMult, float extraDmgMult, bool isBoss)
    {
    
        c->SetLevel(targetLevel);

    
        if (isBoss)
        {
            c->SetByteValue(UNIT_FIELD_BYTES_0, 2, 1);  // Elite rank → gold dragon frame
            c->SetObjectScale(1.3f);                      // 30% larger than normal
        }

        uint8 unitClass = c->GetCreatureTemplate()->unit_class;
        const ClassLevelStatEntry* baseStats = GetBaseStatsForLevel(unitClass, targetLevel);

    
        float finalHP;
        if (baseStats)
            finalHP = static_cast<float>(baseStats->BaseHP) * hpMult * extraHpMult;
        else
            finalHP = c->GetMaxHealth() * hpMult * extraHpMult;

        uint32 hp = std::max(1u, static_cast<uint32>(finalHP));
        c->SetMaxHealth(hp);
        c->SetHealth(hp);

        // For bosses, use party-only scaling (bossOnlyDmgMult) instead of the full
        // tier+party dmgMult to prevent double-stacking tier DamageMultiplier with BossDamageMult
        float effectiveDmgMult = isBoss ? bossOnlyDmgMult : dmgMult;

        if (baseStats)
        {
            float dmgBase  = baseStats->BaseDamage;
            float apBonus  = static_cast<float>(baseStats->AttackPower) / 14.0f;
            float atkTime  = static_cast<float>(c->GetCreatureTemplate()->BaseAttackTime) / 1000.0f;
            if (atkTime <= 0.0f) atkTime = 2.0f;

            float minDmg = (dmgBase + apBonus) * atkTime * effectiveDmgMult * extraDmgMult;
            float maxDmg = ((dmgBase * 1.15f) + apBonus) * atkTime * effectiveDmgMult * extraDmgMult;

            minDmg = std::max(1.0f, minDmg);
            maxDmg = std::max(minDmg, maxDmg);

            c->SetBaseWeaponDamage(BASE_ATTACK, MINDAMAGE, minDmg);
            c->SetBaseWeaponDamage(BASE_ATTACK, MAXDAMAGE, maxDmg);
            c->UpdateDamagePhysical(BASE_ATTACK);
        }

        // --- Armor (from classlevelstats for the TARGET level) ---
        if (baseStats && baseStats->BaseArmor > 0)
            c->SetArmor(baseStats->BaseArmor);

        // --- Roguelike: additional armor scaling from tier progression ---
        if (session->RoguelikeRunId != 0)
        {
            float armorMult = sRoguelikeMgr->GetTierArmorMultiplier(session->RoguelikeRunId);
            if (armorMult > 1.0f)
                c->SetArmor(static_cast<uint32>(c->GetArmor() * armorMult));
        }

        // --- Clear ALL spell resistances (original template values are for original level) ---
        for (uint8 school = SPELL_SCHOOL_HOLY; school < MAX_SPELL_SCHOOL; ++school)
            c->SetResistance(SpellSchools(school), 0);

        // --- Clear mechanic immunities ---
        for (uint32 mech = 1; mech < MAX_MECHANIC; ++mech)
            c->ApplySpellImmune(0, IMMUNITY_MECHANIC, mech, false);

        // --- Clear spell immunities that might come from the template ---
        c->ApplySpellImmune(0, IMMUNITY_SCHOOL, SPELL_SCHOOL_MASK_ALL, false);

        // --- Movement ---
        if (isBoss)
        {
            // Bosses idle at spawn — they'll engage when players approach
            c->SetWanderDistance(0.0f);
            c->SetDefaultMovementType(IDLE_MOTION_TYPE);
            c->GetMotionMaster()->MoveIdle();
        }
        else
        {
            // Trash mobs patrol a 5 yd radius around their spawn point
            c->SetWanderDistance(5.0f);
            c->SetDefaultMovementType(RANDOM_MOTION_TYPE);
            c->GetMotionMaster()->MoveRandom(5.0f);
        }

        // --- Strip native abilities from bosses ---
        // Boss templates come from all dungeon tiers.  Their scripted spells have
        // hard-coded damage values designed for their original level range, which
        // are NOT affected by our stat scaling.  A level-80 boss spell would
        // one-shot a level-25 party.  We replace the boss AI with our custom
        // melee-only AI below, and strip any pre-applied auras here so passive
        // procs and combat triggers don't fire unscaled damage.  Bosses still
        // keep their model, name, and gold-dragon boss frame.
        if (isBoss)
        {
            c->RemoveAllAuras();
            // Restore health after aura strip (some auras modify max HP)
            c->SetHealth(c->GetMaxHealth());
        }

        // --- Install custom AI for all spawned creatures ---
        // Both trash and bosses use DungeonMasterCreatureAI for reliable
        // aggro detection, properly-scaled melee damage, and JustDied hook.
        c->SetAI(new DungeonMasterCreatureAI(c));

        // Force visibility refresh or client won't see the creature
        c->UpdateObjectVisibility(true);

        // Track this GUID for future cleanup
        guidList.push_back(c->GetGUID());
    };

    // Spawn trash mobs
    uint32 spawnedMobs = 0;
    for (auto& sp : session->SpawnPoints)
    {
        if (sp.IsBossPosition) continue;

        uint32 entry = SelectCreatureForTheme(theme, false);
        if (!entry) continue;

        Creature* c = map->SummonCreature(entry, sp.Pos);
        if (!c) continue;

        c->SetFaction(14);               // hostile to all
        c->SetReactState(REACT_AGGRESSIVE);
        c->SetObjectScale(1.0f);
        c->SetCorpseDelay(300);          // 5 min corpse before despawn
        c->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_IMMUNE_TO_PC
                                        | UNIT_FLAG_IMMUNE_TO_NPC | UNIT_FLAG_PACIFIED
                                        | UNIT_FLAG_STUNNED | UNIT_FLAG_FLEEING
                                        | UNIT_FLAG_NOT_SELECTABLE);
        c->SetUInt32Value(UNIT_FIELD_FLAGS_2, 0);
        c->SetImmuneToPC(false);
        c->SetImmuneToNPC(false);
        c->setActive(true);             // Keep creature in grid update cycle for aggro detection

        bool isElite = (RandInt<uint32>(1, 100) <= sDMConfig->GetEliteChance());

        // Roguelike affix multipliers for trash
        float affixHpMult = 1.0f, affixDmgMult = 1.0f, affixEliteMult = 1.0f;
        if (session->RoguelikeRunId != 0)
        {
            sRoguelikeMgr->GetAffixMultipliers(session->RoguelikeRunId,
                false, isElite, affixHpMult, affixDmgMult, affixEliteMult);
            // Savage affix: boosted elite chance
            if (affixEliteMult > 1.0f && !isElite)
            {
                uint32 boostedChance = static_cast<uint32>(sDMConfig->GetEliteChance() * affixEliteMult);
                isElite = (RandInt<uint32>(1, 100) <= boostedChance);
            }
        }

        float eliteHpMult  = isElite ? sDMConfig->GetEliteHealthMult() : 1.0f;
        float eliteDmgMult = isElite ? 1.5f : 1.0f;

        applyLevelAndStats(c, eliteHpMult * affixHpMult, eliteDmgMult * affixDmgMult, false);

        // Red glow on affix-affected creatures so players can see they're empowered
        if (affixHpMult > 1.0f || affixDmgMult > 1.0f)
            if (Aura* a = c->AddAura(8599, c))
                a->SetDuration(-1);

        SpawnedCreature sc;
        sc.Guid = c->GetGUID(); sc.Entry = entry;
        sc.IsElite = isElite; sc.IsBoss = false;
        session->SpawnedCreatures.push_back(sc);
        ++spawnedMobs;
    }
    session->TotalMobs = spawnedMobs;

    // Spawn bosses (real dungeon bosses)
    uint32 bossesSpawned = 0;
    for (auto& sp : session->SpawnPoints)
    {
        if (!sp.IsBossPosition || bossesSpawned >= sDMConfig->GetBossCount())
            continue;

        uint32 entry = SelectDungeonBoss(theme);
        if (!entry) { LOG_WARN("module", "DungeonMaster: No boss candidate."); continue; }

        Creature* b = map->SummonCreature(entry, sp.Pos);
        if (!b) continue;

        b->SetFaction(14);
        b->SetReactState(REACT_AGGRESSIVE);
        b->SetCorpseDelay(600);          // 10 min corpse for bosses
        b->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_IMMUNE_TO_PC
                                        | UNIT_FLAG_IMMUNE_TO_NPC | UNIT_FLAG_PACIFIED
                                        | UNIT_FLAG_STUNNED | UNIT_FLAG_FLEEING
                                        | UNIT_FLAG_NOT_SELECTABLE);
        b->SetUInt32Value(UNIT_FIELD_FLAGS_2, 0);
        b->SetImmuneToPC(false);
        b->SetImmuneToNPC(false);
        b->setActive(true);             // Keep creature in grid update cycle for aggro detection

        // Roguelike affix multipliers for bosses
        float bossAffixHpMult = 1.0f, bossAffixDmgMult = 1.0f, _unused = 1.0f;
        if (session->RoguelikeRunId != 0)
            sRoguelikeMgr->GetAffixMultipliers(session->RoguelikeRunId,
                true, true, bossAffixHpMult, bossAffixDmgMult, _unused);

        applyLevelAndStats(b,
            sDMConfig->GetBossHealthMult() * bossAffixHpMult,
            sDMConfig->GetBossDamageMult() * bossAffixDmgMult, true);

        if (bossAffixHpMult > 1.0f || bossAffixDmgMult > 1.0f)
            if (Aura* a = b->AddAura(8599, b))
                a->SetDuration(-1);

        SpawnedCreature sc;
        sc.Guid = b->GetGUID(); sc.Entry = entry;
        sc.IsElite = true; sc.IsBoss = true;
        session->SpawnedCreatures.push_back(sc);
        ++bossesSpawned;
    }
    session->TotalBosses = bossesSpawned;

    LOG_INFO("module", "DungeonMaster: Session {} — {} mobs, {} bosses spawned.",
        session->SessionId, session->TotalMobs, session->TotalBosses);
}

// Select a creature matching the theme
uint32 DungeonMasterMgr::SelectCreatureForTheme(const Theme* theme, bool isBoss)
{
    if (!theme) return 0;


    std::set<uint32> types;
    bool anyType = false;
    for (uint32 t : theme->CreatureTypes)
    {
        if (t == uint32(-1)) anyType = true;
        else types.insert(t);
    }

    auto typeMatch = [&](uint32 cType) -> bool
    {
        return anyType || types.count(cType);
    };

    std::vector<uint32> candidates;

    if (isBoss)
    {
        // --- Try themed elites first ---
        for (const auto& [type, vec] : _bossCreatures)
        {
            if (!typeMatch(type)) continue;
            for (const auto& e : vec)
                candidates.push_back(e.Entry);
        }

        // --- Fallback: promote themed trash to boss (stats will be scaled up) ---
        if (candidates.empty())
        {
            for (const auto& [type, vec] : _creaturesByType)
            {
                if (!typeMatch(type)) continue;
                for (const auto& e : vec)
                    candidates.push_back(e.Entry);
            }
        }
    }
    else
    {
        // --- Themed trash ---
        for (const auto& [type, vec] : _creaturesByType)
        {
            if (!typeMatch(type)) continue;
            for (const auto& e : vec)
                candidates.push_back(e.Entry);
        }
    }

    // Fallback: any type
    if (candidates.empty() && !anyType)
    {
        LOG_WARN("module", "DungeonMaster: No '{}' creatures found — falling back to any type.",
            theme->Name);

        if (isBoss)
        {
            for (const auto& [type, vec] : _bossCreatures)
                for (const auto& e : vec)
                    candidates.push_back(e.Entry);
        }

        if (candidates.empty())
        {
            for (const auto& [type, vec] : _creaturesByType)
                for (const auto& e : vec)
                    candidates.push_back(e.Entry);
        }
    }

    if (!candidates.empty())
    {
        LOG_DEBUG("module", "DungeonMaster: {} candidates for theme '{}' (boss={})",
            candidates.size(), theme->Name, isBoss);
        return candidates[RandInt<size_t>(0, candidates.size() - 1)];
    }

    LOG_ERROR("module", "DungeonMaster: ZERO candidates for theme '{}' (boss={})",
        theme->Name, isBoss);
    return 0;
}


uint32 DungeonMasterMgr::SelectDungeonBoss(const Theme* theme)
{
    if (!theme) return 0;


    std::set<uint32> types;
    bool anyType = false;
    for (uint32 t : theme->CreatureTypes)
    {
        if (t == uint32(-1)) anyType = true;
        else types.insert(t);
    }

    auto typeMatch = [&](uint32 cType) -> bool
    {
        return anyType || types.count(cType);
    };

    // Prefer themed dungeon bosses
    std::vector<uint32> candidates;
    for (const auto& [type, vec] : _dungeonBossPool)
    {
        if (!typeMatch(type)) continue;
        for (const auto& e : vec)
            candidates.push_back(e.Entry);
    }

    // Fallback: any dungeon boss
    if (candidates.empty())
    {
        LOG_DEBUG("module", "DungeonMaster: No themed dungeon boss for '{}' — using any dungeon boss.",
            theme->Name);
        for (const auto& [type, vec] : _dungeonBossPool)
            for (const auto& e : vec)
                candidates.push_back(e.Entry);
    }

    // Last resort: generic boss pool
    if (candidates.empty())
    {
        LOG_WARN("module", "DungeonMaster: Dungeon boss pool empty — falling back to generic boss selection.");
        return SelectCreatureForTheme(theme, true);
    }

    uint32 entry = candidates[RandInt<size_t>(0, candidates.size() - 1)];
    LOG_DEBUG("module", "DungeonMaster: Selected dungeon boss entry {} from {} candidates (theme '{}')",
        entry, candidates.size(), theme->Name);
    return entry;
}

// Death handling
void DungeonMasterMgr::HandleCreatureDeath(Creature* creature, Session* session)
{
    if (!creature || !session || !session->IsActive())
        return;

    LOG_INFO("module", "DungeonMaster: HandleCreatureDeath called for {} (GUID: {}) in session {}",
        creature->GetName(), creature->GetGUID().GetCounter(), session->SessionId);

    for (auto& sc : session->SpawnedCreatures)
    {
        if (sc.Guid == creature->GetGUID())
        {
            if (sc.IsDead)
            {
                LOG_WARN("module", "DungeonMaster: Creature {} already marked as dead (race condition guard)",
                    creature->GetGUID().GetCounter());
                return;
            }

            sc.IsDead = true;
            LOG_INFO("module", "DungeonMaster: Processing death for {} (Boss: {}, Elite: {})",
                creature->GetName(), sc.IsBoss, sc.IsElite);

            FillCreatureLoot(creature, session, sc.IsBoss);
            GiveKillXP(session, sc.IsBoss, sc.IsElite);

            if (sc.IsBoss)
            {
                // Multi-phase support: defer boss kill count to allow phase transitions
                PendingPhaseCheck ppc;
                ppc.DeathPos   = { creature->GetPositionX(), creature->GetPositionY(),
                                   creature->GetPositionZ(), creature->GetOrientation() };
                ppc.DeathTime  = GameTime::GetGameTime().count();
                ppc.OrigEntry  = creature->GetEntry();
                ppc.Resolved   = false;
                session->PendingPhaseChecks.push_back(ppc);

                LOG_INFO("module", "DungeonMaster: Boss '{}' died — deferring kill count for phase check",
                    creature->GetName());
            }
            else
            {
                ++session->MobsKilled;
                for (auto& pd : session->Players)
                    ++pd.MobsKilled;
            }
            break;
        }
    }

    // Completion is now handled by the phase check system in Update()
}

void DungeonMasterMgr::HandleBossDeath(Session* session)
{
    if (!session) return;
    for (const auto& pd : session->Players)
        if (Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid))
        {
            if (session->BossesKilled < session->TotalBosses && p->GetSession())
            {
                char buf[128];
                snprintf(buf, sizeof(buf),
                    "|cFFFFFF00[Dungeon Master]|r Boss defeated! |cFFFFFFFF%u|r remaining.",
                    session->TotalBosses - session->BossesKilled);
                ChatHandler(p->GetSession()).SendSysMessage(buf);
            }
        }
}

    // Called from JustDied hook — fills loot before corpse is opened
void DungeonMasterMgr::OnCreatureDeathHook(Creature* creature)
{
    if (!creature) return;

    LOG_INFO("module", "DungeonMaster: OnCreatureDeathHook called for {} (GUID: {})",
        creature->GetName(), creature->GetGUID().GetCounter());

    std::lock_guard<std::mutex> lock(_sessionMutex);

    for (auto& [sid, session] : _activeSessions)
    {
        if (!session.IsActive())
            continue;
        if (creature->GetMapId() != session.MapId)
            continue;

        for (auto& sc : session.SpawnedCreatures)
        {
            if (sc.Guid == creature->GetGUID())
            {
                if (sc.IsDead)
                {
                    LOG_WARN("module", "DungeonMaster: OnCreatureDeathHook - creature {} already marked as dead",
                        creature->GetGUID().GetCounter());
                    return;
                }

                sc.IsDead = true;
                LOG_INFO("module", "DungeonMaster: OnCreatureDeathHook processing death for {} (Boss: {}, Elite: {})",
                    creature->GetName(), sc.IsBoss, sc.IsElite);

                // Fill loot immediately at death time
                FillCreatureLoot(creature, &session, sc.IsBoss);
                GiveKillXP(&session, sc.IsBoss, sc.IsElite);

                if (sc.IsBoss)
                {
                    // Multi-phase support: defer the boss kill count for a few seconds
                    // to check if a phase-2 creature spawns near the death location.
                    PendingPhaseCheck ppc;
                    ppc.DeathPos   = { creature->GetPositionX(), creature->GetPositionY(),
                                       creature->GetPositionZ(), creature->GetOrientation() };
                    ppc.DeathTime  = GameTime::GetGameTime().count();
                    ppc.OrigEntry  = creature->GetEntry();
                    ppc.Resolved   = false;
                    session.PendingPhaseChecks.push_back(ppc);

                    LOG_INFO("module", "DungeonMaster: Boss '{}' died — deferring kill count for phase check (entry {})",
                        creature->GetName(), creature->GetEntry());
                }
                else
                {
                    ++session.MobsKilled;
                }

                // Credit all party members (boss credits are deferred for phase check)
                for (auto& pd : session.Players)
                {
                    if (!sc.IsBoss) ++pd.MobsKilled;
                    // Boss kill credits are applied when the phase check resolves
                }

                LOG_DEBUG("module", "DungeonMaster: Creature {} (entry {}) death handled via hook "
                    "(session {}, boss={}).",
                    creature->GetGUID().ToString(), creature->GetEntry(),
                    sid, sc.IsBoss);
                return;
            }
        }
    }
}

void DungeonMasterMgr::HandlePlayerDeath(Player* player, Session* session)
{
    if (!player || !session) return;

    if (PlayerSessionData* pd = session->GetPlayerData(player->GetGUID()))
        ++pd->Deaths;

    // Block release-spirit; auto-rez instead
    player->SetFlag(PLAYER_FIELD_BYTES, PLAYER_FIELD_BYTE_NO_RELEASE_WINDOW);
    player->RemoveFlag(PLAYER_FIELD_BYTES, PLAYER_FIELD_BYTE_RELEASE_TIMER);

    if (session->IsPartyWiped())
    {
        ++session->Wipes;

        // --- Roguelike: delegate wipe handling to RoguelikeMgr ---
        if (session->RoguelikeRunId != 0)
        {
            session->State   = SessionState::Failed;
            session->EndTime = GameTime::GetGameTime().count();
            sRoguelikeMgr->OnPartyWipe(session->RoguelikeRunId);
            return;
        }

        session->State   = SessionState::Failed;
        session->EndTime = GameTime::GetGameTime().count();

        for (const auto& psd : session->Players)
        {
            Player* p = ObjectAccessor::FindPlayer(psd.PlayerGuid);
            if (!p) continue;
            p->RemoveFlag(PLAYER_FIELD_BYTES, PLAYER_FIELD_BYTE_NO_RELEASE_WINDOW);
            if (!p->IsAlive()) { p->ResurrectPlayer(1.0f); p->SpawnCorpseBones(); }
            if (p->GetSession())
                ChatHandler(p->GetSession()).SendSysMessage(
                    "|cFFFF0000[Dungeon Master]|r Total party wipe! Challenge failed.");
            p->TeleportTo(psd.ReturnMapId, psd.ReturnPosition.GetPositionX(),
                psd.ReturnPosition.GetPositionY(), psd.ReturnPosition.GetPositionZ(),
                psd.ReturnPosition.GetOrientation());
        }
    }
    else
    {
        if (player->GetSession())
            ChatHandler(player->GetSession()).SendSysMessage(
                "|cFFFFFF00[Dungeon Master]|r You have fallen! "
                "You will be revived when your group leaves combat.");
    }
}

// Rewards
void DungeonMasterMgr::DistributeRewards(Session* session)
{
    if (!session) return;
    const DifficultyTier* diff = sDMConfig->GetDifficulty(session->DifficultyId);
    if (!diff) return;


    uint32 lvl       = session->EffectiveLevel;
    uint32 baseGold  = lvl * 500;
    uint32 mobGold   = session->MobsKilled  * (lvl * 10);
    uint32 bossGold  = session->BossesKilled * (lvl * 500);
    uint32 total     = static_cast<uint32>((baseGold + mobGold + bossGold) * diff->RewardMultiplier);
    uint32 perPlayer = total / std::max<uint32>(1, session->Players.size());

    uint8 rewardLevel = static_cast<uint8>(std::min<uint32>(lvl, 80));

    LOG_INFO("module", "DungeonMaster: DistributeRewards — EffectiveLevel={}, rewardLevel={}, "
        "rewardPool={} items, players={}",
        lvl, rewardLevel, _rewardItems.size(), session->Players.size());

    for (const auto& pd : session->Players)
    {
        Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid);
        if (!p || !p->IsInWorld())
        {
            LOG_WARN("module", "DungeonMaster: Player {} not found/not in world for rewards", pd.PlayerGuid.GetCounter());
            continue;
        }

        // Gold goes directly to wallet
        GiveGoldReward(p, perPlayer);

        // Completion item: roll epic first, then rare, fallback green
        // Item goes directly to inventory (mail fallback if bags full)
        uint8 quality = 2;  // green baseline
        if (RandInt<uint32>(1, 100) <= sDMConfig->GetEpicChance())
            quality = 4;
        else if (RandInt<uint32>(1, 100) <= sDMConfig->GetRareChance())
            quality = 3;

        GiveItemReward(p, rewardLevel, quality);
    }
}


void DungeonMasterMgr::GiveKillXP(Session* session, bool isBoss, bool isElite)
{
    if (!session) return;

    for (const auto& pd : session->Players)
    {
        Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid);
        if (!p || !p->IsAlive()) continue;
        if (p->GetLevel() >= sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL)) continue;

        uint32 baseXP = (p->GetLevel() * 5) + 45;

        float mult = 1.0f;
        if (isBoss)       mult = 10.0f;
        else if (isElite) mult = 2.0f;

        uint32 xp = static_cast<uint32>(baseXP * mult);
        p->GiveXP(xp, nullptr);
    }
}

void DungeonMasterMgr::GiveGoldReward(Player* player, uint32 amount)
{
    if (!player || !amount) return;
    player->ModifyMoney(amount);

    if (player->GetSession())
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
            "|cFFFFD700[Dungeon Master]|r You received: |cFFFFD700%u|rg |cFFC0C0C0%u|rs |cFFB87333%u|rc",
            amount / 10000, (amount % 10000) / 100, amount % 100);
        ChatHandler(player->GetSession()).SendSysMessage(buf);
    }
}

void DungeonMasterMgr::GiveItemReward(Player* player, uint8 level, uint8 quality)
{
    uint32 playerClass = player->getClass();
    uint32 itemEntry = SelectRewardItem(level, quality, playerClass);

    // Quality fallback: if requested quality isn't found, try lower qualities
    // but still maintain level appropriateness
    if (!itemEntry && quality > 2)
    {
        LOG_WARN("module", "DungeonMaster: No quality {} items for level {}, class {}. Trying lower quality...",
            quality, level, playerClass);
        for (uint8 q = quality - 1; q >= 2 && !itemEntry; --q)
            itemEntry = SelectRewardItem(level, q, playerClass);
    }

    if (!itemEntry)
    {
        LOG_ERROR("module", "DungeonMaster: No suitable reward item for player {} (level {}, class {}, quality {}). "
            "Reward pool has {} items total. Gold only.",
            player->GetName(), level, playerClass, quality, _rewardItems.size());
        if (player->GetSession())
            ChatHandler(player->GetSession()).SendSysMessage(
                "|cFFFF0000[Dungeon Master]|r No suitable gear found for your level and class. Gold only.");
        return;
    }

    LOG_INFO("module", "DungeonMaster: Giving item {} to {} (level {}, quality {}, class {})",
        itemEntry, player->GetName(), level, quality, playerClass);

    ItemPosCountVec dest;
    if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemEntry, 1) == EQUIP_ERR_OK)
    {
        if (Item* item = player->StoreNewItem(dest, itemEntry, true))
        {
            player->SendNewItem(item, 1, true, false);
            if (const ItemTemplate* t = sObjectMgr->GetItemTemplate(itemEntry))
            {
                if (player->GetSession())
                {
                    char buf[256];
                    snprintf(buf, sizeof(buf),
                        "|cFFFFD700[Dungeon Master]|r You received: |cFFFFFFFF%s|r", t->Name1.c_str());
                    ChatHandler(player->GetSession()).SendSysMessage(buf);
                }
            }
        }
    }
    else
    {
        // Bags full — mail the item instead.
        Item* mailItem = Item::CreateItem(itemEntry, 1, player);
        if (mailItem)
        {
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            MailDraft("Dungeon Master Reward", "Your bags were full. Here is your reward!")
                .AddItem(mailItem)
                .SendMailTo(trans,
                    MailReceiver(player, player->GetGUID().GetCounter()),
                    MailSender(MAIL_NORMAL, 0, MAIL_STATIONERY_GM));
            CharacterDatabase.CommitTransaction(trans);
            if (player->GetSession())
                ChatHandler(player->GetSession()).SendSysMessage(
                    "|cFFFFD700[Dungeon Master]|r Bags full! Reward mailed to you.");
        }
        else
        {
            LOG_ERROR("module", "DungeonMaster: Failed to create mail item {} for {}", itemEntry, player->GetName());
        }
    }
}

    // Mail a reward item to player
void DungeonMasterMgr::MailItemReward(Player* player, uint8 level, uint8 quality,
                                       const std::string& subject, const std::string& body)
{
    if (!player || !player->IsInWorld()) return;

    uint32 playerClass = player->getClass();
    uint32 itemEntry = SelectRewardItem(level, quality, playerClass);

    // Quality fallback
    if (!itemEntry && quality > 2)
    {
        for (uint8 q = quality - 1; q >= 2 && !itemEntry; --q)
            itemEntry = SelectRewardItem(level, q, playerClass);
    }

    // Level window fallback
    if (!itemEntry)
    {
        uint8 windows[] = { 15, 25, 80 };
        for (uint8 w : windows)
        {
            uint8 lo = (level > w) ? level - w : 1;
            uint8 hi = std::min<uint8>(level + w, 80);
            for (const auto& ri : _rewardItems)
            {
                if (ri.Quality < 2 || ri.Quality > 4) continue;
                if (ri.MinLevel < lo || ri.MinLevel > hi) continue;
                if (ri.AllowableClass != -1 && !(ri.AllowableClass & (1 << (playerClass - 1))))
                    continue;
                itemEntry = ri.Entry;
                break;
            }
            if (itemEntry) break;
        }
    }

    if (!itemEntry)
    {
        LOG_ERROR("module", "DungeonMaster: No reward item found for mail to {} (level {}, class {})",
            player->GetName(), level, playerClass);
        return;
    }

    Item* mailItem = Item::CreateItem(itemEntry, 1, player);
    if (mailItem)
    {
        // Must use a real transaction (nullptr crashes SendMailTo)
        // inside SendMailTo when it calls trans->Append() on the null ptr.
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

        MailDraft(subject, body)
            .AddItem(mailItem)
            .SendMailTo(trans,
                MailReceiver(player, player->GetGUID().GetCounter()),
                MailSender(MAIL_NORMAL, 0, MAIL_STATIONERY_GM));

        CharacterDatabase.CommitTransaction(trans);

        if (const ItemTemplate* t = sObjectMgr->GetItemTemplate(itemEntry))
        {
            if (player->GetSession())
            {
                char buf[256];
                snprintf(buf, sizeof(buf),
                    "|cFFFFD700[Dungeon Master]|r Reward mailed: |cFFFFFFFF%s|r", t->Name1.c_str());
                ChatHandler(player->GetSession()).SendSysMessage(buf);
            }
        }
    }
    else
    {
        LOG_ERROR("module", "DungeonMaster: Failed to create mail item {} for {}", itemEntry, player->GetName());
    }
}


void DungeonMasterMgr::DistributeRoguelikeRewards(uint32 tier, uint8 effectiveLevel,
                                                    const std::vector<ObjectGuid>& playerGuids)
{
    uint8 rewardLevel = static_cast<uint8>(std::min<uint32>(effectiveLevel, 80));


    uint32 baseGold = effectiveLevel * 500u;
    uint32 tierGold = baseGold * tier;

    // Epic chance scales with tier
    uint32 epicChance = std::min<uint32>(5 + (tier * 5), 80);


    uint32 blueItems  = 1;
    uint32 greenItems = 0;
    uint32 epicItems  = 0;

    if (tier >= 9)      { blueItems = 3; epicItems = 1; }
    else if (tier >= 7) { blueItems = 2; epicItems = 1; }
    else if (tier >= 5) { blueItems = 2; }
    else if (tier >= 3) { blueItems = 1; greenItems = 1; }

    LOG_INFO("module", "DungeonMaster: DistributeRoguelikeRewards — tier={}, level={}, "
        "blue={}, green={}, epic={}, epicChance={}%, gold={}",
        tier, rewardLevel, blueItems, greenItems, epicItems, epicChance, tierGold);

    for (const auto& guid : playerGuids)
    {
        Player* p = ObjectAccessor::FindPlayer(guid);
        if (!p || !p->IsInWorld()) continue;

        // Gold
        GiveGoldReward(p, tierGold);

        // Items go directly to inventory (mail fallback if bags full)

        // Guaranteed epic items
        for (uint32 i = 0; i < epicItems; ++i)
            GiveItemReward(p, rewardLevel, 4);

        // Roll for bonus epics
        if (epicItems == 0 && RandInt<uint32>(1, 100) <= epicChance)
            GiveItemReward(p, rewardLevel, 4);
        else if (epicItems > 0 && tier >= 9 && RandInt<uint32>(1, 100) <= 25)
            GiveItemReward(p, rewardLevel, 4);

        // Blue items
        for (uint32 i = 0; i < blueItems; ++i)
            GiveItemReward(p, rewardLevel, 3);

        // Green items
        for (uint32 i = 0; i < greenItems; ++i)
            GiveItemReward(p, rewardLevel, 2);

        if (p->GetSession())
            ChatHandler(p->GetSession()).SendSysMessage(
                "|cFF00FFFF[Roguelike]|r Rewards added to your inventory!");
    }
}

// Armor type by class
static uint8 GetMaxArmorSubclass(uint32 playerClass)
{
    switch (playerClass)
    {
        case 5: case 8: case 9:              return 1;  // cloth: Priest, Mage, Warlock
        case 4: case 11:                     return 2;  // leather: Rogue, Druid
        case 3: case 7:                      return 3;  // mail: Hunter, Shaman
        case 1: case 2: case 6:              return 4;  // plate: Warrior, Paladin, DK
        default:                             return 4;
    }
}

static uint32 GetClassBitmask(uint32 playerClass)
{
    if (playerClass == 0 || playerClass > 11) return 0x7FF;  // all classes
    return 1 << (playerClass - 1);
}

uint32 DungeonMasterMgr::SelectRewardItem(uint8 level, uint8 quality, uint32 playerClass)
{
    uint8  maxArmor   = GetMaxArmorSubclass(playerClass);
    uint32 classMask  = GetClassBitmask(playerClass);

    // Try progressively wider level windows, but always prefer closer to player level
    struct { uint8 below; uint8 above; } windows[] = {
        { 3, 0 },    // strict: [level-3, level]
        { 8, 0 },    // medium: [level-8, level]
        { 15, 0 },   // wide: [level-15, level]
        { 25, 0 },   // very wide: [level-25, level]
        { 80, 0 },   // last resort: [1, level] (never items above player level)
    };

    for (const auto& win : windows)
    {
        std::vector<uint32> cands;
        uint8 lo = (level > win.below) ? (level - win.below) : 1;
        uint8 hi = level;  // Never give items above player level

        for (const auto& ri : _rewardItems)
        {
            // Quality filter
            if (ri.Quality != quality) continue;

            // Level filter: item RequiredLevel must be within window
            if (ri.MinLevel < lo || ri.MinLevel > hi) continue;

            // Class restriction: AllowableClass bitmask check
            if (ri.AllowableClass != -1 && !(ri.AllowableClass & classMask))
                continue;

            // Armor subclass: player can only wear their class's max armor or lower
            if (ri.Class == 4 && ri.SubClass > 0 && ri.SubClass <= 4)
            {
                if (ri.SubClass > maxArmor) continue;
            }

            cands.push_back(ri.Entry);
        }

        if (!cands.empty())
        {
            LOG_INFO("module", "DungeonMaster: SelectRewardItem(level={}, quality={}, class={}) "
                "-> {} candidates in window [{}, {}]",
                level, quality, playerClass, cands.size(), lo, hi);
            return cands[RandInt<size_t>(0, cands.size() - 1)];
        }
    }

    LOG_WARN("module", "DungeonMaster: SelectRewardItem(level={}, quality={}, class={}) "
        "-> NO candidates found in reward pool ({} items total)",
        level, quality, playerClass, _rewardItems.size());

    return 0;
}

uint32 DungeonMasterMgr::SelectLootItem(uint8 level, uint8 minQuality, uint8 maxQuality,
                                        bool equipmentOnly, uint32 playerClass)
{
    // Expected ItemLevel range for this level
    uint16 expectedMaxIlvl = static_cast<uint16>(level) * 2 + 10;

    uint8  maxArmor  = playerClass ? GetMaxArmorSubclass(playerClass) : 4;
    uint32 classMask = playerClass ? GetClassBitmask(playerClass) : 0x7FF;

    // Progressively widen level windows, always preferring items closer to player level
    struct { uint8 below; uint8 above; } windows[] = {
        { 3, 1 },   // strict: RequiredLevel in [level-3, level+1]
        { 5, 2 },   // medium
        { 8, 3 },   // wide
        { 15, 5 },  // very wide
        { 25, 8 },  // extremely wide (last resort)
    };

    for (const auto& win : windows)
    {
        uint8 lo = (level > win.below) ? (level - win.below) : 0;
        uint8 hi = std::min<uint16>(level + win.above, 83);

        std::vector<uint32> cands;
        for (const auto& li : _lootPool)
        {
            if (li.Quality < minQuality || li.Quality > maxQuality) continue;
            if (equipmentOnly && li.ItemClass != 2 && li.ItemClass != 4) continue;

            // Level filter for items with RequiredLevel > 0
            if (li.MinLevel > 0)
            {
                if (li.MinLevel < lo || li.MinLevel > hi) continue;
            }
            else
            {
                // RequiredLevel = 0: use ItemLevel as a sanity check
                if (li.ItemLevel > expectedMaxIlvl) continue;
            }

            // Class restriction for equipment items
            if (equipmentOnly || (li.ItemClass == 2 || li.ItemClass == 4))
            {
                // AllowableClass bitmask check
                if (li.AllowableClass != -1 && !(li.AllowableClass & classMask))
                    continue;

                // Armor subclass check (only for armor, not weapons)
                if (li.ItemClass == 4 && li.SubClass > 0 && li.SubClass <= 4)
                    if (li.SubClass > maxArmor) continue;
            }

            cands.push_back(li.Entry);
        }

        if (!cands.empty())
        {
            LOG_INFO("module", "DungeonMaster: SelectLootItem(level={}, quality={}-{}, eqOnly={}, class={}) "
                "-> {} candidates in window [{}, {}]",
                level, minQuality, maxQuality, equipmentOnly, playerClass, cands.size(), lo, hi);
            return cands[RandInt<size_t>(0, cands.size() - 1)];
        }
    }

    LOG_WARN("module", "DungeonMaster: SelectLootItem(level={}, quality={}-{}, eqOnly={}, class={}) "
        "-> NO candidates found in loot pool ({} items total)",
        level, minQuality, maxQuality, equipmentOnly, playerClass, _lootPool.size());

    return 0;
}


void DungeonMasterMgr::FillCreatureLoot(Creature* creature, Session* session, bool isBoss)
{
    if (!creature || !session) return;

    Loot& loot = creature->loot;
    loot.clear();

    uint8 level = session->EffectiveLevel;

    // Pick a random party member's class for loot filtering
    uint32 lootClass = 0;
    if (!session->Players.empty())
    {
        // Try to pick a random alive player's class
        std::vector<uint32> classes;
        for (const auto& pd : session->Players)
        {
            Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid);
            if (p && p->IsAlive())
                classes.push_back(p->getClass());
        }
        if (classes.empty())
        {
            // All dead? Just pick from any player
            for (const auto& pd : session->Players)
            {
                Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid);
                if (p) { classes.push_back(p->getClass()); break; }
            }
        }
        if (!classes.empty())
            lootClass = classes[RandInt<size_t>(0, classes.size() - 1)];
    }

    // Gold drop
    uint32 baseGold = isBoss ? (level * 2000u) : (level * 200u);
    loot.gold = std::max(500u, baseGold + RandInt<uint32>(0, baseGold / 3));

    // Item drops
    uint32 itemsAdded = 0;
    auto addItem = [&](uint8 minQ, uint8 maxQ, bool eqOnly) -> bool
    {
        uint32 entry = SelectLootItem(level, minQ, maxQ, eqOnly, eqOnly ? lootClass : 0);
        if (!entry)
        {
            LOG_WARN("module", "DungeonMaster: FillCreatureLoot failed to find item (level={}, quality={}-{}, eqOnly={}, class={})",
                level, minQ, maxQ, eqOnly, lootClass);
            return false;
        }

        LootStoreItem storeItem(entry, 0, 100.0f, false, 1, 0, 1, 1);
        loot.AddItem(storeItem);
        ++itemsAdded;
        LOG_INFO("module", "DungeonMaster: Added loot item {} (quality {}-{}) to {} (boss={})",
            entry, minQ, maxQ, creature->GetName(), isBoss);
        return true;
    };

    if (isBoss)
    {
        // Boss: 2 guaranteed rare (blue) equipment pieces
        if (!addItem(3, 3, true))
            addItem(2, 3, true);   // fallback to green/blue if no rare at this level
        if (!addItem(3, 3, true))
            addItem(2, 3, true);
    }
    else
    {
        bool isElite = false;
        for (const auto& sc : session->SpawnedCreatures)
            if (sc.Guid == creature->GetGUID() && sc.IsElite) { isElite = true; break; }

        if (isElite)
        {
            // Elite: 40% chance of green equipment
            if (RandInt<uint32>(1, 100) <= 40)
            {
                if (!addItem(2, 2, true))
                    addItem(2, 2, false);
            }
        }
        else
        {
            // Trash: 15% grey/white junk, 3% green equipment
            if (RandInt<uint32>(1, 100) <= 15)
                addItem(0, 1, false);
            if (RandInt<uint32>(1, 100) <= 3)
                addItem(2, 2, true);
        }
    }

    // Ensure lootable flag is set (critical for boss loot)
    creature->SetDynamicFlag(UNIT_DYNFLAG_LOOTABLE);

    // --- Group Loot Support (Need/Greed) ---
    // If the killing player is in a group with Group Loot or Need Before Greed,
    // trigger the group's loot distribution system for qualifying items.
    loot.loot_type = LOOT_CORPSE;
    Player* looter = nullptr;
    Group*  group  = nullptr;
    for (const auto& pd : session->Players)
    {
        Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid);
        if (p && p->IsInWorld() && p->GetGroup())
        {
            looter = p;
            group  = p->GetGroup();
            break;
        }
    }

    if (group && looter)
    {
        // Set the loot owner so the group system can process it
        creature->SetLootRecipient(looter);

        // Mark items above the group's loot threshold for rolling;
        // items below threshold become free-for-all (direct loot)
        uint8 threshold = group->GetLootThreshold();
        for (auto& item : loot.items)
        {
            const ItemTemplate* proto = sObjectMgr->GetItemTemplate(item.itemid);
            if (!proto || proto->Quality < threshold)
                item.is_underthreshold = true;
        }

        // Trigger group loot distribution — sends Need/Greed/Pass rolls
        // to all eligible group members for qualifying items
        group->GroupLoot(&loot, creature);

        LOG_INFO("module", "DungeonMaster: Group loot triggered for {} — {} items eligible for rolls",
            creature->GetName(), loot.items.size());
    }
    
    LOG_INFO("module", "DungeonMaster: FillCreatureLoot complete for {} (GUID: {}, Boss: {}, Level: {}, Gold: {}, Items: {})",
        creature->GetName(), creature->GetGUID().GetCounter(), isBoss, level, loot.gold, itemsAdded);
}

// Session end / cleanup
void DungeonMasterMgr::EndSession(uint32 sessionId, bool success)
{
    // Check if roguelike session
    uint32 roguelikeRunId = 0;
    {
        std::lock_guard<std::mutex> lock(_sessionMutex);
        auto it = _activeSessions.find(sessionId);
        if (it == _activeSessions.end()) return;

        Session& s = it->second;
        roguelikeRunId = s.RoguelikeRunId;

        if (roguelikeRunId != 0)
        {
            LOG_INFO("module", "DungeonMaster: EndSession {} — roguelike run {}, delegating to RoguelikeMgr.",
                sessionId, roguelikeRunId);

            // Persist stats while session is still alive
            UpdatePlayerStatsFromSession(s, success);

            // Clean up mappings
            uint32 savedInstanceId = s.InstanceId;
            if (savedInstanceId != 0)
                _instanceToSession.erase(savedInstanceId);
            for (const auto& pd : s.Players)
                _playerToSession.erase(pd.PlayerGuid);

            _activeSessions.erase(it);
        }
    } // lock released

    if (roguelikeRunId != 0)
    {
        sRoguelikeMgr->EndRun(roguelikeRunId, false);
        return;
    }

    // --- Normal (non-roguelike) session ---
    std::lock_guard<std::mutex> lock(_sessionMutex);
    auto it = _activeSessions.find(sessionId);
    if (it == _activeSessions.end()) return;

    Session& s = it->second;

    LOG_INFO("module", "DungeonMaster: EndSession {} — success={}, state={}, players={}",
        sessionId, success, static_cast<int>(s.State), s.Players.size());

    for (const auto& pd : s.Players)
        if (Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid))
            if (p->GetSession())
                ChatHandler(p->GetSession()).SendSysMessage(
                    success ? "|cFF00FF00[Dungeon Master]|r Challenge complete! Distributing rewards..."
                            : "|cFFFF0000[Dungeon Master]|r Challenge ended. No rewards given.");

    if (success && s.State == SessionState::Completed)
        DistributeRewards(&s);

    UpdatePlayerStatsFromSession(s, success);
    if (success && s.State == SessionState::Completed)
        SaveLeaderboardEntry(s);

    TeleportPartyOut(&s);

    // Save instance ID before cleanup
    uint32 savedInstanceId = s.InstanceId;
    CleanupSession(s);

    for (const auto& pd : s.Players)
        SetCooldown(pd.PlayerGuid);

    if (savedInstanceId != 0)
        _instanceToSession.erase(savedInstanceId);
    for (const auto& pd : s.Players)
        _playerToSession.erase(pd.PlayerGuid);

    _activeSessions.erase(it);
}

void DungeonMasterMgr::AbandonSession(uint32 id) { EndSession(id, false); }


void DungeonMasterMgr::CleanupRoguelikeSession(uint32 sessionId, bool success)
{
    std::lock_guard<std::mutex> lock(_sessionMutex);
    auto it = _activeSessions.find(sessionId);
    if (it == _activeSessions.end()) return;

    Session& s = it->second;


    UpdatePlayerStatsFromSession(s, success);
    if (success && s.State == SessionState::Completed)
        SaveLeaderboardEntry(s);


    uint32 savedInstanceId = s.InstanceId;

    // Clean up mappings (no teleport/cooldowns for roguelike)
    if (savedInstanceId != 0)
        _instanceToSession.erase(savedInstanceId);
    for (const auto& pd : s.Players)
        _playerToSession.erase(pd.PlayerGuid);

    _activeSessions.erase(it);

    LOG_DEBUG("module", "DungeonMaster: Roguelike session {} cleaned up (success={}).",
        sessionId, success);
}

void DungeonMasterMgr::CleanupSession(Session& s) { s.InstanceId = 0; }

// Cooldowns
bool DungeonMasterMgr::IsOnCooldown(ObjectGuid g) const
{
    std::lock_guard<std::mutex> lock(_cooldownMutex);
    auto it = _cooldowns.find(g);
    return it != _cooldowns.end()
        && GameTime::GetGameTime().count() < static_cast<time_t>(it->second);
}

void DungeonMasterMgr::SetCooldown(ObjectGuid g)
{
    std::lock_guard<std::mutex> lock(_cooldownMutex);
    _cooldowns[g] = GameTime::GetGameTime().count() + sDMConfig->GetCooldownMinutes() * 60;
}

void DungeonMasterMgr::ClearCooldown(ObjectGuid g)
{
    std::lock_guard<std::mutex> lock(_cooldownMutex);
    _cooldowns.erase(g);
}

uint32 DungeonMasterMgr::GetRemainingCooldown(ObjectGuid g) const
{
    std::lock_guard<std::mutex> lock(_cooldownMutex);
    auto it = _cooldowns.find(g);
    if (it == _cooldowns.end()) return 0;
    time_t now = GameTime::GetGameTime().count();
    return (now < static_cast<time_t>(it->second))
        ? static_cast<uint32>(it->second - now) : 0;
}

bool DungeonMasterMgr::CanCreateNewSession() const
{
    return _activeSessions.size() < sDMConfig->GetMaxConcurrentRuns();
}

// Player Statistics & Leaderboard

void DungeonMasterMgr::LoadAllPlayerStats()
{
    std::lock_guard<std::mutex> lock(_statsMutex);
    _playerStats.clear();

    QueryResult result = CharacterDatabase.Query(
        "SELECT guid, total_runs, completed_runs, failed_runs, "
        "total_mobs_killed, total_bosses_killed, total_deaths, fastest_clear "
        "FROM dm_player_stats");

    if (!result)
    {
        LOG_INFO("module", "DungeonMaster: No player stats found (table may be empty or missing).");
        return;
    }

    uint32 count = 0;
    do
    {
        Field* f = result->Fetch();
        uint32 guidLow = f[0].Get<uint32>();

        PlayerStats ps;
        ps.TotalRuns        = f[1].Get<uint32>();
        ps.CompletedRuns    = f[2].Get<uint32>();
        ps.FailedRuns       = f[3].Get<uint32>();
        ps.TotalMobsKilled  = f[4].Get<uint32>();
        ps.TotalBossesKilled = f[5].Get<uint32>();
        ps.TotalDeaths      = f[6].Get<uint32>();
        ps.FastestClear     = f[7].Get<uint32>();

        _playerStats[guidLow] = ps;
        ++count;
    } while (result->NextRow());

    LOG_INFO("module", "DungeonMaster: Loaded stats for {} players.", count);
}

PlayerStats DungeonMasterMgr::GetPlayerStats(ObjectGuid guid) const
{
    std::lock_guard<std::mutex> lock(_statsMutex);
    uint32 guidLow = guid.GetCounter();
    auto it = _playerStats.find(guidLow);
    if (it != _playerStats.end())
        return it->second;
    return {};
}

void DungeonMasterMgr::SavePlayerStats(uint32 guidLow)
{
    PlayerStats ps;
    {
        std::lock_guard<std::mutex> lock(_statsMutex);
        auto it = _playerStats.find(guidLow);
        if (it == _playerStats.end()) return;
        ps = it->second;
    }

    char query[512];
    snprintf(query, sizeof(query),
        "REPLACE INTO dm_player_stats "
        "(guid, total_runs, completed_runs, failed_runs, "
        "total_mobs_killed, total_bosses_killed, total_deaths, fastest_clear) "
        "VALUES (%u, %u, %u, %u, %u, %u, %u, %u)",
        guidLow, ps.TotalRuns, ps.CompletedRuns, ps.FailedRuns,
        ps.TotalMobsKilled, ps.TotalBossesKilled, ps.TotalDeaths, ps.FastestClear);
    CharacterDatabase.Execute(query);
}

void DungeonMasterMgr::UpdatePlayerStatsFromSession(const Session& session, bool success)
{
    uint32 clearTime = 0;
    if (session.EndTime > session.StartTime)
        clearTime = static_cast<uint32>(session.EndTime - session.StartTime);
    else
        clearTime = static_cast<uint32>(GameTime::GetGameTime().count() - session.StartTime);

    for (const auto& pd : session.Players)
    {
        uint32 guidLow = pd.PlayerGuid.GetCounter();

        {
            std::lock_guard<std::mutex> lock(_statsMutex);
            auto& ps = _playerStats[guidLow];
            ps.TotalRuns++;
            if (success)
            {
                ps.CompletedRuns++;
                if (ps.FastestClear == 0 || clearTime < ps.FastestClear)
                    ps.FastestClear = clearTime;
            }
            else
                ps.FailedRuns++;

            ps.TotalMobsKilled   += pd.MobsKilled;
            ps.TotalBossesKilled += pd.BossesKilled;
            ps.TotalDeaths       += pd.Deaths;
        }

        SavePlayerStats(guidLow);
    }
}

void DungeonMasterMgr::SaveLeaderboardEntry(const Session& session)
{
    uint32 clearTime = 0;
    if (session.EndTime > session.StartTime)
        clearTime = static_cast<uint32>(session.EndTime - session.StartTime);
    else
        clearTime = static_cast<uint32>(GameTime::GetGameTime().count() - session.StartTime);

    if (clearTime == 0) return;


    std::string leaderName = "Unknown";
    if (Player* leader = ObjectAccessor::FindPlayer(session.LeaderGuid))
        leaderName = leader->GetName();

    uint8 partySize = static_cast<uint8>(session.Players.size());

    // Escape name for SQL
    std::string safeName = leaderName;
    size_t pos = 0;
    while ((pos = safeName.find('\'', pos)) != std::string::npos)
    {
        safeName.replace(pos, 1, "''");
        pos += 2;
    }

    char query[512];
    snprintf(query, sizeof(query),
        "INSERT INTO dm_leaderboard "
        "(guid, char_name, map_id, difficulty_id, clear_time, party_size, scaled) "
        "VALUES (%u, '%s', %u, %u, %u, %u, %u)",
        session.LeaderGuid.GetCounter(), safeName.c_str(),
        session.MapId, session.DifficultyId, clearTime,
        partySize, session.ScaleToParty ? 1u : 0u);
    CharacterDatabase.Execute(query);
}

std::vector<LeaderboardEntry> DungeonMasterMgr::GetLeaderboard(
    uint32 mapId, uint32 difficultyId, uint32 limit) const
{
    std::vector<LeaderboardEntry> entries;

    char query[512];
    snprintf(query, sizeof(query),
        "SELECT id, guid, char_name, map_id, difficulty_id, clear_time, party_size, scaled "
        "FROM dm_leaderboard "
        "WHERE map_id = %u AND difficulty_id = %u "
        "ORDER BY clear_time ASC LIMIT %u",
        mapId, difficultyId, limit);

    QueryResult result = CharacterDatabase.Query(query);

    if (!result) return entries;

    do
    {
        Field* f = result->Fetch();
        LeaderboardEntry e;
        e.Id           = f[0].Get<uint32>();
        e.Guid         = f[1].Get<uint32>();
        e.CharName     = f[2].Get<std::string>();
        e.MapId        = f[3].Get<uint32>();
        e.DifficultyId = f[4].Get<uint32>();
        e.ClearTime    = f[5].Get<uint32>();
        e.PartySize    = f[6].Get<uint8>();
        e.Scaled       = f[7].Get<uint8>() != 0;
        entries.push_back(e);
    } while (result->NextRow());

    return entries;
}

std::vector<LeaderboardEntry> DungeonMasterMgr::GetOverallLeaderboard(uint32 limit) const
{
    std::vector<LeaderboardEntry> entries;

    char query[512];
    snprintf(query, sizeof(query),
        "SELECT id, guid, char_name, map_id, difficulty_id, clear_time, party_size, scaled "
        "FROM dm_leaderboard "
        "ORDER BY clear_time ASC LIMIT %u",
        limit);

    QueryResult result = CharacterDatabase.Query(query);

    if (!result) return entries;

    do
    {
        Field* f = result->Fetch();
        LeaderboardEntry e;
        e.Id           = f[0].Get<uint32>();
        e.Guid         = f[1].Get<uint32>();
        e.CharName     = f[2].Get<std::string>();
        e.MapId        = f[3].Get<uint32>();
        e.DifficultyId = f[4].Get<uint32>();
        e.ClearTime    = f[5].Get<uint32>();
        e.PartySize    = f[6].Get<uint8>();
        e.Scaled       = f[7].Get<uint8>() != 0;
        entries.push_back(e);
    } while (result->NextRow());

    return entries;
}

// Scaling multipliers
float DungeonMasterMgr::CalculateHealthMultiplier(const Session* s) const
{
    if (!s) return 1.0f;
    const DifficultyTier* d = sDMConfig->GetDifficulty(s->DifficultyId);
    if (!d) return 1.0f;

    float base = d->HealthMultiplier;
    uint32 n   = s->Players.size();
    float mult;
    if (n <= 1) mult = base * sDMConfig->GetSoloMultiplier();
    else        mult = base * (1.0f + (n - 1) * sDMConfig->GetPerPlayerHealthMult());

    // Roguelike tier scaling
    if (s->RoguelikeRunId != 0)
        mult *= sRoguelikeMgr->GetTierHealthMultiplier(s->RoguelikeRunId);

    return mult;
}

float DungeonMasterMgr::CalculateDamageMultiplier(const Session* s) const
{
    if (!s) return 1.0f;
    const DifficultyTier* d = sDMConfig->GetDifficulty(s->DifficultyId);
    if (!d) return 1.0f;

    float base = d->DamageMultiplier;
    uint32 n   = s->Players.size();
    float mult;
    if (n <= 1) mult = base * sDMConfig->GetSoloMultiplier();
    else        mult = base * (1.0f + (n - 1) * sDMConfig->GetPerPlayerDamageMult());

    // Roguelike tier scaling
    if (s->RoguelikeRunId != 0)
        mult *= sRoguelikeMgr->GetTierDamageMultiplier(s->RoguelikeRunId);

    return mult;
}

// Check if creature belongs to an active session
bool DungeonMasterMgr::IsSessionCreature(ObjectGuid playerGuid, ObjectGuid creatureGuid)
{
    std::lock_guard<std::mutex> lock(_sessionMutex);
    auto pit = _playerToSession.find(playerGuid);
    if (pit == _playerToSession.end())
        return false;

    auto sit = _activeSessions.find(pit->second);
    if (sit == _activeSessions.end())
        return false;

    return sit->second.IsSessionCreature(creatureGuid);
}

// Scale environmental damage to party level
float DungeonMasterMgr::GetEnvironmentalDamageScale(ObjectGuid playerGuid)
{
    std::lock_guard<std::mutex> lock(_sessionMutex);
    auto pit = _playerToSession.find(playerGuid);
    if (pit == _playerToSession.end())
        return 1.0f;

    auto sit = _activeSessions.find(pit->second);
    if (sit == _activeSessions.end())
        return 1.0f;

    const Session& session = sit->second;

    if (!session.ScaleToParty)
        return 1.0f;

    const DungeonInfo* dg = sDMConfig->GetDungeon(session.MapId);
    if (!dg)
        return 1.0f;

    uint8 dungeonLevel = dg->MaxLevel;
    uint8 partyLevel   = session.EffectiveLevel;

    if (partyLevel >= dungeonLevel)
        return 1.0f;

    float ratio = static_cast<float>(partyLevel) / static_cast<float>(dungeonLevel);
    float scale = std::pow(ratio, 1.5f);
    if (scale < 0.05f)
        scale = 0.05f;

    return scale;
}

// Main update tick (1s interval)
void DungeonMasterMgr::Update(uint32 diff)
{
    _updateTimer += diff;
    if (_updateTimer < UPDATE_INTERVAL)
        return;
    _updateTimer = 0;

    std::vector<std::pair<uint32, bool>> toEnd;
    std::vector<std::pair<uint32, uint32>> roguelikeCompleted; // {runId, sessionId}

    {
        std::lock_guard<std::mutex> lock(_sessionMutex);

        for (auto& [sid, session] : _activeSessions)
        {
            // ---- Poll creature deaths ----
            if (session.IsActive())
            {
                Player* ref = nullptr;
                for (const auto& pd : session.Players)
                {
                    Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid);
                    if (p && p->GetMapId() == session.MapId) { ref = p; break; }
                }

                if (ref)
                {
                    // ---- Ensure instance mapping is registered ----
                    if (session.InstanceId == 0)
                    {
                        Map* m2 = ref->GetMap();
                        if (m2 && m2->IsDungeon())
                        {
                            session.InstanceId = m2->ToInstanceMap()->GetInstanceId();
                        }
                    }
                    if (session.InstanceId != 0 &&
                        _instanceToSession.find(session.InstanceId) == _instanceToSession.end())
                    {
                        _instanceToSession[session.InstanceId] = session.SessionId;
                    }

                    // ---- Populate if not yet done ----
                    if (session.TotalMobs == 0 && session.TotalBosses == 0)
                    {
                        Map* m = ref->GetMap();
                        if (m && m->IsDungeon())
                        {
                            InstanceMap* inst = m->ToInstanceMap();
                            if (inst)
                            {
                                session.InstanceId = inst->GetInstanceId();
                                _instanceToSession[session.InstanceId] = session.SessionId;

                                for (const auto& pd2 : session.Players)
                                    if (Player* p2 = ObjectAccessor::FindPlayer(pd2.PlayerGuid))
                                        ChatHandler(p2->GetSession()).SendSysMessage(
                                            "|cFF00FF00[Dungeon Master]|r Preparing the challenge...");

                                PopulateDungeon(&session, inst);

                                LOG_INFO("module", "DungeonMaster: Session {} — populated (map {}, mobs={}, bosses={})",
                                    session.SessionId, session.MapId,
                                    session.TotalMobs, session.TotalBosses);

                                char buf[256];
                                snprintf(buf, sizeof(buf),
                                    "|cFF00FF00[Dungeon Master]|r |cFFFFFFFF%u|r enemies and "
                                    "|cFFFFFFFF%u|r boss(es) spawned. Creature levels: "
                                    "|cFFFFFFFF%u-%u|r. Good luck!",
                                    session.TotalMobs, session.TotalBosses,
                                    session.LevelBandMin, session.LevelBandMax);
                                for (const auto& pd2 : session.Players)
                                    if (Player* p2 = ObjectAccessor::FindPlayer(pd2.PlayerGuid))
                                        ChatHandler(p2->GetSession()).SendSysMessage(buf);
                            }
                        }
                    }

                    // Build set of our known GUIDs for stray detection
                    std::set<ObjectGuid> ourGuids;
                    for (const auto& sc : session.SpawnedCreatures)
                        ourGuids.insert(sc.Guid);

                    for (auto& sc : session.SpawnedCreatures)
                    {
                        if (sc.IsDead) continue;
                        Creature* c = ObjectAccessor::GetCreature(*ref, sc.Guid);
                        if (!c || !c->IsAlive())
                        {
                            sc.IsDead = true;
                            if (c) FillCreatureLoot(c, &session, sc.IsBoss);
                            GiveKillXP(&session, sc.IsBoss, sc.IsElite);

                            if (sc.IsBoss)
                            {
                                // Multi-phase: defer boss kill for phase check
                                PendingPhaseCheck ppc;
                                if (c)
                                    ppc.DeathPos = { c->GetPositionX(), c->GetPositionY(),
                                                     c->GetPositionZ(), c->GetOrientation() };
                                ppc.DeathTime = GameTime::GetGameTime().count();
                                ppc.OrigEntry = sc.Entry;
                                ppc.Resolved  = false;
                                session.PendingPhaseChecks.push_back(ppc);
                            }
                            else
                            {
                                ++session.MobsKilled;
                                for (auto& pd : session.Players)
                                    ++pd.MobsKilled;
                            }
                        }
                    }

                    // ---- Multi-phase boss resolution ----
                    // After 5 seconds, check if new creatures spawned near the boss death location.
                    // If found, promote them to boss status. If not, confirm the boss kill.
                    uint64 nowTime = GameTime::GetGameTime().count();
                    for (auto& ppc : session.PendingPhaseChecks)
                    {
                        if (ppc.Resolved) continue;
                        if (nowTime - ppc.DeathTime < 5) continue;  // Wait 5 seconds for phase transitions

                        ppc.Resolved = true;

                        // Scan for new non-tracked creatures near the boss death position
                        bool phaseCreatureFound = false;
                        Map* scanMap = ref->GetMap();
                        if (scanMap && scanMap->IsDungeon() && ppc.DeathPos.GetPositionX() != 0.0f)
                        {
                            std::list<Creature*> nearby;
                            ref->GetCreatureListWithEntryInGrid(nearby, 0, 5000.0f);

                            for (Creature* nc : nearby)
                            {
                                if (!nc || !nc->IsAlive() || nc->IsPet() || nc->IsGuardian())
                                    continue;
                                if (nc->GetEntry() == sDMConfig->GetNpcEntry())
                                    continue;
                                if (ourGuids.count(nc->GetGUID()) > 0)
                                    continue;  // Already tracked

                                // Check distance from boss death position (within 40 yards)
                                float dx = nc->GetPositionX() - ppc.DeathPos.GetPositionX();
                                float dy = nc->GetPositionY() - ppc.DeathPos.GetPositionY();
                                float dz = nc->GetPositionZ() - ppc.DeathPos.GetPositionZ();
                                float dist = std::sqrt(dx*dx + dy*dy + dz*dz);

                                if (dist > 40.0f) continue;

                                // Check if it's an elite/boss creature (likely phase 2)
                                const CreatureTemplate* tmpl = nc->GetCreatureTemplate();
                                if (!tmpl || (tmpl->rank != 1 && tmpl->rank != 2 && tmpl->rank != 4))
                                    continue;

                                // Promote to boss creature
                                LOG_INFO("module", "DungeonMaster: Phase creature detected! '{}' (entry {}) "
                                    "spawned {:.1f} yds from boss death location — promoting to boss",
                                    nc->GetName(), nc->GetEntry(), dist);

                                nc->SetFaction(14);
                                nc->SetReactState(REACT_AGGRESSIVE);
                                nc->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_IMMUNE_TO_PC
                                                                | UNIT_FLAG_IMMUNE_TO_NPC | UNIT_FLAG_PACIFIED);
                                nc->SetImmuneToPC(false);
                                nc->SetImmuneToNPC(false);

                                SpawnedCreature nsc;
                                nsc.Guid = nc->GetGUID();
                                nsc.Entry = nc->GetEntry();
                                nsc.IsElite = true;
                                nsc.IsBoss = true;
                                session.SpawnedCreatures.push_back(nsc);
                                ourGuids.insert(nc->GetGUID());

                                // Track the GUID for cleanup
                                auto& gl = _instanceCreatureGuids[session.InstanceId];
                                gl.push_back(nc->GetGUID());

                                phaseCreatureFound = true;

                                for (const auto& pd3 : session.Players)
                                    if (Player* p3 = ObjectAccessor::FindPlayer(pd3.PlayerGuid))
                                        if (p3->GetSession())
                                            ChatHandler(p3->GetSession()).SendSysMessage(
                                                "|cFFFF8000[Dungeon Master]|r The boss enters a new phase!");
                                break;  // Only promote one phase creature per check
                            }
                        }

                        if (!phaseCreatureFound)
                        {
                            // No phase creature found — confirm the boss kill
                            ++session.BossesKilled;
                            for (auto& pd : session.Players)
                                ++pd.BossesKilled;

                            LOG_INFO("module", "DungeonMaster: Boss kill confirmed (entry {}) — progress: {}/{}",
                                ppc.OrigEntry, session.BossesKilled, session.TotalBosses);
                            HandleBossDeath(&session);

                            // Check completion
                            if (session.IsActive() && session.TotalBosses > 0
                                && session.BossesKilled >= session.TotalBosses)
                            {
                                session.State   = SessionState::Completed;
                                session.EndTime = GameTime::GetGameTime().count();

                                uint32 delay = (session.RoguelikeRunId != 0)
                                    ? sDMConfig->GetRoguelikeTransitionDelay()
                                    : sDMConfig->GetCompletionTeleportDelay();

                                for (const auto& pd2 : session.Players)
                                    if (Player* p = ObjectAccessor::FindPlayer(pd2.PlayerGuid))
                                        if (p->GetSession())
                                        {
                                            char buf[256];
                                            snprintf(buf, sizeof(buf),
                                                "|cFF00FF00[Dungeon Master]|r %s "
                                                "Rewards in |cFFFFFFFF%u|r seconds...",
                                                session.RoguelikeRunId != 0
                                                    ? "Floor cleared!" : "Dungeon complete!",
                                                delay);
                                            ChatHandler(p->GetSession()).SendSysMessage(buf);
                                        }
                                break;
                            }
                        }
                    }

                    // Clean up resolved phase checks
                    session.PendingPhaseChecks.erase(
                        std::remove_if(session.PendingPhaseChecks.begin(), session.PendingPhaseChecks.end(),
                            [](const PendingPhaseCheck& p) { return p.Resolved; }),
                        session.PendingPhaseChecks.end());

                    // ---- Sweep for stray creatures (script-spawned, respawned) ----
                    Map* m = ref->GetMap();
                    if (m && m->IsDungeon())
                    {
                        uint32 npcEntry = sDMConfig->GetNpcEntry();
                        auto const& dbStore = static_cast<InstanceMap*>(m)->GetCreatureBySpawnIdStore();
                        for (auto const& pair : dbStore)
                        {
                            Creature* stray = pair.second;
                            if (stray && stray->IsInWorld() && stray->IsAlive()
                                && stray->GetEntry() != npcEntry
                                && !stray->IsPet() && !stray->IsGuardian() && !stray->IsTotem()
                                && ourGuids.count(stray->GetGUID()) == 0)
                            {
                                stray->SetRespawnTime(7 * DAY);
                                stray->DespawnOrUnsummon();
                            }
                        }
                    }
                }

                // ---- Auto-rez when out of combat ----
                if (session.IsActive() && !session.IsGroupInCombat())
                {
                    for (const auto& pd : session.Players)
                    {
                        Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid);
                        if (p && !p->IsAlive() && p->GetMapId() == session.MapId)
                        {
                            p->RemoveFlag(PLAYER_FIELD_BYTES, PLAYER_FIELD_BYTE_NO_RELEASE_WINDOW);
                            p->ResurrectPlayer(1.0f);
                            p->SpawnCorpseBones();
                            p->TeleportTo(session.MapId,
                                session.EntrancePos.GetPositionX(),
                                session.EntrancePos.GetPositionY(),
                                session.EntrancePos.GetPositionZ(),
                                session.EntrancePos.GetOrientation());
                            ChatHandler(p->GetSession()).SendSysMessage(
                                "|cFF00FF00[Dungeon Master]|r Revived at entrance. Get back in there!");
                        }
                    }
                }
            }

            // ---- Time limit ----
            if (session.TimeLimit > 0 && session.State == SessionState::InProgress)
            {
                uint64 elapsed = GameTime::GetGameTime().count() - session.StartTime;
                if (elapsed >= session.TimeLimit)
                {
                    session.State = SessionState::Failed;
                    toEnd.emplace_back(sid, false);
                    for (const auto& pd : session.Players)
                        if (Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid))
                            ChatHandler(p->GetSession()).SendSysMessage(
                                "|cFFFF0000[Dungeon Master]|r Time's up! Challenge failed.");
                    continue;
                }
            }

            // ---- Completed → teleport delay ----
            if (session.State == SessionState::Completed)
            {
                uint32 delay = (session.RoguelikeRunId != 0)
                    ? sDMConfig->GetRoguelikeTransitionDelay()
                    : sDMConfig->GetCompletionTeleportDelay();
                uint64 elapsed = GameTime::GetGameTime().count() - session.EndTime;

                // ---- Roguelike countdown announcements ----
                if (session.RoguelikeRunId != 0 && elapsed < delay)
                {
                    uint32 remaining = static_cast<uint32>(delay - elapsed);
                    static const uint32 announceAt[] = {25, 20, 15, 10, 5, 4, 3, 2, 1};
                    for (uint32 sec : announceAt)
                    {
                        if (remaining == sec)
                        {
                            char cbuf[128];
                            snprintf(cbuf, sizeof(cbuf),
                                "|cFF00FFFF[Roguelike]|r Next dungeon in |cFFFFFFFF%u|r second%s...",
                                remaining, remaining != 1 ? "s" : "");
                            for (const auto& pd3 : session.Players)
                                if (Player* p3 = ObjectAccessor::FindPlayer(pd3.PlayerGuid))
                                    if (p3->GetSession())
                                        ChatHandler(p3->GetSession()).SendSysMessage(cbuf);
                            break;
                        }
                    }
                }

                if (elapsed >= delay)
                {
                    if (session.RoguelikeRunId != 0)
                        roguelikeCompleted.push_back({session.RoguelikeRunId, sid});
                    else
                        toEnd.emplace_back(sid, true);
                    continue;
                }
            }

            // ---- Failed cleanup ----
            if (session.State == SessionState::Failed)
            {
                // Roguelike sessions: wipe is handled by RoguelikeMgr::OnPartyWipe
                if (session.RoguelikeRunId != 0)
                    continue;

                if (session.EndTime == 0)
                    session.EndTime = GameTime::GetGameTime().count();
                else if (GameTime::GetGameTime().count() - session.EndTime >= 2)
                {
                    toEnd.emplace_back(sid, false);
                    continue;
                }
            }

            // ---- Abandoned detection ----
            if (session.IsActive()
                && (GameTime::GetGameTime().count() - session.StartTime) >= 15)
            {
                bool anyone = false;
                for (const auto& pd : session.Players)
                {
                    Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid);
                    if (p && p->GetMapId() == session.MapId) { anyone = true; break; }
                }
                if (!anyone)
                {
                    LOG_INFO("module", "DungeonMaster: Session {} abandoned — no players on map {} after grace period",
                        sid, session.MapId);
                    session.State = SessionState::Abandoned;
                    toEnd.emplace_back(sid, false);
                }
            }
        }
    } // release lock

    for (const auto& [id, ok] : toEnd)
        EndSession(id, ok);

    // Process roguelike completions outside session lock
    for (const auto& [runId, sessId] : roguelikeCompleted)
        sRoguelikeMgr->OnDungeonCompleted(runId, sessId);


    {
        std::lock_guard<std::mutex> lock(_cooldownMutex);
        time_t now = GameTime::GetGameTime().count();
        for (auto it = _cooldowns.begin(); it != _cooldowns.end(); )
            (now >= static_cast<time_t>(it->second)) ? it = _cooldowns.erase(it) : ++it;
    }
}

std::string DungeonMasterMgr::GetSessionStatusString(const Session* s) const
{
    if (!s) return "No session";
    static const char* names[] = { "None","Preparing","InProgress","BossPhase","Completed","Failed","Abandoned" };
    char buf[256];
    snprintf(buf, sizeof(buf), "Session %u — %s, Mobs %u/%u, Bosses %u/%u, Band %u-%u",
        s->SessionId, names[static_cast<int>(s->State)],
        s->MobsKilled, s->TotalMobs, s->BossesKilled, s->TotalBosses,
        s->LevelBandMin, s->LevelBandMax);
    return buf;
}

} // namespace DungeonMaster
