/*
 * Copyright (C) 2025 AzerothCore - mod-dungeon-master
 *
 * DMTypes.h — Shared data structures for the Dungeon Master module.
 *
 * Design notes:
 *   - All types live in the DungeonMaster namespace to avoid collisions.
 *   - POD-like structs are preferred over classes so they can be stored
 *     in flat containers (unordered_map, vector) without custom allocators.
 *   - Session is the central state object.  One Session per active run.
 *
 * Released under GNU GPL v2.
 */

#ifndef DM_TYPES_H
#define DM_TYPES_H

#include "Define.h"
#include "ObjectGuid.h"
#include "Position.h"
#include <string>
#include <vector>

class Player;
class Creature;
class Map;
class InstanceMap;

namespace DungeonMaster
{

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
constexpr uint32 MAX_DIFFICULTIES      = 10;
constexpr uint32 MAX_THEMES            = 20;
constexpr uint32 MAX_PARTY_SIZE        = 5;

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------

/// Lifecycle of a single dungeon run.
enum class SessionState : uint8
{
    None       = 0,
    Preparing,          // Instance is being set up
    InProgress,         // Players are inside, fighting trash
    BossPhase,          // Final boss spawned / engaged
    Completed,          // Boss dead, awaiting teleport-out
    Failed,             // Time expired or too many wipes
    Abandoned           // All players left the instance
};

// ---------------------------------------------------------------------------
// Configuration structs  (populated once at startup from .conf)
// ---------------------------------------------------------------------------

/// One row in the difficulty table (parsed from DungeonMaster.Difficulty.N).
struct DifficultyTier
{
    uint32      Id               = 0;
    std::string Name;
    uint8       MinLevel         = 1;
    uint8       MaxLevel         = 80;
    float       HealthMultiplier = 1.0f;
    float       DamageMultiplier = 1.0f;
    float       RewardMultiplier = 1.0f;
    float       MobCountMultiplier = 1.0f;

    /// A player can *select* this difficulty if they meet the minimum level.
    bool IsValidForLevel(uint8 level) const { return level >= MinLevel; }

    /// True when the player is within the intended band (not over-leveled).
    bool IsOnLevelFor(uint8 level) const { return level >= MinLevel && level <= MaxLevel; }
};

/// A creature-theme groups one or more WoW creature types together.
struct Theme
{
    uint32                  Id = 0;
    std::string             Name;
    std::vector<uint32>     CreatureTypes;   // WoW creature types (1=Beast…); -1 = any

    bool IsRandom() const
    {
        return CreatureTypes.size() == 1 && CreatureTypes[0] == uint32(-1);
    }
};

/// Static metadata about a dungeon map.
struct DungeonInfo
{
    uint32      MapId       = 0;
    std::string Name;
    uint8       MinLevel    = 1;
    uint8       MaxLevel    = 80;
    Position    EntrancePos;
    bool        IsAvailable = true;
};

// ---------------------------------------------------------------------------
// Runtime / session structs
// ---------------------------------------------------------------------------

/// A position inside the dungeon where a creature can be placed.
struct SpawnPoint
{
    Position    Pos;
    float       DistanceFromEntrance = 0.0f;
    bool        IsBossPosition       = false;
    bool        IsUsed               = false;
};

/// Tracks a single creature that the module has summoned.
struct SpawnedCreature
{
    ObjectGuid  Guid;
    uint32      Entry   = 0;
    bool        IsElite = false;
    bool        IsBoss  = false;
    bool        IsDead  = false;
};

/// Per-player bookkeeping within a session.
struct PlayerSessionData
{
    ObjectGuid  PlayerGuid;
    Position    ReturnPosition;
    uint32      ReturnMapId  = 0;
    uint32      MobsKilled   = 0;
    uint32      BossesKilled = 0;
    uint32      Deaths       = 0;
};

/// The master state object for one dungeon run.
struct Session
{
    uint32          SessionId    = 0;
    ObjectGuid      LeaderGuid;
    SessionState    State        = SessionState::None;

    // --- Configuration chosen at creation ---
    uint32  DifficultyId = 0;
    uint32  ThemeId      = 0;
    uint32  MapId        = 0;
    uint32  InstanceId   = 0;

    // --- Effective level band (derived from player/group level) ---
    uint8   EffectiveLevel = 1;     // anchor level for creature selection
    uint8   LevelBandMin   = 1;     // EffectiveLevel - LEVEL_BAND
    uint8   LevelBandMax   = 80;    // EffectiveLevel + LEVEL_BAND

    // --- Timing ---
    uint64  StartTime = 0;
    uint64  EndTime   = 0;
    uint32  TimeLimit = 0;          // 0 = unlimited

    // --- Tracking ---
    std::vector<PlayerSessionData>  Players;
    std::vector<SpawnedCreature>    SpawnedCreatures;
    std::vector<SpawnPoint>         SpawnPoints;

    // --- Progress ---
    uint32  TotalMobs   = 0;
    uint32  MobsKilled  = 0;
    uint32  TotalBosses = 0;
    uint32  BossesKilled = 0;
    uint32  Wipes       = 0;

    // --- Dungeon entrance (for respawns) ---
    Position EntrancePos;

    // ---- Helpers ----
    bool IsActive() const
    {
        return State == SessionState::InProgress
            || State == SessionState::BossPhase
            || State == SessionState::Preparing;
    }

    bool IsComplete() const { return State == SessionState::Completed; }

    bool HasPlayer(ObjectGuid guid) const
    {
        for (const auto& p : Players)
            if (p.PlayerGuid == guid)
                return true;
        return false;
    }

    PlayerSessionData* GetPlayerData(ObjectGuid guid)
    {
        for (auto& p : Players)
            if (p.PlayerGuid == guid)
                return &p;
        return nullptr;
    }

    uint32 GetAlivePlayerCount() const;
    bool   IsPartyWiped() const;
    bool   IsGroupInCombat() const;
};

// ---------------------------------------------------------------------------
// Creature / item pool entries  (loaded from world DB at startup)
// ---------------------------------------------------------------------------

/// One row from the creature pool query, carrying level info for filtering.
struct CreaturePoolEntry
{
    uint32 Entry    = 0;
    uint32 Type     = 0;
    uint8  MinLevel = 1;
    uint8  MaxLevel = 80;
};

/// Base stats from creature_classlevelstats, used to force-scale creatures.
struct ClassLevelStatEntry
{
    uint32 BaseHP         = 1;
    float  BaseDamage     = 1.0f;
    uint32 BaseArmor      = 0;
    uint32 AttackPower    = 0;
};

/// One candidate reward item.
struct RewardItem
{
    uint32 Entry         = 0;
    uint32 MinLevel      = 1;
    uint32 MaxLevel      = 80;
    uint8  Quality       = 0;   // 0=Poor … 4=Epic
    uint32 InventoryType = 0;
    uint32 Class         = 0;
    uint32 SubClass      = 0;
};

/// One item in the mob loot pool (all qualities, broader categories).
struct LootPoolItem
{
    uint32 Entry     = 0;
    uint8  MinLevel  = 0;
    uint8  Quality   = 0;    // 0=Grey 1=White 2=Green 3=Blue 4=Epic
    uint8  ItemClass = 0;    // 2=Weapon, 4=Armor, etc.
};

/// Lifetime stats for one player (placeholder – not yet persisted).
struct PlayerStats
{
    ObjectGuid PlayerGuid;
    uint32 TotalRuns       = 0;
    uint32 CompletedRuns   = 0;
    uint32 FailedRuns      = 0;
    uint32 TotalMobsKilled = 0;
    uint32 TotalBossesKilled = 0;
    uint32 TotalDeaths     = 0;
};

} // namespace DungeonMaster

#endif // DM_TYPES_H
