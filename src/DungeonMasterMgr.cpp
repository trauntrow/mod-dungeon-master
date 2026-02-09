/*
 * Copyright (C) 2025 AzerothCore - mod-dungeon-master
 *
 * DungeonMasterMgr.cpp — Implementation of the central manager.
 *
 * KEY SCALING FIX (Task 3):
 *   Previous versions used the *difficulty tier's* MinLevel / MaxLevel to
 *   select creatures, which meant a level-30 player on a "Journeyman 30-44"
 *   run could face level-44 mobs.  Now we derive an EffectiveLevel from
 *   the actual player (or group average) and apply a tight ±LEVEL_BAND
 *   window (default ±3).  Only creatures whose own level range intersects
 *   that window are eligible.  This eliminates mixed-level packs entirely.
 */

#include "DungeonMasterMgr.h"
#include "DMConfig.h"
#include "Player.h"
#include "Group.h"
#include "Creature.h"
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
#include <random>
#include <algorithm>
#include <set>
#include <cstdio>

namespace DungeonMaster
{

// ---------------------------------------------------------------------------
// RNG helpers (thread-local for safety)
// ---------------------------------------------------------------------------
static thread_local std::mt19937 tRng{ std::random_device{}() };

template<typename T>
static T RandInt(T lo, T hi) { return std::uniform_int_distribution<T>(lo, hi)(tRng); }

static float RandFloat(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(tRng); }

// ---------------------------------------------------------------------------
// Session helper implementations (declared in DMTypes.h)
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------
DungeonMasterMgr::DungeonMasterMgr()  = default;
DungeonMasterMgr::~DungeonMasterMgr() = default;

DungeonMasterMgr* DungeonMasterMgr::Instance()
{
    static DungeonMasterMgr inst;
    return &inst;
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------
void DungeonMasterMgr::Initialize()
{
    LOG_INFO("module", "DungeonMaster: Initializing...");
    LoadFromDB();
    LOG_INFO("module", "DungeonMaster: Ready — {} creature types, {} bosses, {} reward items, {} loot items.",
        _creaturesByType.size(), _bossCreatures.size(), _rewardItems.size(), _lootPool.size());
}

void DungeonMasterMgr::LoadFromDB()
{
    LoadCreaturePools();
    LoadClassLevelStats();
    LoadRewardItems();
    LoadLootPool();
    LoadAllPlayerStats();
}

// ---------------------------------------------------------------------------
// LoadCreaturePools — build the themed creature & boss pools from world DB.
//
// LEVEL-AGNOSTIC DESIGN:  We load ALL valid combat creatures regardless of
// their original template level.  SelectCreatureForTheme picks purely by
// creature type, and applyLevelAndStats force-scales every creature to the
// session's target level (level, HP, damage, armor, resistances, rank).
// A level 80 Northrend ghoul picked for a level 14 session becomes a
// proper level 14 mob with correct stats and no skull/"??" display.
//
// Pools:
//   _creaturesByType : type → normal creatures (rank 0, rare rank 3 excluded)
//   _bossCreatures   : type → elite/rare-elite creatures (rank 1, 2, 4)
//
// Minimal filtering — only exclude entries that are inherently broken when
// summoned.  We force-override faction, flags, immunities, level, stats,
// and everything else in applyLevelAndStats, so original template values
// are irrelevant.  If SummonCreature fails (no model, bad entry), we
// simply skip it (the nullptr check handles that).
// ---------------------------------------------------------------------------
void DungeonMasterMgr::LoadCreaturePools()
{
    _creaturesByType.clear();
    _bossCreatures.clear();

    // Ultra-minimal query: type for theming, level for bookkeeping, rank for boss/trash split
    QueryResult result = WorldDatabase.Query(
        "SELECT entry, type, minlevel, maxlevel, `rank` "
        "FROM creature_template "
        "WHERE type > 0 AND type <= 10 AND type != 8 "               // combat types, skip Critter
        "AND minlevel > 0 AND maxlevel <= 83 "
        "AND `rank` != 3 "                                            // not World Boss
        "AND name NOT LIKE '%Trigger%' "
        "AND name NOT LIKE '%Invisible%' "
        "AND name NOT LIKE '%Dummy%' "
        "AND name NOT LIKE '%[DNT]%' "
        "AND name NOT LIKE '% - DNT' "
        "AND name NOT LIKE '%zzOLD%' "
        "ORDER BY type, minlevel");

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

    // Log per-type breakdown so admins can verify theme coverage
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

// ---------------------------------------------------------------------------
// LoadClassLevelStats — cache creature_classlevelstats for force-scaling.
//
// This table provides the canonical base HP / damage / armor for every
// (unit_class, level) pair.  When we force a creature to a target level
// we look up the correct base stats here instead of relying on whatever
// the creature's original template had.
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// GetBaseStatsForLevel — look up cached base stats.
// Falls back to class 1 (Warrior) if the specific class isn't found, and
// returns nullptr only if the level itself is completely missing.
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// LoadRewardItems — cache equippable green / blue / purple items.
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// LoadLootPool — cache items for mob drops and boss drops.
// Includes grey junk through epic equipment so bosses can drop
// level-appropriate rare/epic gear.
// ---------------------------------------------------------------------------
void DungeonMasterMgr::LoadLootPool()
{
    _lootPool.clear();

    // Load all usable items: grey vendor junk, white consumables,
    // green/blue/purple equipment (weapons + armor).
    // Exclude equipment items with RequiredLevel=0 (these are often GM/test/high-ilvl
    // items that would be inappropriate for low-level players).
    // Non-equipment (consumables, junk) with RequiredLevel=0 is fine.
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

    // Count by quality for logging
    uint32 counts[5] = {};
    for (const auto& li : _lootPool)
        if (li.Quality <= 4) ++counts[li.Quality];

    LOG_INFO("module", "DungeonMaster: {} mob loot items cached "
        "(grey={}, white={}, green={}, blue={}, epic={}).",
        _lootPool.size(), counts[0], counts[1], counts[2], counts[3], counts[4]);
}

// ===========================================================================
// LEVEL-BAND CALCULATION  (Task 3 — the core scaling fix)
// ===========================================================================
//
// Determines the EffectiveLevel for creature selection:
//   Solo player   → player's level
//   Group / bots  → arithmetic mean of all members
//
// The session then stores [EffectiveLevel - band, EffectiveLevel + band],
// and only creatures within that window are eligible for spawning.
// ---------------------------------------------------------------------------
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

// ===========================================================================
// SESSION LIFECYCLE
// ===========================================================================

Session* DungeonMasterMgr::CreateSession(Player* leader, uint32 difficultyId,
                                          uint32 themeId, uint32 mapId,
                                          bool scaleToParty)
{
    if (!CanCreateNewSession())
        return nullptr;

    const DifficultyTier* diff  = sDMConfig->GetDifficulty(difficultyId);
    const Theme*          theme = sDMConfig->GetTheme(themeId);
    const DungeonInfo*    dg    = sDMConfig->GetDungeon(mapId);
    if (!diff || !theme || !dg)
        return nullptr;

    std::lock_guard<std::mutex> lock(_sessionMutex);

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

    // ---- Compute the level band ----
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

    // ---- Add leader ----
    PlayerSessionData ld;
    ld.PlayerGuid  = leader->GetGUID();
    ld.ReturnMapId = leader->GetMapId();
    ld.ReturnPosition = { leader->GetPositionX(), leader->GetPositionY(),
                          leader->GetPositionZ(), leader->GetOrientation() };
    s.Players.push_back(ld);

    // ---- Add group members ----
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

// ===========================================================================
// StartDungeon / TeleportPartyIn / TeleportPartyOut
// ===========================================================================

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

        if (p->TeleportTo(session->MapId, ent.GetPositionX(), ent.GetPositionY(),
                          ent.GetPositionZ(), ent.GetOrientation()))
        {
            ++ok;
            char buf[256];
            snprintf(buf, sizeof(buf),
                "|cFF00FF00[Dungeon Master]|r Welcome to |cFFFFFFFF%s|r! "
                "Defeat all enemies and the boss to claim your reward.",
                dg->Name.c_str());
            ChatHandler(p->GetSession()).SendSysMessage(buf);
        }
        else
        {
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
        if (!p) continue;
        p->RemoveFlag(PLAYER_FIELD_BYTES, PLAYER_FIELD_BYTE_NO_RELEASE_WINDOW);
        if (!p->IsAlive()) { p->ResurrectPlayer(1.0f); p->SpawnCorpseBones(); }
        p->TeleportTo(pd.ReturnMapId, pd.ReturnPosition.GetPositionX(),
                      pd.ReturnPosition.GetPositionY(), pd.ReturnPosition.GetPositionZ(),
                      pd.ReturnPosition.GetOrientation());
    }
}

// ===========================================================================
// Dungeon entrance lookup — areatrigger_teleport is authoritative.
// ===========================================================================
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

// ===========================================================================
// Spawn-point collection
// ===========================================================================
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

    // Sort near → far; bosses go at the far end.
    std::sort(pts.begin(), pts.end(),
        [](const SpawnPoint& a, const SpawnPoint& b)
        { return a.DistanceFromEntrance < b.DistanceFromEntrance; });

    uint32 bc = sDMConfig->GetBossCount();
    for (uint32 i = 0; i < bc && i < pts.size(); ++i)
        pts[pts.size() - 1 - i].IsBossPosition = true;

    return pts;
}

// ===========================================================================
// Instance population
// ===========================================================================
void DungeonMasterMgr::ClearDungeonCreatures(InstanceMap* map)
{
    if (!map) return;

    // --- First: despawn any creatures WE previously summoned in this instance ---
    uint32 instanceId = map->GetInstanceId();
    auto guidIt = _instanceCreatureGuids.find(instanceId);
    uint32 summonedRemoved = 0;
    if (guidIt != _instanceCreatureGuids.end())
    {
        for (const ObjectGuid& guid : guidIt->second)
        {
            Creature* c = map->GetCreature(guid);
            if (c && c->IsInWorld())
            {
                c->DespawnOrUnsummon();
                ++summonedRemoved;
            }
        }
        guidIt->second.clear();
    }

    // --- Second: despawn ALL DB-spawned creatures (alive AND dead) ---
    // Dead creatures must also be handled to prevent scripted respawns
    // (e.g. SFK event mobs that die and respawn via instance script).
    std::vector<Creature*> toRemove;
    uint32 npcEntry = sDMConfig->GetNpcEntry();
    auto const& store = map->GetCreatureBySpawnIdStore();
    for (auto const& pair : store)
    {
        Creature* c = pair.second;
        if (c && c->IsInWorld() && !c->IsPet() && !c->IsGuardian()
            && !c->IsTotem() && c->GetEntry() != npcEntry)
            toRemove.push_back(c);
    }

    for (Creature* c : toRemove)
    {
        if (c && c->IsInWorld())
        {
            c->SetRespawnTime(7 * DAY);
            c->DespawnOrUnsummon();
        }
    }

    LOG_DEBUG("module", "DungeonMaster: Cleared {} DB + {} summoned creatures from map {}",
        toRemove.size(), summonedRemoved, map->GetId());
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

// ---------------------------------------------------------------------------
// PopulateDungeon — the heart of the module.
//
// For every original spawn point in the dungeon, we summon ONE themed
// creature whose level falls within the session's computed level band.
// Boss positions get a scaled-up elite / boss creature.
//
// FORCE-SCALING:  After spawning, every creature is set to the session's
// EffectiveLevel and given base HP / damage / armor from the canonical
// creature_classlevelstats table, then difficulty multipliers are applied.
// This ensures creatures always display and fight at the correct level
// regardless of what their original template said.
// ---------------------------------------------------------------------------
void DungeonMasterMgr::PopulateDungeon(Session* session, InstanceMap* map)
{
    if (!session || !map) return;

    const DifficultyTier* diff  = sDMConfig->GetDifficulty(session->DifficultyId);
    const Theme*          theme = sDMConfig->GetTheme(session->ThemeId);
    if (!diff || !theme) return;

    ClearDungeonCreatures(map);
    OpenAllDoors(map);

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

    // Prepare the GUID tracking list for this instance
    uint32 instanceId = map->GetInstanceId();
    auto& guidList = _instanceCreatureGuids[instanceId];
    guidList.clear();

    LOG_INFO("module", "DungeonMaster: Populating session {} — theme '{}', band {}-{}, target lvl {}, HP x{:.2f}, DMG x{:.2f}",
        session->SessionId, theme->Name, bandMin, bandMax, targetLevel, hpMult, dmgMult);

    // Helper lambda: force a creature to the target level with proper stats.
    // This is the core of the level-agnostic design — ANY creature from level 1-83
    // becomes a proper creature at targetLevel with correct HP/damage/armor/display.
    // No skull icon, no "??" level, no elite dragon frame (unless it's a boss).
    auto applyLevelAndStats = [&](Creature* c, float extraHpMult, float extraDmgMult, bool /*isBoss*/)
    {
        // --- Force level FIRST (before any stat calculations) ---
        // This is what determines the client's level display.  The skull/"??"
        // appears when the creature level is 10+ above the player; since we
        // set it to targetLevel (near the player), it will show as a number.
        c->SetLevel(targetLevel);

        uint8 unitClass = c->GetCreatureTemplate()->unit_class;
        const ClassLevelStatEntry* baseStats = GetBaseStatsForLevel(unitClass, targetLevel);

        // --- Health ---
        float finalHP;
        if (baseStats)
            finalHP = static_cast<float>(baseStats->BaseHP) * hpMult * extraHpMult;
        else
            finalHP = c->GetMaxHealth() * hpMult * extraHpMult;

        uint32 hp = std::max(1u, static_cast<uint32>(finalHP));
        c->SetMaxHealth(hp);
        c->SetHealth(hp);

        // --- Damage ---
        if (baseStats)
        {
            float dmgBase  = baseStats->BaseDamage;
            float apBonus  = static_cast<float>(baseStats->AttackPower) / 14.0f;
            float atkTime  = static_cast<float>(c->GetCreatureTemplate()->BaseAttackTime) / 1000.0f;
            if (atkTime <= 0.0f) atkTime = 2.0f;

            float minDmg = (dmgBase + apBonus) * atkTime * dmgMult * extraDmgMult;
            float maxDmg = ((dmgBase * 1.5f) + apBonus) * atkTime * dmgMult * extraDmgMult;

            minDmg = std::max(1.0f, minDmg);
            maxDmg = std::max(minDmg, maxDmg);

            c->SetBaseWeaponDamage(BASE_ATTACK, MINDAMAGE, minDmg);
            c->SetBaseWeaponDamage(BASE_ATTACK, MAXDAMAGE, maxDmg);
            c->UpdateDamagePhysical(BASE_ATTACK);
        }

        // --- Armor (from classlevelstats for the TARGET level) ---
        if (baseStats && baseStats->BaseArmor > 0)
            c->SetArmor(baseStats->BaseArmor);

        // --- Clear ALL spell resistances (original template values are for original level) ---
        for (uint8 school = SPELL_SCHOOL_HOLY; school < MAX_SPELL_SCHOOL; ++school)
            c->SetResistance(SpellSchools(school), 0);

        // --- Clear mechanic immunities ---
        for (uint32 mech = 1; mech < MAX_MECHANIC; ++mech)
            c->ApplySpellImmune(0, IMMUNITY_MECHANIC, mech, false);

        // --- Clear spell immunities that might come from the template ---
        c->ApplySpellImmune(0, IMMUNITY_SCHOOL, SPELL_SCHOOL_MASK_ALL, false);

        // --- Movement: idle at spawn, no wandering ---
        c->SetWanderDistance(0.0f);
        c->SetDefaultMovementType(IDLE_MOTION_TYPE);
        c->GetMotionMaster()->MoveIdle();

        // --- CRITICAL: Force full visibility refresh ---
        // SummonCreature sends CREATE_OBJECT with the *template* level/rank.
        // We need the client to receive updated fields.  On AzerothCore the
        // cleanest way is to force a visibility update which re-sends all
        // unit fields to nearby players.
        c->UpdateObjectVisibility(true);

        // Track this GUID for future cleanup
        guidList.push_back(c->GetGUID());
    };

    // ---- Trash mobs ----
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

        bool isElite = (RandInt<uint32>(1, 100) <= sDMConfig->GetEliteChance());
        float eliteHpMult  = isElite ? sDMConfig->GetEliteHealthMult() : 1.0f;
        float eliteDmgMult = isElite ? 1.5f : 1.0f;

        applyLevelAndStats(c, eliteHpMult, eliteDmgMult, false);

        SpawnedCreature sc;
        sc.Guid = c->GetGUID(); sc.Entry = entry;
        sc.IsElite = isElite; sc.IsBoss = false;
        session->SpawnedCreatures.push_back(sc);
        ++spawnedMobs;
    }
    session->TotalMobs = spawnedMobs;

    // ---- Bosses ----
    uint32 bossesSpawned = 0;
    for (auto& sp : session->SpawnPoints)
    {
        if (!sp.IsBossPosition || bossesSpawned >= sDMConfig->GetBossCount())
            continue;

        uint32 entry = SelectCreatureForTheme(theme, true);
        if (!entry) { LOG_WARN("module", "DungeonMaster: No boss candidate."); continue; }

        Creature* b = map->SummonCreature(entry, sp.Pos);
        if (!b) continue;

        b->SetFaction(14);
        b->SetReactState(REACT_AGGRESSIVE);
        b->SetObjectScale(1.0f);
        b->SetCorpseDelay(600);          // 10 min corpse for bosses
        b->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_IMMUNE_TO_PC
                                        | UNIT_FLAG_IMMUNE_TO_NPC | UNIT_FLAG_PACIFIED
                                        | UNIT_FLAG_STUNNED | UNIT_FLAG_FLEEING
                                        | UNIT_FLAG_NOT_SELECTABLE);
        b->SetUInt32Value(UNIT_FIELD_FLAGS_2, 0);
        b->SetImmuneToPC(false);
        b->SetImmuneToNPC(false);

        applyLevelAndStats(b, sDMConfig->GetBossHealthMult(), sDMConfig->GetBossDamageMult(), true);

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

// ===========================================================================
// Creature selection — LEVEL-AGNOSTIC, TYPE-ONLY
//
// Original template level is IRRELEVANT.  We select purely by creature
// type (matching the session's theme), then applyLevelAndStats force-scales
// every creature to the session's target level.  A level 80 Northrend
// ghoul becomes a proper level 14 mob — correct HP, damage, armor, and
// the client displays "14" on the nameplate (no skull, no "??").
//
// This means every theme has access to the ENTIRE creature database for
// its type, not just the tiny slice at the session's native level range.
// "Undead Rising" at level 14 can pick from Duskwood ghouls, Stratholme
// skeletons, AND Icecrown geists — they'll all fight identically at 14.
// ===========================================================================
uint32 DungeonMasterMgr::SelectCreatureForTheme(const Theme* theme, bool isBoss)
{
    if (!theme) return 0;

    // Build the set of acceptable creature types for the theme
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

    // --- Last resort: if theme has zero entries, use ANY type ---
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

// ===========================================================================
// Death handling
// ===========================================================================
void DungeonMasterMgr::HandleCreatureDeath(Creature* creature, Session* session)
{
    if (!creature || !session || !session->IsActive())
        return;

    for (auto& sc : session->SpawnedCreatures)
    {
        if (sc.Guid == creature->GetGUID())
        {
            sc.IsDead = true;
            FillCreatureLoot(creature, session, sc.IsBoss);
            GiveKillXP(session, sc.IsBoss, sc.IsElite);
            if (sc.IsBoss) { ++session->BossesKilled; HandleBossDeath(session); }
            else           { ++session->MobsKilled; }

            // Credit all party members with the kill
            for (auto& pd : session->Players)
            {
                if (sc.IsBoss) ++pd.BossesKilled;
                else           ++pd.MobsKilled;
            }
            break;
        }
    }

    // Check completion
    if (session->IsActive() && session->TotalBosses > 0
        && session->BossesKilled >= session->TotalBosses)
    {
        session->State   = SessionState::Completed;
        session->EndTime = GameTime::GetGameTime().count();

        for (const auto& pd : session->Players)
            if (Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid))
            {
                char buf[256];
                snprintf(buf, sizeof(buf),
                    "|cFF00FF00[Dungeon Master]|r Congratulations! Dungeon complete! "
                    "Rewards in |cFFFFFFFF%u|r seconds...",
                    sDMConfig->GetCompletionTeleportDelay());
                ChatHandler(p->GetSession()).SendSysMessage(buf);
            }
    }
}

void DungeonMasterMgr::HandleBossDeath(Session* session)
{
    if (!session) return;
    for (const auto& pd : session->Players)
        if (Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid))
        {
            if (session->BossesKilled < session->TotalBosses)
            {
                char buf[128];
                snprintf(buf, sizeof(buf),
                    "|cFFFFFF00[Dungeon Master]|r Boss defeated! |cFFFFFFFF%u|r remaining.",
                    session->TotalBosses - session->BossesKilled);
                ChatHandler(p->GetSession()).SendSysMessage(buf);
            }
        }
}

void DungeonMasterMgr::HandlePlayerDeath(Player* player, Session* session)
{
    if (!player || !session) return;

    if (PlayerSessionData* pd = session->GetPlayerData(player->GetGUID()))
        ++pd->Deaths;

    // Block release-spirit (player must wait for auto-rez)
    player->SetFlag(PLAYER_FIELD_BYTES, PLAYER_FIELD_BYTE_NO_RELEASE_WINDOW);
    player->RemoveFlag(PLAYER_FIELD_BYTES, PLAYER_FIELD_BYTE_RELEASE_TIMER);

    if (session->IsPartyWiped())
    {
        ++session->Wipes;
        session->State   = SessionState::Failed;
        session->EndTime = GameTime::GetGameTime().count();

        for (const auto& psd : session->Players)
        {
            Player* p = ObjectAccessor::FindPlayer(psd.PlayerGuid);
            if (!p) continue;
            p->RemoveFlag(PLAYER_FIELD_BYTES, PLAYER_FIELD_BYTE_NO_RELEASE_WINDOW);
            if (!p->IsAlive()) { p->ResurrectPlayer(1.0f); p->SpawnCorpseBones(); }
            ChatHandler(p->GetSession()).SendSysMessage(
                "|cFFFF0000[Dungeon Master]|r Total party wipe! Challenge failed.");
            p->TeleportTo(psd.ReturnMapId, psd.ReturnPosition.GetPositionX(),
                psd.ReturnPosition.GetPositionY(), psd.ReturnPosition.GetPositionZ(),
                psd.ReturnPosition.GetOrientation());
        }
    }
    else
    {
        ChatHandler(player->GetSession()).SendSysMessage(
            "|cFFFFFF00[Dungeon Master]|r You have fallen! "
            "You will be revived when your group leaves combat.");
    }
}

// ===========================================================================
// Rewards
// ===========================================================================
void DungeonMasterMgr::DistributeRewards(Session* session)
{
    if (!session) return;
    const DifficultyTier* diff = sDMConfig->GetDifficulty(session->DifficultyId);
    if (!diff) return;

    // Gold scales with the session's effective level
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
        if (!p)
        {
            LOG_WARN("module", "DungeonMaster: Player {} not found for rewards", pd.PlayerGuid.GetCounter());
            continue;
        }

        GiveGoldReward(p, perPlayer);

        // Completion reward: always give at least one item
        // Determine quality: roll epic first, then rare, fallback green
        uint8 quality = 2;  // green baseline
        if (RandInt<uint32>(1, 100) <= sDMConfig->GetEpicChance())
            quality = 4;
        else if (RandInt<uint32>(1, 100) <= sDMConfig->GetRareChance())
            quality = 3;

        GiveItemReward(p, rewardLevel, quality);
    }
}

// ---------------------------------------------------------------------------
// GiveKillXP — Award level-appropriate XP to every player in the session.
//
// Each player receives XP based on THEIR OWN level (not the mob's), so
// mixed-level groups each get a fair amount.  Max-level players are skipped.
//
// Base formula (WoW-like):  XP = (playerLevel * 5) + 45
//   Elite:  x2          Boss:  x10
// ---------------------------------------------------------------------------
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

    char buf[128];
    snprintf(buf, sizeof(buf),
        "|cFFFFD700[Dungeon Master]|r You received: |cFFFFD700%u|rg |cFFC0C0C0%u|rs |cFFB87333%u|rc",
        amount / 10000, (amount % 10000) / 100, amount % 100);
    ChatHandler(player->GetSession()).SendSysMessage(buf);
}

void DungeonMasterMgr::GiveItemReward(Player* player, uint8 level, uint8 quality)
{
    uint32 playerClass = player->getClass();
    uint32 itemEntry = SelectRewardItem(level, quality, playerClass);

    // Quality fallback: if requested quality isn't found, try lower
    if (!itemEntry && quality > 2)
    {
        LOG_WARN("module", "DungeonMaster: No quality {} items for level {}, class {}. Trying lower...",
            quality, level, playerClass);
        for (uint8 q = quality - 1; q >= 2 && !itemEntry; --q)
            itemEntry = SelectRewardItem(level, q, playerClass);
    }

    // Level window fallback: try wider level ranges
    if (!itemEntry)
    {
        LOG_WARN("module", "DungeonMaster: No items for level {}, class {}, quality {}. Widening search...",
            level, playerClass, quality);

        // Try levels ±15, then ±25, then any level
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
        LOG_ERROR("module", "DungeonMaster: STILL no reward item for player {} (level {}, class {}). "
            "Reward pool has {} items total.",
            player->GetName(), level, playerClass, _rewardItems.size());
        ChatHandler(player->GetSession()).SendSysMessage(
            "|cFFFF0000[Dungeon Master]|r No suitable gear found for your class. Gold only.");
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
                char buf[256];
                snprintf(buf, sizeof(buf),
                    "|cFFFFD700[Dungeon Master]|r You received: |cFFFFFFFF%s|r", t->Name1.c_str());
                ChatHandler(player->GetSession()).SendSysMessage(buf);
            }
        }
    }
    else
    {
        Item* mailItem = Item::CreateItem(itemEntry, 1, player);
        if (mailItem)
        {
            MailDraft("Dungeon Master Reward", "Your bags were full. Here is your reward!")
                .AddItem(mailItem)
                .SendMailTo(CharacterDatabaseTransaction(nullptr),
                    MailReceiver(player, player->GetGUID().GetCounter()),
                    MailSender(MAIL_NORMAL, 0, MAIL_STATIONERY_GM));
            ChatHandler(player->GetSession()).SendSysMessage(
                "|cFFFFD700[Dungeon Master]|r Bags full! Reward mailed to you.");
        }
        else
        {
            LOG_ERROR("module", "DungeonMaster: Failed to create mail item {} for {}", itemEntry, player->GetName());
        }
    }
}

// ---------------------------------------------------------------------------
// Armor proficiency mapping:
//   WoW playerClass:  1=Warrior 2=Paladin 3=Hunter 4=Rogue 5=Priest
//                     6=DK 7=Shaman 8=Mage 9=Warlock 11=Druid
//
// Max armor subclass each class can wear:
//   Cloth(1):  Priest, Mage, Warlock
//   Leather(2): Rogue, Druid
//   Mail(3):    Hunter, Shaman
//   Plate(4):   Warrior, Paladin, Death Knight
//
// Class bitmask: bit(classId-1), so Warrior=0x001, Paladin=0x002, etc.
// ---------------------------------------------------------------------------
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

    std::vector<uint32> cands;
    for (const auto& ri : _rewardItems)
    {
        // Quality filter
        if (ri.Quality != quality) continue;

        // Level filter: item RequiredLevel must be within a sensible window
        // of the player's level (not above, and not more than 8 below)
        if (ri.MinLevel > level) continue;
        if (ri.MinLevel + 8 < level && level > 10) continue;

        // Class restriction: AllowableClass bitmask check
        if (ri.AllowableClass != -1 && !(ri.AllowableClass & classMask))
            continue;

        // Armor subclass: player can only wear their class's max armor or lower
        // (Weapons use Class=2 so this only applies to Class=4 Armor items)
        if (ri.Class == 4 && ri.SubClass > 0 && ri.SubClass <= 4)
        {
            if (ri.SubClass > maxArmor) continue;
        }

        cands.push_back(ri.Entry);
    }

    LOG_INFO("module", "DungeonMaster: SelectRewardItem(level={}, quality={}, class={}) "
        "-> {} candidates (pool={}, maxArmor={})",
        level, quality, playerClass, cands.size(), _rewardItems.size(), maxArmor);

    return cands.empty() ? 0 : cands[RandInt<size_t>(0, cands.size() - 1)];
}

uint32 DungeonMasterMgr::SelectLootItem(uint8 level, uint8 minQuality, uint8 maxQuality,
                                        bool equipmentOnly, uint32 playerClass)
{
    // Determine expected ItemLevel range for this player level.
    // Rough WoW mapping: ItemLevel ≈ RequiredLevel * 1.2 for green gear.
    // We use this as a sanity check for items with RequiredLevel=0.
    uint16 expectedMaxIlvl = static_cast<uint16>(level) * 2 + 10;

    uint8  maxArmor  = playerClass ? GetMaxArmorSubclass(playerClass) : 4;
    uint32 classMask = playerClass ? GetClassBitmask(playerClass) : 0x7FF;

    // Try increasingly wider level windows
    struct { uint8 below; uint8 above; } windows[] = {
        { 3, 1 },   // strict: RequiredLevel in [level-3, level+1]
        { 5, 2 },   // medium
        { 8, 3 },   // wide
        { 15, 5 },  // very wide (last resort)
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
                // (skip items whose ItemLevel is far above what the player should see)
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
            return cands[RandInt<size_t>(0, cands.size() - 1)];
    }

    // Ultimate fallback: any item of the right quality, ignore level entirely
    // (better to drop SOMETHING than nothing)
    std::vector<uint32> fallback;
    for (const auto& li : _lootPool)
    {
        if (li.Quality < minQuality || li.Quality > maxQuality) continue;
        if (equipmentOnly && li.ItemClass != 2 && li.ItemClass != 4) continue;
        // Still enforce ItemLevel cap to avoid absurd drops
        if (li.ItemLevel > expectedMaxIlvl) continue;
        fallback.push_back(li.Entry);
    }

    return fallback.empty() ? 0 : fallback[RandInt<size_t>(0, fallback.size() - 1)];
}

// ---------------------------------------------------------------------------
// FillCreatureLoot — Populate a dead creature's loot with level-appropriate
// gold and items.
//
// Drop table:
//   Trash:  gold (always) + 15% grey/white + 3% green equipment
//   Elite:  gold (always) + 40% green equipment
//   Boss:   gold (always) + 2 rare/epic equipment (weapons/armor)
//
// Equipment drops are filtered by a random party member's class so that
// items are always usable by someone in the group.
// ---------------------------------------------------------------------------
void DungeonMasterMgr::FillCreatureLoot(Creature* creature, Session* session, bool isBoss)
{
    if (!creature || !session) return;

    Loot& loot = creature->loot;
    loot.clear();

    uint8 level = session->EffectiveLevel;

    // Pick a random party member's class for equipment filtering.
    // This ensures drops are usable by at least one person.
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

    // --- Gold (always drops — minimum 1 silver for visibility) ---
    uint32 baseGold = isBoss ? (level * 500u) : (level * 50u);
    loot.gold = std::max(100u, baseGold + RandInt<uint32>(0, baseGold / 3));

    // --- Items ---
    auto addItem = [&](uint8 minQ, uint8 maxQ, bool eqOnly) -> bool
    {
        uint32 entry = SelectLootItem(level, minQ, maxQ, eqOnly, eqOnly ? lootClass : 0);
        if (!entry) return false;

        LootStoreItem storeItem(entry, 0, 100.0f, false, 1, 0, 1, 1);
        loot.AddItem(storeItem);
        return true;
    };

    if (isBoss)
    {
        // Boss: 2 guaranteed rare (blue) / epic (purple) equipment pieces
        if (!addItem(3, 4, true))
            addItem(2, 3, true);   // fallback to green/blue if no rare/epic at this level
        if (!addItem(3, 4, true))
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

    // Ensure lootable even if only gold
    creature->SetDynamicFlag(UNIT_DYNFLAG_LOOTABLE);
}

// ===========================================================================
// Session end / cleanup
// ===========================================================================
void DungeonMasterMgr::EndSession(uint32 sessionId, bool success)
{
    std::lock_guard<std::mutex> lock(_sessionMutex);
    auto it = _activeSessions.find(sessionId);
    if (it == _activeSessions.end()) return;

    Session& s = it->second;

    for (const auto& pd : s.Players)
        if (Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid))
            ChatHandler(p->GetSession()).SendSysMessage(
                success ? "|cFF00FF00[Dungeon Master]|r Challenge complete! Distributing rewards..."
                        : "|cFFFF0000[Dungeon Master]|r Challenge ended. No rewards given.");

    if (success && s.State == SessionState::Completed)
        DistributeRewards(&s);

    // --- Persist stats & leaderboard ---
    UpdatePlayerStatsFromSession(s, success);
    if (success && s.State == SessionState::Completed)
        SaveLeaderboardEntry(s);

    // NOTE: We intentionally do NOT despawn summoned creatures here.
    // The player may already be off the instance map, making GUID lookups
    // unreliable.  ClearDungeonCreatures() handles cleanup at the start
    // of the next run using the actual InstanceMap* pointer.

    TeleportPartyOut(&s);
    CleanupSession(s);

    for (const auto& pd : s.Players)
        SetCooldown(pd.PlayerGuid);

    _instanceToSession.erase(s.InstanceId);
    for (const auto& pd : s.Players)
        _playerToSession.erase(pd.PlayerGuid);

    _activeSessions.erase(it);
}

void DungeonMasterMgr::AbandonSession(uint32 id) { EndSession(id, false); }

void DungeonMasterMgr::CleanupSession(Session& s) { s.InstanceId = 0; }

// ===========================================================================
// Cooldowns
// ===========================================================================
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

// ===========================================================================
// Player Statistics & Leaderboard
// ===========================================================================

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

    // Get the leader's name for the leaderboard display
    std::string leaderName = "Unknown";
    if (Player* leader = ObjectAccessor::FindPlayer(session.LeaderGuid))
        leaderName = leader->GetName();

    uint8 partySize = static_cast<uint8>(session.Players.size());

    // Escape the name to prevent SQL injection (replace ' with '')
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

// ===========================================================================
// Scaling multipliers
// ===========================================================================
float DungeonMasterMgr::CalculateHealthMultiplier(const Session* s) const
{
    if (!s) return 1.0f;
    const DifficultyTier* d = sDMConfig->GetDifficulty(s->DifficultyId);
    if (!d) return 1.0f;

    float base = d->HealthMultiplier;
    uint32 n   = s->Players.size();
    if (n <= 1) return base * sDMConfig->GetSoloMultiplier();
    return base * (1.0f + (n - 1) * sDMConfig->GetPerPlayerHealthMult());
}

float DungeonMasterMgr::CalculateDamageMultiplier(const Session* s) const
{
    if (!s) return 1.0f;
    const DifficultyTier* d = sDMConfig->GetDifficulty(s->DifficultyId);
    if (!d) return 1.0f;

    float base = d->DamageMultiplier;
    uint32 n   = s->Players.size();
    if (n <= 1) return base * sDMConfig->GetSoloMultiplier();
    return base * (1.0f + (n - 1) * sDMConfig->GetPerPlayerDamageMult());
}

// ===========================================================================
// Update() — called every world tick, throttled to 1 s intervals.
// ===========================================================================
void DungeonMasterMgr::Update(uint32 diff)
{
    _updateTimer += diff;
    if (_updateTimer < UPDATE_INTERVAL)
        return;
    _updateTimer = 0;

    std::vector<std::pair<uint32, bool>> toEnd;

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
                    // The allmap script may have set session.InstanceId but
                    // can't access the private _instanceToSession map.
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
                    // This is the primary populate trigger.  If the session is
                    // InProgress but no mobs have been spawned yet, the player
                    // has arrived in the instance and we can now clear + spawn.
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

                                LOG_INFO("module", "DungeonMaster: Session {} — populated via Update tick fallback (map {})",
                                    session.SessionId, session.MapId);

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
                            if (sc.IsBoss) { ++session.BossesKilled; HandleBossDeath(&session); }
                            else           { ++session.MobsKilled; }

                            // Credit all party members
                            for (auto& pd : session.Players)
                            {
                                if (sc.IsBoss) ++pd.BossesKilled;
                                else           ++pd.MobsKilled;
                            }

                            if (session.IsActive() && session.TotalBosses > 0
                                && session.BossesKilled >= session.TotalBosses)
                            {
                                session.State   = SessionState::Completed;
                                session.EndTime = GameTime::GetGameTime().count();
                                for (const auto& pd2 : session.Players)
                                    if (Player* p = ObjectAccessor::FindPlayer(pd2.PlayerGuid))
                                    {
                                        char buf[256];
                                        snprintf(buf, sizeof(buf),
                                            "|cFF00FF00[Dungeon Master]|r Dungeon complete! "
                                            "Rewards in |cFFFFFFFF%u|r seconds...",
                                            sDMConfig->GetCompletionTeleportDelay());
                                        ChatHandler(p->GetSession()).SendSysMessage(buf);
                                    }
                                break;
                            }
                        }
                    }

                    // ---- Sweep for stray creatures (script-spawned, respawned) ----
                    // Despawn any creature on the map that we didn't spawn and
                    // isn't our DM NPC.  Catches SFK-style scripted event spawns.
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
                if (GameTime::GetGameTime().count() - session.EndTime
                        >= sDMConfig->GetCompletionTeleportDelay())
                {
                    toEnd.emplace_back(sid, true);
                    continue;
                }
            }

            // ---- Failed cleanup ----
            if (session.State == SessionState::Failed)
            {
                if (session.EndTime == 0)
                    session.EndTime = GameTime::GetGameTime().count();
                else if (GameTime::GetGameTime().count() - session.EndTime >= 2)
                {
                    toEnd.emplace_back(sid, false);
                    continue;
                }
            }

            // ---- Abandoned detection ----
            if (session.IsActive())
            {
                bool anyone = false;
                for (const auto& pd : session.Players)
                {
                    Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid);
                    if (p && p->GetMapId() == session.MapId) { anyone = true; break; }
                }
                if (!anyone)
                {
                    session.State = SessionState::Abandoned;
                    toEnd.emplace_back(sid, false);
                }
            }
        }
    } // release lock

    for (const auto& [id, ok] : toEnd)
        EndSession(id, ok);

    // Expire old cooldowns
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
