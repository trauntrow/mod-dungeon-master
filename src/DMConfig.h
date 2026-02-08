/*
 * Copyright (C) 2025 AzerothCore - mod-dungeon-master
 *
 * DMConfig.h — Singleton that reads and caches every setting from the
 *              mod_dungeon_master.conf.dist file.
 *
 * Responsibilities:
 *   - Parse difficulties, themes, and the dungeon list from config.
 *   - Provide read-only accessors for the rest of the module.
 *   - Support live reload via `.dm reload`.
 *
 * Thread safety:
 *   Config is loaded/reloaded on the world thread only (OnAfterConfigLoad).
 *   All accessors return by value or const-reference, so they are safe to
 *   call from the map-update thread that runs the Update() loop.
 */

#ifndef DM_CONFIG_H
#define DM_CONFIG_H

#include "DMTypes.h"
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace DungeonMaster
{

class DMConfig
{
    DMConfig() = default;
    ~DMConfig() = default;

public:
    static DMConfig* Instance();

    void LoadConfig(bool reload = false);

    // --- Core ---
    bool   IsEnabled()        const { return _enabled; }
    bool   IsDebugEnabled()   const { return _debug; }
    uint32 GetNpcEntry()      const { return _npcEntry; }

    // --- Difficulties ---
    const std::vector<DifficultyTier>&      GetDifficulties() const { return _difficulties; }
    const DifficultyTier*                   GetDifficulty(uint32 id) const;
    std::vector<const DifficultyTier*>      GetDifficultiesForLevel(uint8 level) const;

    // --- Themes ---
    const std::vector<Theme>&   GetThemes() const { return _themes; }
    const Theme*                GetTheme(uint32 id) const;

    // --- Dungeons ---
    const std::vector<DungeonInfo>&     GetDungeons() const { return _dungeons; }
    const DungeonInfo*                  GetDungeon(uint32 mapId) const;
    std::vector<const DungeonInfo*>     GetDungeonsForLevel(uint8 minLevel, uint8 maxLevel) const;
    bool                                IsDungeonAllowed(uint32 mapId) const;

    // --- Scaling ---
    uint8 GetLevelBand()          const { return _levelBand; }
    float GetPerPlayerHealthMult() const { return _perPlayerHealth; }
    float GetPerPlayerDamageMult() const { return _perPlayerDamage; }
    float GetSoloMultiplier()     const { return _soloMultiplier; }
    float GetEliteHealthMult()    const { return _eliteHealthMult; }
    float GetEliteDamageMult()    const { return _eliteDamageMult; }
    float GetBossHealthMult()     const { return _bossHealthMult; }
    float GetBossDamageMult()     const { return _bossDamageMult; }

    // --- Rewards ---
    uint32 GetBaseGold()     const { return _baseGold; }
    uint32 GetGoldPerMob()   const { return _goldPerMob; }
    uint32 GetGoldPerBoss()  const { return _goldPerBoss; }
    float  GetXPMultiplier() const { return _xpMultiplier; }
    uint32 GetItemChance()   const { return _itemChance; }
    uint32 GetRareChance()   const { return _rareChance; }
    uint32 GetEpicChance()   const { return _epicChance; }

    // --- Dungeon population ---
    uint32 GetBossCount()       const { return _bossCount; }
    uint32 GetEliteChance()     const { return _eliteChance; }
    float  GetAggroRadius()     const { return _aggroRadius; }

    // --- Timers ---
    uint32 GetCooldownMinutes()   const { return _cooldownMinutes; }
    bool   IsTimeLimitEnabled()   const { return _timeLimitEnabled; }
    uint32 GetTimeLimitMinutes()  const { return _timeLimitMinutes; }
    uint32 GetMaxConcurrentRuns() const { return _maxConcurrentRuns; }

    // --- Death ---
    bool   ShouldRespawnAtStart()  const { return _respawnAtStart; }
    uint32 GetMaxWipes()           const { return _maxWipes; }

    // --- Completion ---
    uint32 GetCompletionTeleportDelay() const { return _completionTeleportDelay; }
    bool   ShouldAnnounceCompletion()   const { return _announceCompletion; }

private:
    void LoadDifficulties();
    void LoadThemes();
    void LoadDungeons();
    void ParseStringList(const std::string& str, std::unordered_set<uint32>& outSet);

    // Core
    bool   _enabled   = true;
    bool   _debug     = false;
    uint32 _npcEntry  = 500000;

    // Data
    std::vector<DifficultyTier>     _difficulties;
    std::vector<Theme>              _themes;
    std::vector<DungeonInfo>        _dungeons;
    std::unordered_set<uint32>      _dungeonWhitelist;
    std::unordered_set<uint32>      _dungeonBlacklist;

    // Scaling
    uint8 _levelBand       = 3;     // creatures must be within ±N levels
    float _perPlayerHealth = 0.25f;
    float _perPlayerDamage = 0.10f;
    float _soloMultiplier  = 0.50f;
    float _eliteHealthMult = 2.0f;
    float _eliteDamageMult = 1.5f;
    float _bossHealthMult  = 5.0f;
    float _bossDamageMult  = 2.0f;

    // Rewards
    uint32 _baseGold    = 50000;
    uint32 _goldPerMob  = 50;
    uint32 _goldPerBoss = 10000;
    float  _xpMultiplier = 1.0f;
    uint32 _itemChance  = 80;
    uint32 _rareChance  = 40;
    uint32 _epicChance  = 15;

    // Dungeon
    uint32 _bossCount    = 1;
    uint32 _eliteChance  = 20;
    float  _aggroRadius  = 15.0f;

    // Timers
    uint32 _cooldownMinutes   = 5;
    bool   _timeLimitEnabled  = false;
    uint32 _timeLimitMinutes  = 30;
    uint32 _maxConcurrentRuns = 20;

    // Death
    bool   _respawnAtStart = true;
    uint32 _maxWipes       = 0;

    // Completion
    uint32 _completionTeleportDelay = 30;
    bool   _announceCompletion      = true;
};

} // namespace DungeonMaster

#define sDMConfig DungeonMaster::DMConfig::Instance()

#endif // DM_CONFIG_H
