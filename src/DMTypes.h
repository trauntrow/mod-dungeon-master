/*
 * mod-dungeon-master — Shared data structures
 * AGPL v3
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

constexpr uint32 MAX_DIFFICULTIES      = 10;
constexpr uint32 MAX_THEMES            = 20;
constexpr uint32 MAX_PARTY_SIZE        = 5;

enum class SessionState : uint8
{
    None       = 0,
    Preparing,
    InProgress,
    BossPhase,
    Completed,
    Failed,
    Abandoned
};

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

    bool IsValidForLevel(uint8 level) const { return level >= MinLevel; }
    bool IsOnLevelFor(uint8 level) const { return level >= MinLevel && level <= MaxLevel; }
};

// Groups creature types for themed spawns; -1 = any type
struct Theme
{
    uint32                  Id = 0;
    std::string             Name;
    std::vector<uint32>     CreatureTypes;

    bool IsRandom() const
    {
        return CreatureTypes.size() == 1 && CreatureTypes[0] == uint32(-1);
    }
};

struct DungeonInfo
{
    uint32      MapId       = 0;
    std::string Name;
    uint8       MinLevel    = 1;
    uint8       MaxLevel    = 80;
    Position    EntrancePos;
    bool        IsAvailable = true;
};

struct SpawnPoint
{
    Position    Pos;
    float       DistanceFromEntrance = 0.0f;
    bool        IsBossPosition       = false;
    bool        IsUsed               = false;
};

struct SpawnedCreature
{
    ObjectGuid  Guid;
    uint32      Entry      = 0;
    bool        IsElite    = false;
    bool        IsBoss     = false;
    bool        IsRare     = false;
    bool        IsDead     = false;
};

struct PendingPhaseCheck
{
    Position    DeathPos;
    uint64      DeathTime  = 0;
    uint32      OrigEntry  = 0;
    bool        Resolved   = false;
};

struct PlayerSessionData
{
    ObjectGuid  PlayerGuid;
    Position    ReturnPosition;
    uint32      ReturnMapId  = 0;
    uint32      MobsKilled   = 0;
    uint32      BossesKilled = 0;
    uint32      Deaths       = 0;
};

struct Session
{
    uint32          SessionId    = 0;
    ObjectGuid      LeaderGuid;
    SessionState    State        = SessionState::None;

    uint32  DifficultyId    = 0;
    uint32  ThemeId         = 0;
    uint32  MapId           = 0;
    uint32  InstanceId      = 0;
    bool    ScaleToParty    = true;
    uint32  RoguelikeRunId  = 0;  // 0 = standalone, >0 = roguelike

    uint8   EffectiveLevel = 1;
    uint8   LevelBandMin   = 1;
    uint8   LevelBandMax   = 80;

    uint64  StartTime = 0;
    uint64  EndTime   = 0;
    uint32  TimeLimit = 0;

    std::vector<PlayerSessionData>  Players;
    std::vector<SpawnedCreature>    SpawnedCreatures;
    std::vector<SpawnPoint>         SpawnPoints;
    std::vector<PendingPhaseCheck>  PendingPhaseChecks;

    uint32  TotalMobs   = 0;
    uint32  MobsKilled  = 0;
    uint32  TotalBosses = 0;
    uint32  BossesKilled = 0;
    uint32  Wipes       = 0;

    Position EntrancePos;

    bool IsSessionCreature(ObjectGuid guid) const
    {
        for (const auto& sc : SpawnedCreatures)
            if (sc.Guid == guid) return true;
        return false;
    }

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
            if (p.PlayerGuid == guid) return true;
        return false;
    }

    PlayerSessionData* GetPlayerData(ObjectGuid guid)
    {
        for (auto& p : Players)
            if (p.PlayerGuid == guid) return &p;
        return nullptr;
    }

    uint32 GetAlivePlayerCount() const;
    bool   IsPartyWiped() const;
    bool   IsGroupInCombat() const;
};

struct CreaturePoolEntry
{
    uint32 Entry    = 0;
    uint32 Type     = 0;
    uint8  MinLevel = 1;
    uint8  MaxLevel = 80;
};

struct ClassLevelStatEntry
{
    uint32 BaseHP         = 1;
    float  BaseDamage     = 1.0f;
    uint32 BaseArmor      = 0;
    uint32 AttackPower    = 0;
};

struct RewardItem
{
    uint32 Entry         = 0;
    uint32 MinLevel      = 1;
    uint32 MaxLevel      = 80;
    uint16 ItemLevel     = 0;
    uint8  Quality       = 0;       // 0=Poor .. 4=Epic
    uint32 InventoryType = 0;
    uint32 Class         = 0;       // 2=Weapon, 4=Armor
    uint32 SubClass      = 0;
    int32  AllowableClass = -1;
};

struct LootPoolItem
{
    uint32 Entry          = 0;
    uint8  MinLevel       = 0;
    uint16 ItemLevel      = 0;
    uint8  Quality        = 0;
    uint8  ItemClass      = 0;
    uint8  SubClass       = 0;
    int32  AllowableClass = -1;
};

struct PlayerStats
{
    ObjectGuid PlayerGuid;
    uint32 TotalRuns       = 0;
    uint32 CompletedRuns   = 0;
    uint32 FailedRuns      = 0;
    uint32 TotalMobsKilled = 0;
    uint32 TotalBossesKilled = 0;
    uint32 TotalDeaths     = 0;
    uint32 FastestClear    = 0;
};

struct LeaderboardEntry
{
    uint32      Id            = 0;
    uint32      Guid          = 0;
    std::string CharName;
    uint32      MapId         = 0;
    uint32      DifficultyId  = 0;
    uint32      ClearTime     = 0;
    uint8       PartySize     = 1;
    bool        Scaled        = false;
    uint8       EffectiveLevel = 0;
    uint32      MobsKilled    = 0;
    uint32      BossesKilled  = 0;
    uint32      Deaths        = 0;
};

} // namespace DungeonMaster

#endif // DM_TYPES_H
