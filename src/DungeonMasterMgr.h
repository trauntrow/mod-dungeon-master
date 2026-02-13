/*
 * mod-dungeon-master — DungeonMasterMgr.h
 * Central session manager singleton.
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

    void Initialize();
    void LoadFromDB();

    // Session lifecycle
    Session*  CreateSession(Player* leader, uint32 difficultyId, uint32 themeId, uint32 mapId, bool scaleToParty = true);
    Session*  GetSession(uint32 sessionId);
    Session*  GetSessionByInstance(uint32 instanceId);
    Session*  GetSessionByPlayer(ObjectGuid playerGuid);
    void      EndSession(uint32 sessionId, bool success);
    void      AbandonSession(uint32 sessionId);
    void      CleanupRoguelikeSession(uint32 sessionId, bool success);

    // Session ops
    bool StartDungeon(Session* session);
    bool TeleportPartyIn(Session* session);
    void TeleportPartyOut(Session* session);
    void HandlePlayerDeath(Player* player, Session* session);
    void HandleCreatureDeath(Creature* creature, Session* session);
    void HandleBossDeath(Session* session);
    void OnCreatureDeathHook(Creature* creature);

    // Dungeon population
    void ClearDungeonCreatures(InstanceMap* map);
    void OpenAllDoors(InstanceMap* map);
    void PopulateDungeon(Session* session, InstanceMap* map);

    // Rewards
    void DistributeRewards(Session* session);
    void FillCreatureLoot(Creature* creature, Session* session, bool isBoss);

    // Cooldowns
    bool   IsOnCooldown(ObjectGuid playerGuid) const;
    void   SetCooldown(ObjectGuid playerGuid);
    void   ClearCooldown(ObjectGuid playerGuid);
    uint32 GetRemainingCooldown(ObjectGuid playerGuid) const;

    // Stats & leaderboard
    PlayerStats GetPlayerStats(ObjectGuid guid) const;
    void        LoadAllPlayerStats();
    void        SavePlayerStats(uint32 guidLow);
    void        UpdatePlayerStatsFromSession(const Session& session, bool success);
    void        SaveLeaderboardEntry(const Session& session);
    std::vector<LeaderboardEntry> GetLeaderboard(uint32 mapId, uint32 difficultyId, uint32 limit = 10) const;
    std::vector<LeaderboardEntry> GetOverallLeaderboard(uint32 limit = 10) const;

    void Update(uint32 diff);

    uint32 GetActiveSessionCount() const { return static_cast<uint32>(_activeSessions.size()); }
    bool   CanCreateNewSession()   const;

    // Env damage scaling
    bool  IsSessionCreature(ObjectGuid playerGuid, ObjectGuid creatureGuid);
    bool  IsSessionBoss(ObjectGuid playerGuid, ObjectGuid creatureGuid);
    float GetEnvironmentalDamageScale(ObjectGuid playerGuid);
    float GetSessionCreatureDamageScale(ObjectGuid playerGuid, ObjectGuid creatureGuid);

    Position    GetDungeonEntrance(uint32 mapId);
    std::string GetSessionStatusString(const Session* session) const;
    uint8       ComputeEffectiveLevel(Player* leader) const;

    void   DistributeRoguelikeRewards(uint32 tier, uint8 effectiveLevel,
                                       const std::vector<ObjectGuid>& playerGuids);

private:
    std::vector<SpawnPoint> GetSpawnPointsForMap(uint32 mapId);
    uint32 SelectCreatureForTheme(const Theme* theme, bool isBoss);
    uint32 SelectDungeonBoss(const Theme* theme);

    void   GiveGoldReward(Player* player, uint32 amount);
    void   GiveItemReward(Player* player, uint8 rewardLevel, uint8 quality);
    void   MailItemReward(Player* player, uint8 level, uint8 quality,
                          const std::string& subject, const std::string& body);
    void   GiveKillXP(Session* session, bool isBoss, bool isElite);
    uint32 SelectRewardItem(uint8 level, uint8 quality, uint32 playerClass);
    uint32 SelectLootItem(uint8 level, uint8 minQuality, uint8 maxQuality, bool equipmentOnly = false, uint32 playerClass = 0);

    float CalculateHealthMultiplier(const Session* session) const;
    float CalculateDamageMultiplier(const Session* session) const;
    const ClassLevelStatEntry* GetBaseStatsForLevel(uint8 unitClass, uint8 level) const;

    void LoadCreaturePools();
    void LoadDungeonBossPool();
    void LoadClassLevelStats();
    void LoadRewardItems();
    void LoadLootPool();
    void CleanupSession(Session& session);

    std::unordered_map<uint32, Session>      _activeSessions;
    std::unordered_map<uint32, uint32>       _instanceToSession;
    std::unordered_map<ObjectGuid, uint32>   _playerToSession;
    uint32 _nextSessionId = 1;
    mutable std::mutex _sessionMutex;

    std::unordered_map<ObjectGuid, uint64>   _cooldowns;
    mutable std::mutex _cooldownMutex;

    std::unordered_map<uint32, PlayerStats>  _playerStats;
    mutable std::mutex _statsMutex;

    std::unordered_map<uint32, std::vector<CreaturePoolEntry>> _creaturesByType;
    std::unordered_map<uint32, std::vector<CreaturePoolEntry>> _bossCreatures;
    std::unordered_map<uint32, std::vector<CreaturePoolEntry>> _dungeonBossPool;

    std::map<std::pair<uint8,uint8>, ClassLevelStatEntry> _classLevelStats;
    std::unordered_map<uint32, std::vector<ObjectGuid>> _instanceCreatureGuids;

    std::vector<RewardItem> _rewardItems;
    std::vector<LootPoolItem> _lootPool;

    uint32 _updateTimer = 0;
    static constexpr uint32 UPDATE_INTERVAL = 1000;
};

} // namespace DungeonMaster

#define sDungeonMasterMgr DungeonMaster::DungeonMasterMgr::Instance()

#endif
