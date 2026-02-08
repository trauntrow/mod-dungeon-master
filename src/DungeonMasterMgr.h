/*
 * Copyright (C) 2025 AzerothCore - mod-dungeon-master
 *
 * DungeonMasterMgr.h — Central singleton that owns all session state
 *                       and orchestrates dungeon lifecycle.
 *
 * Architecture overview:
 *
 *   1. NPC gossip (npc_dungeon_master) collects player choices and
 *      calls CreateSession() → StartDungeon() → TeleportPartyIn().
 *
 *   2. When the leader enters the instance map (dm_allmap_script),
 *      PopulateDungeon() is invoked to:
 *        a) Clear original creatures.
 *        b) Open all doors.
 *        c) Select themed creatures whose level falls within the
 *           *player-derived* level band (effective_level ± LEVEL_BAND).
 *        d) Summon them at the map's original spawn points.
 *        e) Apply difficulty and elite/boss multipliers.
 *
 *   3. The Update() tick (every 1 s) polls creature deaths, auto-
 *      resurrects out-of-combat, checks time limits, and detects
 *      abandoned / completed sessions.
 *
 *   4. EndSession() distributes rewards, teleports players home,
 *      and sets cooldowns.
 *
 * Thread safety:
 *   _sessionMutex protects all session containers.
 *   _cooldownMutex protects the cooldown map.
 *   Both are acquired with std::lock_guard and released automatically.
 */

#ifndef DUNGEON_MASTER_MGR_H
#define DUNGEON_MASTER_MGR_H

#include "DMTypes.h"
#include "DMConfig.h"
#include <mutex>
#include <map>
#include <unordered_map>

class Player;
class Group;
class Creature;
class Map;
class InstanceMap;

namespace DungeonMaster
{

class DungeonMasterMgr
{
    DungeonMasterMgr();
    ~DungeonMasterMgr();

public:
    static DungeonMasterMgr* Instance();

    // --- Initialization (called once on startup) ---
    void Initialize();
    void LoadFromDB();

    // --- Session lifecycle ---
    Session*  CreateSession(Player* leader, uint32 difficultyId, uint32 themeId, uint32 mapId, bool scaleToParty = true);
    Session*  GetSession(uint32 sessionId);
    Session*  GetSessionByInstance(uint32 instanceId);
    Session*  GetSessionByPlayer(ObjectGuid playerGuid);
    void      EndSession(uint32 sessionId, bool success);
    void      AbandonSession(uint32 sessionId);

    // --- Session operations ---
    bool StartDungeon(Session* session);
    bool TeleportPartyIn(Session* session);
    void TeleportPartyOut(Session* session);
    void HandlePlayerDeath(Player* player, Session* session);
    void HandleCreatureDeath(Creature* creature, Session* session);
    void HandleBossDeath(Session* session);

    // --- Dungeon population ---
    void ClearDungeonCreatures(InstanceMap* map);
    void OpenAllDoors(InstanceMap* map);
    void PopulateDungeon(Session* session, InstanceMap* map);

    // --- Rewards ---
    void DistributeRewards(Session* session);
    void FillCreatureLoot(Creature* creature, Session* session, bool isBoss);

    // --- Cooldowns ---
    bool   IsOnCooldown(ObjectGuid playerGuid) const;
    void   SetCooldown(ObjectGuid playerGuid);
    void   ClearCooldown(ObjectGuid playerGuid);
    uint32 GetRemainingCooldown(ObjectGuid playerGuid) const;

    // --- Stats & Leaderboard ---
    PlayerStats GetPlayerStats(ObjectGuid guid) const;
    void        LoadAllPlayerStats();
    void        SavePlayerStats(uint32 guidLow);
    void        UpdatePlayerStatsFromSession(const Session& session, bool success);
    void        SaveLeaderboardEntry(const Session& session);
    std::vector<LeaderboardEntry> GetLeaderboard(uint32 mapId, uint32 difficultyId, uint32 limit = 10) const;
    std::vector<LeaderboardEntry> GetOverallLeaderboard(uint32 limit = 10) const;

    // --- Tick ---
    void Update(uint32 diff);

    // --- Accessors ---
    uint32 GetActiveSessionCount() const { return static_cast<uint32>(_activeSessions.size()); }
    bool   CanCreateNewSession()   const;

    // --- Utility ---
    Position    GetDungeonEntrance(uint32 mapId);
    std::string GetSessionStatusString(const Session* session) const;
    uint8       ComputeEffectiveLevel(Player* leader) const;

private:
    // Creature selection
    std::vector<SpawnPoint> GetSpawnPointsForMap(uint32 mapId);
    uint32 SelectCreatureForTheme(const Theme* theme, uint8 bandMin, uint8 bandMax, bool isBoss);

    // Reward helpers
    void   GiveGoldReward(Player* player, uint32 amount);
    void   GiveItemReward(Player* player, uint8 rewardLevel, uint8 quality);
    void   GiveKillXP(Session* session, bool isBoss, bool isElite);
    uint32 SelectRewardItem(uint8 level, uint8 quality, uint32 playerClass);
    uint32 SelectLootItem(uint8 level, uint8 minQuality, uint8 maxQuality, bool equipmentOnly = false, uint32 playerClass = 0);

    // Scaling helpers
    float CalculateHealthMultiplier(const Session* session) const;
    float CalculateDamageMultiplier(const Session* session) const;
    const ClassLevelStatEntry* GetBaseStatsForLevel(uint8 unitClass, uint8 level) const;

    // Internal
    void LoadCreaturePools();
    void LoadClassLevelStats();
    void LoadRewardItems();
    void LoadLootPool();
    void CleanupSession(Session& session);

    // --- Data ---
    std::unordered_map<uint32, Session>      _activeSessions;
    std::unordered_map<uint32, uint32>       _instanceToSession;   // instanceId → sessionId
    std::unordered_map<ObjectGuid, uint32>   _playerToSession;     // guid → sessionId
    uint32 _nextSessionId = 1;
    mutable std::mutex _sessionMutex;

    std::unordered_map<ObjectGuid, uint64>   _cooldowns;           // guid → expiry ts
    mutable std::mutex _cooldownMutex;

    // Persistent player statistics (loaded from characters DB)
    std::unordered_map<uint32, PlayerStats>  _playerStats;         // guidLow → stats
    mutable std::mutex _statsMutex;

    // Creature pools (loaded from world DB once)
    std::unordered_map<uint32, std::vector<CreaturePoolEntry>> _creaturesByType;  // type → entries
    std::unordered_map<uint32, std::vector<CreaturePoolEntry>> _bossCreatures;    // 10-lvl bracket → entries

    // Base stats per (unitClass, level) from creature_classlevelstats
    std::map<std::pair<uint8,uint8>, ClassLevelStatEntry> _classLevelStats;

    // Track summoned creature GUIDs per instance so we can despawn them later
    std::unordered_map<uint32, std::vector<ObjectGuid>> _instanceCreatureGuids;

    // Reward pool
    std::vector<RewardItem> _rewardItems;

    // Mob loot pool (grey/white/green/blue items for creature drops)
    std::vector<LootPoolItem> _lootPool;

    // Tick
    uint32 _updateTimer = 0;
    static constexpr uint32 UPDATE_INTERVAL = 1000; // ms
};

} // namespace DungeonMaster

#define sDungeonMasterMgr DungeonMaster::DungeonMasterMgr::Instance()

#endif // DUNGEON_MASTER_MGR_H
