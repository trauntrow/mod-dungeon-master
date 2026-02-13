/*
 * mod-dungeon-master — RoguelikeMgr.cpp
 * Roguelike run manager: multi-dungeon progression, buffs, affixes, transitions.
 */

#include "RoguelikeMgr.h"
#include "DungeonMasterMgr.h"
#include "DMConfig.h"
#include "Player.h"
#include "Group.h"
#include "Creature.h"
#include "ObjectAccessor.h"
#include "Chat.h"
#include "GameTime.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "SpellAuras.h"
#include "SpellAuraEffects.h"
#include <random>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace DungeonMaster
{

// RNG helpers (thread-local for safety)
static thread_local std::mt19937 tRng{ std::random_device{}() };

template<typename T>
static T RandInt(T lo, T hi)
{
    return std::uniform_int_distribution<T>(lo, hi)(tRng);
}

// Singleton
RoguelikeMgr::RoguelikeMgr()  = default;
RoguelikeMgr::~RoguelikeMgr() = default;

RoguelikeMgr* RoguelikeMgr::Instance()
{
    static RoguelikeMgr inst;
    return &inst;
}

// Initialization
void RoguelikeMgr::Initialize()
{
    BuildAffixPool();
    LOG_INFO("module", "RoguelikeMgr: Initialized — {} affix definitions, {} buff pool entries.",
        _affixDefs.size(), sDMConfig->GetRoguelikeBuffPool().size());
}

void RoguelikeMgr::BuildAffixPool()
{
    _affixDefs.clear();

    // Fortified — trash mobs are significantly harder
    {
        AffixDef a;
        a.Id           = AFFIX_FORTIFIED;
        a.Name         = "Fortified";
        a.TrashHpMult  = 1.30f;
        a.TrashDmgMult = 1.15f;
        a.BossHpMult   = 1.0f;
        a.BossDmgMult  = 1.0f;
        _affixDefs.push_back(a);
    }
    // Tyrannical — bosses are significantly harder
    {
        AffixDef a;
        a.Id           = AFFIX_TYRANNICAL;
        a.Name         = "Tyrannical";
        a.TrashHpMult  = 1.0f;
        a.TrashDmgMult = 1.0f;
        a.BossHpMult   = 1.40f;
        a.BossDmgMult  = 1.20f;
        _affixDefs.push_back(a);
    }
    // Raging — everything hits harder
    {
        AffixDef a;
        a.Id           = AFFIX_RAGING;
        a.Name         = "Raging";
        a.TrashHpMult  = 1.0f;
        a.TrashDmgMult = 1.25f;
        a.BossHpMult   = 1.0f;
        a.BossDmgMult  = 1.25f;
        _affixDefs.push_back(a);
    }
    // Bolstering — everything has more health
    {
        AffixDef a;
        a.Id           = AFFIX_BOLSTERING;
        a.Name         = "Bolstering";
        a.TrashHpMult  = 1.20f;
        a.TrashDmgMult = 1.0f;
        a.BossHpMult   = 1.20f;
        a.BossDmgMult  = 1.0f;
        _affixDefs.push_back(a);
    }
    // Savage — more elites, elites are nastier
    {
        AffixDef a;
        a.Id              = AFFIX_SAVAGE;
        a.Name            = "Savage";
        a.TrashHpMult     = 1.0f;
        a.TrashDmgMult    = 1.10f;
        a.BossHpMult      = 1.0f;
        a.BossDmgMult     = 1.0f;
        a.EliteChanceMult = 2.0f;
        _affixDefs.push_back(a);
    }
}

// RUN LIFECYCLE

bool RoguelikeMgr::StartRun(Player* leader, uint32 difficultyId, uint32 themeId, bool scaleToParty)
{
    if (!leader)
        return false;

    if (!sDMConfig->IsRoguelikeEnabled())
    {
        ChatHandler(leader->GetSession()).SendSysMessage(
            "|cFFFF0000[Roguelike]|r Roguelike mode is disabled.");
        return false;
    }

    // Check: player not already in a run
    if (IsPlayerInRun(leader->GetGUID()))
    {
        ChatHandler(leader->GetSession()).SendSysMessage(
            "|cFFFF0000[Roguelike]|r You are already in a roguelike run!");
        return false;
    }

    // Check: player not in a DM session
    if (sDungeonMasterMgr->GetSessionByPlayer(leader->GetGUID()))
    {
        ChatHandler(leader->GetSession()).SendSysMessage(
            "|cFFFF0000[Roguelike]|r You are in an active dungeon challenge!");
        return false;
    }

    // Check: cooldown
    if (sDungeonMasterMgr->IsOnCooldown(leader->GetGUID()))
    {
        uint32 rem = sDungeonMasterMgr->GetRemainingCooldown(leader->GetGUID());
        char buf[256];
        snprintf(buf, sizeof(buf),
            "|cFFFF0000[Roguelike]|r Wait |cFFFFFFFF%u|r min |cFFFFFFFF%u|r sec before starting.",
            rem / 60, rem % 60);
        ChatHandler(leader->GetSession()).SendSysMessage(buf);
        return false;
    }

    // Check: can create a session
    if (!sDungeonMasterMgr->CanCreateNewSession())
    {
        ChatHandler(leader->GetSession()).SendSysMessage(
            "|cFFFF0000[Roguelike]|r Too many active challenges. Try again later.");
        return false;
    }

    // Build the run
    RoguelikeRun run;
    {
        std::lock_guard<std::mutex> lock(_runMutex);
        run.RunId          = _nextRunId++;
    }
    run.LeaderGuid     = leader->GetGUID();
    run.State          = RoguelikeRunState::Active;
    run.ThemeId        = themeId;
    run.ScaleToParty   = scaleToParty;
    run.CurrentTier    = 1;
    run.RunStartTime   = GameTime::GetGameTime().count();

    // Use the difficulty selected by the player (or fall back to first available)
    run.BaseDifficultyId = difficultyId;
    const DifficultyTier* diff = sDMConfig->GetDifficulty(difficultyId);
    if (!diff)
    {
        const auto& diffs = sDMConfig->GetDifficulties();
        run.BaseDifficultyId = diffs.empty() ? 1 : diffs[0].Id;
    }

    // Store original positions for ALL party members
    RoguelikePlayerData ld;
    ld.PlayerGuid       = leader->GetGUID();
    ld.OriginalMapId    = leader->GetMapId();
    ld.OriginalPosition = { leader->GetPositionX(), leader->GetPositionY(),
                            leader->GetPositionZ(), leader->GetOrientation() };
    run.Players.push_back(ld);

    if (Group* g = leader->GetGroup())
    {
        for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
        {
            Player* m = ref->GetSource();
            if (m && m != leader && m->IsInWorld())
            {
                // Check: group member not in another run
                if (IsPlayerInRun(m->GetGUID()))
                    continue;
                if (sDungeonMasterMgr->GetSessionByPlayer(m->GetGUID()))
                    continue;

                RoguelikePlayerData md;
                md.PlayerGuid       = m->GetGUID();
                md.OriginalMapId    = m->GetMapId();
                md.OriginalPosition = { m->GetPositionX(), m->GetPositionY(),
                                        m->GetPositionZ(), m->GetOrientation() };
                run.Players.push_back(md);
            }
        }
    }

    // Select the first dungeon
    uint32 mapId = SelectRandomDungeon(run);
    if (!mapId)
    {
        ChatHandler(leader->GetSession()).SendSysMessage(
            "|cFFFF0000[Roguelike]|r No dungeons available for your level!");
        return false;
    }

    // Clear cooldowns for all party members so they can enter
    for (const auto& pd : run.Players)
        sDungeonMasterMgr->ClearCooldown(pd.PlayerGuid);

    // Create the DM session with the player's scaling choice
    Session* session = sDungeonMasterMgr->CreateSession(
        leader, run.BaseDifficultyId, themeId, mapId, run.ScaleToParty);
    if (!session)
    {
        ChatHandler(leader->GetSession()).SendSysMessage(
            "|cFFFF0000[Roguelike]|r Failed to create dungeon session!");
        return false;
    }

    // Tag the session as roguelike
    session->RoguelikeRunId = run.RunId;
    run.CurrentSessionId    = session->SessionId;

    // Start the dungeon
    if (!sDungeonMasterMgr->StartDungeon(session))
    {
        ChatHandler(leader->GetSession()).SendSysMessage(
            "|cFFFF0000[Roguelike]|r Failed to initialize dungeon!");
        sDungeonMasterMgr->CleanupRoguelikeSession(session->SessionId, false);
        return false;
    }

    if (!sDungeonMasterMgr->TeleportPartyIn(session))
    {
        ChatHandler(leader->GetSession()).SendSysMessage(
            "|cFFFF0000[Roguelike]|r Teleport failed!");
        sDungeonMasterMgr->CleanupRoguelikeSession(session->SessionId, false);
        return false;
    }

    // No buff on tier 1 — first +10% earned after clearing floor 1
    run.BuffStacks = 0;

    // Grace period for async teleport
    run.TransitionStartTime = GameTime::GetGameTime().count();

    // Select affixes for tier 1 (may be none if affix start tier > 1)
    SelectAffixesForTier(run);

    // Register the run
    {
        std::lock_guard<std::mutex> lock(_runMutex);
        _activeRuns[run.RunId] = run;
        _sessionToRun[run.CurrentSessionId] = run.RunId;
        for (const auto& pd : run.Players)
            _playerToRun[pd.PlayerGuid] = run.RunId;
    }

    // Announce
    const Theme* theme = sDMConfig->GetTheme(themeId);
    char buf[256];
    snprintf(buf, sizeof(buf),
        "|cFF00FFFF[Roguelike]|r |cFFFFD700%s|r started a Roguelike Run! "
        "Theme: |cFF00FF00%s|r — How far can you go?",
        leader->GetName().c_str(),
        theme ? theme->Name.c_str() : "Random");

    for (const auto& pd : run.Players)
        if (Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid))
            ChatHandler(p->GetSession()).SendSysMessage(buf);

    // Announce active affixes if any are present at tier 1
    if (!run.ActiveAffixes.empty())
    {
        std::string affixNames = GetActiveAffixNames(run.RunId);
        if (!affixNames.empty())
        {
            char affixBuf[512];
            snprintf(affixBuf, sizeof(affixBuf),
                "|cFF00FFFF[Roguelike]|r Active affixes: %s",
                affixNames.c_str());
            for (const auto& pd : run.Players)
                if (Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid))
                    ChatHandler(p->GetSession()).SendSysMessage(affixBuf);
        }
    }

    LOG_INFO("module", "RoguelikeMgr: Run {} started — leader {}, party {}, theme {}, map {}",
        run.RunId, leader->GetName(), run.Players.size(),
        theme ? theme->Name.c_str() : "Random", mapId);

    return true;
}


void RoguelikeMgr::OnDungeonCompleted(uint32 runId, uint32 sessionId)
{
    RoguelikeRun* run = nullptr;
    {
        std::lock_guard<std::mutex> lock(_runMutex);
        auto it = _activeRuns.find(runId);
        if (it == _activeRuns.end()) return;
        run = &it->second;
    }

    if (run->CurrentSessionId != sessionId)
    {
        LOG_WARN("module", "RoguelikeMgr: OnDungeonCompleted — session {} != current {}",
            sessionId, run->CurrentSessionId);
        return;
    }

    // Copy data before cleanup invalidates the session
    uint32 sessionMobsKilled   = 0;
    uint32 sessionBossesKilled = 0;
    uint32 sessionDeaths       = 0;
    uint32 sessionMapId        = 0;
    {
        Session* session = sDungeonMasterMgr->GetSession(sessionId);
        if (session)
        {
            sessionMobsKilled   = session->MobsKilled;
            sessionBossesKilled = session->BossesKilled;
            sessionMapId        = session->MapId;
            for (const auto& pd : session->Players)
                sessionDeaths += pd.Deaths;

            // Distribute per-floor rewards while session pointer is still valid
            sDungeonMasterMgr->DistributeRewards(session);
        }
    }

    // Accumulate stats
    run->TotalMobsKilled   += sessionMobsKilled;
    run->TotalBossesKilled += sessionBossesKilled;
    run->TotalDeaths       += sessionDeaths;

    ++run->DungeonsCleared;
    run->PreviousMapId = sessionMapId;

    // Clean up the DM session (no teleport, no cooldown)
    sDungeonMasterMgr->CleanupRoguelikeSession(sessionId, true);

    // Remove old session mapping
    {
        std::lock_guard<std::mutex> lock(_runMutex);
        _sessionToRun.erase(sessionId);
    }

    // Increment tier
    ++run->CurrentTier;

    // Select new affixes
    SelectAffixesForTier(*run);

    // Apply a new buff stack (+10% all stats)
    IncrementBuffStacks(run->RunId);

    // Announce progress
    char buf[512];
    snprintf(buf, sizeof(buf),
        "|cFF00FFFF[Roguelike]|r |cFFFFD700Floor %u cleared!|r "
        "Advancing to |cFFFF0000Tier %u|r...",
        run->DungeonsCleared, run->CurrentTier);

    // Append active affixes
    std::string affixStr;
    for (RoguelikeAffix afxId : run->ActiveAffixes)
    {
        for (const auto& def : _affixDefs)
        {
            if (def.Id == afxId)
            {
                if (!affixStr.empty()) affixStr += ", ";
                affixStr += "|cFFFF8800" + def.Name + "|r";
                break;
            }
        }
    }
    if (!affixStr.empty())
    {
        size_t len = strlen(buf);
        snprintf(buf + len, sizeof(buf) - len, " Affixes: %s", affixStr.c_str());
    }

    AnnounceToRun(*run, buf);

    // Grace period for abandoned detection
    run->TransitionStartTime = GameTime::GetGameTime().count();

    // Transition to the next dungeon
    if (!TransitionToNextDungeon(*run))
    {
        // Failed to create next dungeon — end the run gracefully
        char failBuf[256];
        snprintf(failBuf, sizeof(failBuf),
            "|cFFFF0000[Roguelike]|r No more dungeons available! "
            "Run ended at |cFFFFD700Tier %u|r after |cFFFFFFFF%u|r floors.",
            run->CurrentTier, run->DungeonsCleared);
        AnnounceToRun(*run, failBuf);
        EndRun(run->RunId, true);
    }
}

// Handle party wipe
void RoguelikeMgr::OnPartyWipe(uint32 runId)
{
    RoguelikeRun* run = nullptr;
    {
        std::lock_guard<std::mutex> lock(_runMutex);
        auto it = _activeRuns.find(runId);
        if (it == _activeRuns.end()) return;
        run = &it->second;
    }

    // Accumulate stats from the final session
    Session* session = sDungeonMasterMgr->GetSession(run->CurrentSessionId);
    if (session)
    {
        run->TotalMobsKilled   += session->MobsKilled;
        run->TotalBossesKilled += session->BossesKilled;
        for (const auto& pd : session->Players)
            run->TotalDeaths += pd.Deaths;
    }

    // Announce the wipe
    uint32 duration = static_cast<uint32>(
        GameTime::GetGameTime().count() - run->RunStartTime);
    uint32 dm = duration / 60, ds = duration % 60;

    char buf[512];
    snprintf(buf, sizeof(buf),
        "|cFFFF0000[Roguelike]|r |cFFFF4444TOTAL PARTY WIPE!|r "
        "Your run has ended.\n"
        "|cFF00FFFF[Roguelike]|r Final Results:\n"
        "  Tier Reached: |cFFFFD700%u|r\n"
        "  Floors Cleared: |cFFFFFFFF%u|r\n"
        "  Mobs Killed: |cFFFFFFFF%u|r\n"
        "  Bosses Slain: |cFFFFFFFF%u|r\n"
        "  Total Deaths: |cFFFF0000%u|r\n"
        "  Run Duration: |cFF00FFFF%um %02us|r",
        run->CurrentTier, run->DungeonsCleared,
        run->TotalMobsKilled, run->TotalBossesKilled,
        run->TotalDeaths, dm, ds);

    // Resurrect dead players
    for (const auto& pd : run->Players)
    {
        Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid);
        if (!p || !p->IsInWorld()) continue;
        p->RemoveFlag(PLAYER_FIELD_BYTES, PLAYER_FIELD_BYTE_NO_RELEASE_WINDOW);
        if (!p->IsAlive())
        {
            p->ResurrectPlayer(1.0f);
            p->SpawnCorpseBones();
        }
    }

    AnnounceToRun(*run, buf);

    // Save leaderboard
    SaveRoguelikeLeaderboard(*run);

    // Remove all buff stacks (skip mid-teleport players)
    for (const auto& pd : run->Players)
    {
        Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid);
        if (p && p->IsInWorld())
            RemoveBuffStacks(p, run->RunId);
    }

    // Clean up the DM session
    if (run->CurrentSessionId != 0)
        sDungeonMasterMgr->CleanupRoguelikeSession(run->CurrentSessionId, false);

    // Teleport everyone back to their original positions
    TeleportRunPlayersOut(*run);

    // Set cooldowns
    for (const auto& pd : run->Players)
        sDungeonMasterMgr->SetCooldown(pd.PlayerGuid);

    // Save before erase invalidates the pointer
    uint32 savedTier    = run->CurrentTier;
    uint32 savedCleared = run->DungeonsCleared;
    uint32 savedSessId  = run->CurrentSessionId;

    // Clean up run
    {
        std::lock_guard<std::mutex> lock(_runMutex);
        _sessionToRun.erase(savedSessId);
        for (const auto& pd : run->Players)
            _playerToRun.erase(pd.PlayerGuid);
        _activeRuns.erase(runId);
    }


    LOG_INFO("module", "RoguelikeMgr: Run {} ended (wipe) — tier {}, {} floors cleared.",
        runId, savedTier, savedCleared);
}

// End run gracefully (voluntary exit or no dungeons left)
void RoguelikeMgr::EndRun(uint32 runId, bool announceResults)
{
    RoguelikeRun* run = nullptr;
    {
        std::lock_guard<std::mutex> lock(_runMutex);
        auto it = _activeRuns.find(runId);
        if (it == _activeRuns.end()) return;
        run = &it->second;
    }

    if (announceResults)
    {
        uint32 duration = static_cast<uint32>(
            GameTime::GetGameTime().count() - run->RunStartTime);
        uint32 dm = duration / 60, ds = duration % 60;

        char buf[512];
        snprintf(buf, sizeof(buf),
            "|cFF00FFFF[Roguelike]|r Run complete!\n"
            "  Tier Reached: |cFFFFD700%u|r\n"
            "  Floors Cleared: |cFFFFFFFF%u|r\n"
            "  Mobs Killed: |cFFFFFFFF%u|r\n"
            "  Bosses Slain: |cFFFFFFFF%u|r\n"
            "  Run Duration: |cFF00FFFF%um %02us|r",
            run->CurrentTier, run->DungeonsCleared,
            run->TotalMobsKilled, run->TotalBossesKilled,
            dm, ds);
        AnnounceToRun(*run, buf);

        SaveRoguelikeLeaderboard(*run);
    }

    // Resurrect all dead players (skip mid-teleport players)
    for (const auto& pd : run->Players)
    {
        Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid);
        if (!p || !p->IsInWorld()) continue;
        p->RemoveFlag(PLAYER_FIELD_BYTES, PLAYER_FIELD_BYTE_NO_RELEASE_WINDOW);
        if (!p->IsAlive())
        {
            p->ResurrectPlayer(1.0f);
            p->SpawnCorpseBones();
        }
    }

    // Remove buff stacks (skip mid-teleport players)
    for (const auto& pd : run->Players)
        if (Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid))
            if (p->IsInWorld())
                RemoveBuffStacks(p, run->RunId);

    // Distribute roguelike rewards (scaled by tier)
    if (run->DungeonsCleared > 0)
    {
        // Compute effective level from the leader (or first available player)
        uint8 effectiveLevel = 1;
        for (const auto& pd : run->Players)
        {
            Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid);
            if (p) { effectiveLevel = p->GetLevel(); break; }
        }

        std::vector<ObjectGuid> guids;
        for (const auto& pd : run->Players)
            guids.push_back(pd.PlayerGuid);

        sDungeonMasterMgr->DistributeRoguelikeRewards(
            run->CurrentTier, effectiveLevel, guids);
    }

    // Clean up DM session if one is active
    if (run->CurrentSessionId != 0)
        sDungeonMasterMgr->CleanupRoguelikeSession(run->CurrentSessionId, false);

    // Teleport out
    TeleportRunPlayersOut(*run);

    // Set cooldowns
    for (const auto& pd : run->Players)
        sDungeonMasterMgr->SetCooldown(pd.PlayerGuid);

    // Save before erase invalidates the pointer
    uint32 savedTier    = run->CurrentTier;
    uint32 savedCleared = run->DungeonsCleared;
    uint32 savedSessId  = run->CurrentSessionId;

    // Erase run
    {
        std::lock_guard<std::mutex> lock(_runMutex);
        _sessionToRun.erase(savedSessId);
        for (const auto& pd : run->Players)
            _playerToRun.erase(pd.PlayerGuid);
        _activeRuns.erase(runId);
    }


    LOG_INFO("module", "RoguelikeMgr: Run {} ended (graceful) — tier {}, {} floors.",
        runId, savedTier, savedCleared);
}

void RoguelikeMgr::AbandonRun(uint32 runId)
{
    EndRun(runId, true);
}

void RoguelikeMgr::QuitRun(ObjectGuid playerGuid)
{
    uint32 runId = 0;
    {
        std::lock_guard<std::mutex> lock(_runMutex);
        auto it = _playerToRun.find(playerGuid);
        if (it != _playerToRun.end())
            runId = it->second;
    }
    if (runId != 0)
        AbandonRun(runId);
}

// QUERIES

RoguelikeRun* RoguelikeMgr::GetRun(uint32 runId)
{
    std::lock_guard<std::mutex> lock(_runMutex);
    auto it = _activeRuns.find(runId);
    return it != _activeRuns.end() ? &it->second : nullptr;
}

RoguelikeRun* RoguelikeMgr::GetRunBySession(uint32 sessionId)
{
    std::lock_guard<std::mutex> lock(_runMutex);
    auto it = _sessionToRun.find(sessionId);
    if (it != _sessionToRun.end())
    {
        auto rit = _activeRuns.find(it->second);
        return rit != _activeRuns.end() ? &rit->second : nullptr;
    }
    return nullptr;
}

RoguelikeRun* RoguelikeMgr::GetRunByPlayer(ObjectGuid guid)
{
    std::lock_guard<std::mutex> lock(_runMutex);
    auto it = _playerToRun.find(guid);
    if (it != _playerToRun.end())
    {
        auto rit = _activeRuns.find(it->second);
        return rit != _activeRuns.end() ? &rit->second : nullptr;
    }
    return nullptr;
}

uint32 RoguelikeMgr::GetRunIdBySession(uint32 sessionId) const
{
    std::lock_guard<std::mutex> lock(_runMutex);
    auto it = _sessionToRun.find(sessionId);
    return it != _sessionToRun.end() ? it->second : 0;
}

bool RoguelikeMgr::IsPlayerInRun(ObjectGuid guid) const
{
    std::lock_guard<std::mutex> lock(_runMutex);
    return _playerToRun.count(guid) > 0;
}

uint32 RoguelikeMgr::GetActiveRunCount() const
{
    std::lock_guard<std::mutex> lock(_runMutex);
    return static_cast<uint32>(_activeRuns.size());
}

// Scaling (called from PopulateDungeon)

float RoguelikeMgr::GetTierHealthMultiplier(uint32 runId) const
{
    std::lock_guard<std::mutex> lock(_runMutex);
    auto it = _activeRuns.find(runId);
    if (it == _activeRuns.end()) return 1.0f;

    uint32 tier = it->second.CurrentTier;
    if (tier <= 1) return 1.0f;

    float baseScale = sDMConfig->GetRoguelikeHpScaling();
    uint32 expThresh = sDMConfig->GetRoguelikeExpThreshold();
    float  expFactor = sDMConfig->GetRoguelikeExpFactor();

    if (tier <= expThresh)
        return 1.0f + (tier - 1) * baseScale;

    // Exponential scaling past threshold
    float linearPart = (expThresh - 1) * baseScale;
    float expPart    = 0.0f;
    for (uint32 t = expThresh; t < tier; ++t)
        expPart += baseScale * std::pow(expFactor, static_cast<float>(t - expThresh + 1));

    return 1.0f + linearPart + expPart;
}

float RoguelikeMgr::GetTierDamageMultiplier(uint32 runId) const
{
    std::lock_guard<std::mutex> lock(_runMutex);
    auto it = _activeRuns.find(runId);
    if (it == _activeRuns.end()) return 1.0f;

    uint32 tier = it->second.CurrentTier;
    if (tier <= 1) return 1.0f;

    float baseScale = sDMConfig->GetRoguelikeDmgScaling();
    uint32 expThresh = sDMConfig->GetRoguelikeExpThreshold();
    float  expFactor = sDMConfig->GetRoguelikeExpFactor();

    if (tier <= expThresh)
        return 1.0f + (tier - 1) * baseScale;

    float linearPart = (expThresh - 1) * baseScale;
    float expPart    = 0.0f;
    for (uint32 t = expThresh; t < tier; ++t)
        expPart += baseScale * std::pow(expFactor, static_cast<float>(t - expThresh + 1));

    return 1.0f + linearPart + expPart;
}

float RoguelikeMgr::GetTierArmorMultiplier(uint32 runId) const
{
    std::lock_guard<std::mutex> lock(_runMutex);
    auto it = _activeRuns.find(runId);
    if (it == _activeRuns.end()) return 1.0f;

    uint32 tier = it->second.CurrentTier;
    if (tier <= 1) return 1.0f;

    float baseScale = sDMConfig->GetRoguelikeArmorScaling();
    return 1.0f + (tier - 1) * baseScale;   // Armor scales linearly only
}

void RoguelikeMgr::GetAffixMultipliers(
    uint32 runId, bool isBoss, bool /*isElite*/,
    float& outHpMult, float& outDmgMult, float& outEliteChanceMult) const
{
    outHpMult   = 1.0f;
    outDmgMult  = 1.0f;
    outEliteChanceMult = 1.0f;

    std::lock_guard<std::mutex> lock(_runMutex);
    auto it = _activeRuns.find(runId);
    if (it == _activeRuns.end()) return;

    for (RoguelikeAffix afxId : it->second.ActiveAffixes)
    {
        for (const auto& def : _affixDefs)
        {
            if (def.Id == afxId)
            {
                if (isBoss)
                {
                    outHpMult  *= def.BossHpMult;
                    outDmgMult *= def.BossDmgMult;
                }
                else
                {
                    outHpMult  *= def.TrashHpMult;
                    outDmgMult *= def.TrashDmgMult;
                }
                outEliteChanceMult *= def.EliteChanceMult;
                break;
            }
        }
    }
}

bool RoguelikeMgr::HasActiveAffixes(uint32 runId) const
{
    std::lock_guard<std::mutex> lock(_runMutex);
    auto it = _activeRuns.find(runId);
    return it != _activeRuns.end() && !it->second.ActiveAffixes.empty();
}

std::string RoguelikeMgr::GetActiveAffixNames(uint32 runId) const
{
    std::lock_guard<std::mutex> lock(_runMutex);
    auto it = _activeRuns.find(runId);
    if (it == _activeRuns.end()) return "";

    std::string result;
    for (RoguelikeAffix afxId : it->second.ActiveAffixes)
    {
        for (const auto& def : _affixDefs)
        {
            if (def.Id == afxId)
            {
                if (!result.empty()) result += ", ";
                result += "|cFFFF8800" + def.Name + "|r";
                break;
            }
        }
    }
    return result;
}

// Buff system (+10% all stats per stack via direct stat modification)
// In 3.3.5 clients, spell tooltips are hardcoded in the DBC and can't be updated
// Buff system (+10% all stats per stack via BoK aura with visual stack count)
// SetStackAmount(n) both displays the stack number on the buff icon AND
// auto-multiplies the base 10% effect by n (so 3 stacks = 30%).

static constexpr uint32 BUFF_SPELL_ID = 25898;  // Greater Blessing of Kings

void RoguelikeMgr::ApplyBuffAura(Player* player, uint32 stacks)
{
    if (!player || !player->IsInWorld() || stacks == 0) return;

    // Remove old aura before reapplying with new stack count
    player->RemoveAura(BUFF_SPELL_ID);

    Aura* aura = player->AddAura(BUFF_SPELL_ID, player);
    if (aura)
    {
        aura->SetStackAmount(static_cast<uint8>(stacks));
        aura->SetMaxDuration(-1);
        aura->SetDuration(-1);
    }
}

void RoguelikeMgr::IncrementBuffStacks(uint32 runId)
{
    RoguelikeRun* run = nullptr;
    {
        std::lock_guard<std::mutex> lock(_runMutex);
        auto it = _activeRuns.find(runId);
        if (it == _activeRuns.end()) return;
        run = &it->second;
    }

    ++run->BuffStacks;

    for (const auto& pd : run->Players)
    {
        Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid);
        if (p && p->IsInWorld())
            ApplyBuffAura(p, run->BuffStacks);
    }

    // Announce
    float totalPct = BUFF_PCT_PER_STACK * run->BuffStacks;
    char buf[256];
    snprintf(buf, sizeof(buf),
        "|cFF00FFFF[Roguelike]|r |cFF00FF00+%.0f%% All Stats|r (Stack %u)",
        totalPct, run->BuffStacks);
    AnnounceToRun(*run, buf);
}

void RoguelikeMgr::ApplyBuffStacks(Player* player, uint32 runId)
{
    if (!player || !player->IsInWorld()) return;

    std::lock_guard<std::mutex> lock(_runMutex);
    auto it = _activeRuns.find(runId);
    if (it == _activeRuns.end()) return;

    if (it->second.BuffStacks == 0) return;
    ApplyBuffAura(player, it->second.BuffStacks);
}

void RoguelikeMgr::RemoveBuffStacks(Player* player, uint32 runId)
{
    if (!player || !player->IsInWorld()) return;
    player->RemoveAura(BUFF_SPELL_ID);
}

// AFFIX SELECTION

void RoguelikeMgr::SelectAffixesForTier(RoguelikeRun& run)
{
    run.ActiveAffixes.clear();

    uint32 affixStart  = sDMConfig->GetRoguelikeAffixStartTier();
    uint32 secondAffix = sDMConfig->GetRoguelikeSecondAffixTier();
    uint32 thirdAffix  = sDMConfig->GetRoguelikeThirdAffixTier();

    if (run.CurrentTier < affixStart || _affixDefs.empty())
        return;

    uint32 numAffixes = 1;
    if (run.CurrentTier >= thirdAffix)
        numAffixes = 3;
    else if (run.CurrentTier >= secondAffix)
        numAffixes = 2;


    std::vector<RoguelikeAffix> pool;
    for (const auto& def : _affixDefs)
        if (def.Id != AFFIX_NONE)
            pool.push_back(def.Id);

    std::shuffle(pool.begin(), pool.end(), tRng);

    for (uint32 i = 0; i < numAffixes && i < pool.size(); ++i)
        run.ActiveAffixes.push_back(pool[i]);
}

// DUNGEON SELECTION

uint32 RoguelikeMgr::SelectRandomDungeon(const RoguelikeRun& run) const
{
    const DifficultyTier* diff = sDMConfig->GetDifficulty(run.BaseDifficultyId);
    if (!diff)
    {
        // Fallback: use broadest range
        auto dgs = sDMConfig->GetDungeonsForLevel(1, 80);
        if (dgs.empty()) return 0;
        return dgs[RandInt<size_t>(0, dgs.size() - 1)]->MapId;
    }

    auto dgs = sDMConfig->GetDungeonsForLevel(diff->MinLevel, diff->MaxLevel);
    if (dgs.empty()) return 0;

    // Try to avoid repeating the same dungeon
    if (dgs.size() > 1 && run.PreviousMapId != 0)
    {
        std::vector<const DungeonInfo*> filtered;
        for (const auto* d : dgs)
            if (d->MapId != run.PreviousMapId)
                filtered.push_back(d);

        if (!filtered.empty())
            return filtered[RandInt<size_t>(0, filtered.size() - 1)]->MapId;
    }

    return dgs[RandInt<size_t>(0, dgs.size() - 1)]->MapId;
}

// Transition between dungeons

bool RoguelikeMgr::TransitionToNextDungeon(RoguelikeRun& run)
{
    uint32 mapId = SelectRandomDungeon(run);
    if (!mapId)
    {
        LOG_WARN("module", "RoguelikeMgr: No dungeon available for run {} tier {}",
            run.RunId, run.CurrentTier);
        return false;
    }

    // Find leader or first online player
    Player* leader = ObjectAccessor::FindPlayer(run.LeaderGuid);
    if (!leader)
    {
        for (const auto& pd : run.Players)
        {
            leader = ObjectAccessor::FindPlayer(pd.PlayerGuid);
            if (leader) { run.LeaderGuid = leader->GetGUID(); break; }
        }
    }

    if (!leader)
    {
        LOG_WARN("module", "RoguelikeMgr: No online leader for run {}", run.RunId);
        return false;
    }

    // Clear cooldowns (EndSession might have set them)
    for (const auto& pd : run.Players)
        sDungeonMasterMgr->ClearCooldown(pd.PlayerGuid);

    // Select theme: run-locked theme or random
    uint32 themeId = run.ThemeId;
    if (themeId == 0)
    {
        const auto& themes = sDMConfig->GetThemes();
        if (!themes.empty())
            themeId = themes[RandInt<size_t>(0, themes.size() - 1)].Id;
    }

    // Create the new DM session
    Session* session = sDungeonMasterMgr->CreateSession(
        leader, run.BaseDifficultyId, themeId, mapId, run.ScaleToParty);
    if (!session)
    {
        LOG_ERROR("module", "RoguelikeMgr: Failed to create session for run {} tier {}",
            run.RunId, run.CurrentTier);
        return false;
    }

    // Tag as roguelike
    session->RoguelikeRunId = run.RunId;
    run.CurrentSessionId    = session->SessionId;

    // Register session mapping
    {
        std::lock_guard<std::mutex> lock(_runMutex);
        _sessionToRun[session->SessionId] = run.RunId;
    }

    // Start and teleport
    if (!sDungeonMasterMgr->StartDungeon(session))
    {
        LOG_ERROR("module", "RoguelikeMgr: StartDungeon failed for run {}", run.RunId);
        sDungeonMasterMgr->CleanupRoguelikeSession(session->SessionId, false);
        {
            std::lock_guard<std::mutex> lock2(_runMutex);
            _sessionToRun.erase(session->SessionId);
        }
        return false;
    }

    if (!sDungeonMasterMgr->TeleportPartyIn(session))
    {
        LOG_ERROR("module", "RoguelikeMgr: Teleport failed for run {}", run.RunId);
        sDungeonMasterMgr->CleanupRoguelikeSession(session->SessionId, false);
        {
            std::lock_guard<std::mutex> lock2(_runMutex);
            _sessionToRun.erase(session->SessionId);
        }
        return false;
    }

    run.State = RoguelikeRunState::Active;

    const DungeonInfo* dg = sDMConfig->GetDungeon(mapId);
    char buf[256];
    snprintf(buf, sizeof(buf),
        "|cFF00FFFF[Roguelike]|r Entering |cFFFFFFFF%s|r — Tier |cFFFF0000%u|r",
        dg ? dg->Name.c_str() : "Unknown", run.CurrentTier);
    AnnounceToRun(run, buf);

    LOG_INFO("module", "RoguelikeMgr: Run {} transitioned to tier {} — map {} ({})",
        run.RunId, run.CurrentTier, mapId,
        dg ? dg->Name.c_str() : "?");

    return true;
}

void RoguelikeMgr::TeleportRunPlayersOut(RoguelikeRun& run)
{
    for (const auto& pd : run.Players)
    {
        Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid);
        if (!p) continue;

        // Skip players that are mid-teleport or not fully in the world.
        if (!p->IsInWorld())
            continue;

        p->RemoveFlag(PLAYER_FIELD_BYTES, PLAYER_FIELD_BYTE_NO_RELEASE_WINDOW);
        if (!p->IsAlive())
        {
            p->ResurrectPlayer(1.0f);
            p->SpawnCorpseBones();
        }

        p->TeleportTo(pd.OriginalMapId,
            pd.OriginalPosition.GetPositionX(),
            pd.OriginalPosition.GetPositionY(),
            pd.OriginalPosition.GetPositionZ(),
            pd.OriginalPosition.GetOrientation());
    }
}

// ANNOUNCEMENTS

void RoguelikeMgr::AnnounceToRun(const RoguelikeRun& run, const char* msg)
{
    for (const auto& pd : run.Players)
        if (Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid))
            if (p->GetSession())
                ChatHandler(p->GetSession()).SendSysMessage(msg);
}

void RoguelikeMgr::AnnounceCountdown(const RoguelikeRun& run, uint32 remainingSec)
{
    char buf[128];
    snprintf(buf, sizeof(buf),
        "|cFF00FFFF[Roguelike]|r Next dungeon in |cFFFFFFFF%u|r second%s...",
        remainingSec, remainingSec != 1 ? "s" : "");
    AnnounceToRun(run, buf);
}


void RoguelikeMgr::Update(uint32 diff)
{
    _updateTimer += diff;
    if (_updateTimer < UPDATE_INTERVAL)
        return;
    _updateTimer = 0;

    std::vector<uint32> toAbandon;

    {
        std::lock_guard<std::mutex> lock(_runMutex);

        for (auto& [rid, run] : _activeRuns)
        {
            // ---- Transition grace period ----
            if (run.TransitionStartTime > 0)
            {
                uint64 elapsed = GameTime::GetGameTime().count() - run.TransitionStartTime;
                if (elapsed < 30)
                    continue;   // still in grace window
                // Grace expired — clear flag so normal detection resumes
                run.TransitionStartTime = 0;
            }

            // ---- Abandoned detection: all players offline ----
            bool anyOnline = false;
            for (const auto& pd : run.Players)
            {
                Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid);
                if (p && p->IsInWorld()) { anyOnline = true; break; }
            }

            if (!anyOnline)
            {
                toAbandon.push_back(rid);
                continue;
            }

            // ---- Re-apply buff aura after death ----
            if (run.State == RoguelikeRunState::Active && run.BuffStacks > 0)
            {
                for (const auto& pd : run.Players)
                {
                    Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid);
                    if (!p || !p->IsInWorld() || !p->IsAlive()) continue;

                    if (!p->HasAura(BUFF_SPELL_ID))
                        ApplyBuffAura(p, run.BuffStacks);
                }
            }
        }
    }

    for (uint32 rid : toAbandon)
    {
        LOG_INFO("module", "RoguelikeMgr: Run {} — all players offline, abandoning.", rid);
        EndRun(rid, false);
    }
}

// LEADERBOARD

void RoguelikeMgr::SaveRoguelikeLeaderboard(const RoguelikeRun& run)
{
    uint32 duration = 0;
    if (GameTime::GetGameTime().count() > static_cast<time_t>(run.RunStartTime))
        duration = static_cast<uint32>(GameTime::GetGameTime().count() - run.RunStartTime);

    std::string leaderName = "Unknown";
    if (Player* leader = ObjectAccessor::FindPlayer(run.LeaderGuid))
        leaderName = leader->GetName();

    // Escape name
    std::string safeName = leaderName;
    size_t pos = 0;
    while ((pos = safeName.find('\'', pos)) != std::string::npos)
    {
        safeName.replace(pos, 1, "''");
        pos += 2;
    }

    uint8 partySize = static_cast<uint8>(run.Players.size());

    char query[512];
    snprintf(query, sizeof(query),
        "INSERT INTO dm_roguelike_leaderboard "
        "(guid, char_name, tier_reached, dungeons_cleared, total_kills, "
        "run_duration, party_size) "
        "VALUES (%u, '%s', %u, %u, %u, %u, %u)",
        run.LeaderGuid.GetCounter(), safeName.c_str(),
        run.CurrentTier, run.DungeonsCleared,
        run.TotalMobsKilled + run.TotalBossesKilled,
        duration, partySize);
    CharacterDatabase.Execute(query);
}

std::vector<RoguelikeLeaderboardEntry> RoguelikeMgr::GetRoguelikeLeaderboard(uint32 limit) const
{
    std::vector<RoguelikeLeaderboardEntry> entries;

    char query[512];
    snprintf(query, sizeof(query),
        "SELECT id, guid, char_name, tier_reached, dungeons_cleared, "
        "total_kills, run_duration, party_size "
        "FROM dm_roguelike_leaderboard "
        "ORDER BY tier_reached DESC, dungeons_cleared DESC, run_duration ASC "
        "LIMIT %u", limit);

    QueryResult result = CharacterDatabase.Query(query);
    if (!result) return entries;

    do
    {
        Field* f = result->Fetch();
        RoguelikeLeaderboardEntry e;
        e.Id              = f[0].Get<uint32>();
        e.Guid            = f[1].Get<uint32>();
        e.CharName        = f[2].Get<std::string>();
        e.TierReached     = f[3].Get<uint32>();
        e.DungeonsCleared = f[4].Get<uint32>();
        e.TotalKills      = f[5].Get<uint32>();
        e.RunDuration     = f[6].Get<uint32>();
        e.PartySize       = f[7].Get<uint8>();
        entries.push_back(e);
    } while (result->NextRow());

    return entries;
}

} // namespace DungeonMaster
