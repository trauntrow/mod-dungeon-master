/*
 * mod-dungeon-master — Roguelike subsystem types
 * Copyright (C) 2025 — AGPL v3
 */

#ifndef ROGUELIKE_TYPES_H
#define ROGUELIKE_TYPES_H

#include "Define.h"
#include "ObjectGuid.h"
#include "Position.h"
#include <string>
#include <vector>

namespace DungeonMaster
{

constexpr uint32 MAX_ROGUELIKE_BUFFS   = 30;
constexpr uint32 MAX_ROGUELIKE_AFFIXES = 10;
constexpr float ROGUELIKE_BUFF_PCT_PER_STACK = 10.0f;

enum class RoguelikeRunState : uint8
{
    None = 0,
    Active,
    Ended
};

enum RoguelikeAffix : uint32
{
    AFFIX_NONE        = 0,
    AFFIX_FORTIFIED   = 1,   // trash: +30% HP, +15% dmg
    AFFIX_TYRANNICAL  = 2,   // bosses: +40% HP, +20% dmg
    AFFIX_RAGING      = 3,   // all: +25% dmg
    AFFIX_BOLSTERING  = 4,   // all: +20% HP
    AFFIX_SAVAGE      = 5,   // double elite chance
    AFFIX_COUNT       = 6
};

struct RoguelikeBuff
{
    uint32      Id       = 0;
    uint32      SpellId  = 0;
    std::string Name;
    uint32      Weight   = 100;
};

struct AffixDef
{
    RoguelikeAffix  Id            = AFFIX_NONE;
    std::string     Name;
    float           TrashHpMult   = 1.0f;
    float           TrashDmgMult  = 1.0f;
    float           BossHpMult    = 1.0f;
    float           BossDmgMult   = 1.0f;
    float           EliteChanceMult = 1.0f;
};

struct RoguelikePlayerData
{
    ObjectGuid  PlayerGuid;
    Position    OriginalPosition;
    uint32      OriginalMapId = 0;
};

struct RoguelikeRun
{
    uint32              RunId            = 0;
    ObjectGuid          LeaderGuid;
    RoguelikeRunState   State            = RoguelikeRunState::None;

    uint32  ThemeId              = 0;       // 0 = random each floor
    uint32  BaseDifficultyId     = 0;
    bool    ScaleToParty         = true;

    uint32  CurrentTier          = 1;
    uint32  CurrentSessionId     = 0;
    uint32  DungeonsCleared      = 0;
    uint32  PreviousMapId        = 0;

    uint32  BuffStacks = 0;                 // +10% all stats per stack (BoK aura with visual stacks)

    std::vector<RoguelikeAffix> ActiveAffixes;
    std::vector<RoguelikePlayerData> Players;

    uint64  RunStartTime         = 0;
    uint64  TransitionStartTime  = 0;       // grace window for async teleport
    uint32  LastCountdownAnnounce = 999;

    uint32  TotalMobsKilled      = 0;
    uint32  TotalBossesKilled    = 0;
    uint32  TotalDeaths          = 0;

    bool IsActive() const { return State == RoguelikeRunState::Active; }

    bool HasPlayer(ObjectGuid guid) const
    {
        for (const auto& p : Players)
            if (p.PlayerGuid == guid) return true;
        return false;
    }

    RoguelikePlayerData* GetPlayerData(ObjectGuid guid)
    {
        for (auto& p : Players)
            if (p.PlayerGuid == guid) return &p;
        return nullptr;
    }
};

struct RoguelikeLeaderboardEntry
{
    uint32      Id           = 0;
    uint32      Guid         = 0;
    std::string CharName;
    uint32      TierReached  = 0;
    uint32      DungeonsCleared = 0;
    uint32      TotalKills   = 0;
    uint32      RunDuration  = 0;
    uint8       PartySize    = 1;
};

} // namespace DungeonMaster

#endif
