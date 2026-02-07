/*
 * Copyright (C) 2025 AzerothCore - mod-dungeon-master
 *
 * dm_command_script.cpp — GM commands:
 *   .dm reload           Reload config
 *   .dm status           Module status overview
 *   .dm list             List active sessions
 *   .dm end [id]         Force-end a session (uses own/target session if no ID)
 *   .dm clearcooldown    Clear cooldown for whole group (or self/target)
 */

#include "ScriptMgr.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Player.h"
#include "Group.h"
#include "DungeonMasterMgr.h"
#include "DMConfig.h"
#include <cstdio>

using namespace Acore::ChatCommands;
using namespace DungeonMaster;

class dm_command_script : public CommandScript
{
public:
    dm_command_script() : CommandScript("dm_command_script") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable dmTable =
        {
            { "reload",        HandleReload,        SEC_ADMINISTRATOR,  Console::Yes },
            { "status",        HandleStatus,        SEC_GAMEMASTER,     Console::Yes },
            { "list",          HandleList,           SEC_GAMEMASTER,     Console::Yes },
            { "end",           HandleEnd,            SEC_ADMINISTRATOR,  Console::No  },
            { "clearcooldown", HandleClearCD,        SEC_GAMEMASTER,     Console::No  },
        };
        static ChatCommandTable root = { { "dm", dmTable } };
        return root;
    }

    static bool HandleReload(ChatHandler* h)
    {
        sDMConfig->LoadConfig(true);
        h->SendSysMessage("DungeonMaster: Configuration reloaded.");
        return true;
    }

    static bool HandleStatus(ChatHandler* h)
    {
        char buf[256];
        h->SendSysMessage("=== Dungeon Master Status ===");
        snprintf(buf, sizeof(buf), "Enabled: %s", sDMConfig->IsEnabled() ? "Yes" : "No");
        h->SendSysMessage(buf);
        snprintf(buf, sizeof(buf), "Active: %u / %u",
            sDungeonMasterMgr->GetActiveSessionCount(), sDMConfig->GetMaxConcurrentRuns());
        h->SendSysMessage(buf);
        snprintf(buf, sizeof(buf), "Level Band: +/-%u", sDMConfig->GetLevelBand());
        h->SendSysMessage(buf);
        snprintf(buf, sizeof(buf), "Difficulties: %u  Themes: %u  Dungeons: %u",
            uint32(sDMConfig->GetDifficulties().size()),
            uint32(sDMConfig->GetThemes().size()),
            uint32(sDMConfig->GetDungeons().size()));
        h->SendSysMessage(buf);
        return true;
    }

    static bool HandleList(ChatHandler* h)
    {
        uint32 n = sDungeonMasterMgr->GetActiveSessionCount();
        char buf[128];
        snprintf(buf, sizeof(buf), "Active DM sessions: %u", n);
        h->SendSysMessage(buf);
        return true;
    }

    static bool HandleEnd(ChatHandler* h, Optional<uint32> sessionId)
    {
        char buf[128];
        if (sessionId)
        {
            Session* s = sDungeonMasterMgr->GetSession(*sessionId);
            if (!s) { snprintf(buf, sizeof(buf), "Session %u not found.", *sessionId); h->SendSysMessage(buf); return false; }
            sDungeonMasterMgr->EndSession(*sessionId, false);
            snprintf(buf, sizeof(buf), "Session %u ended.", *sessionId); h->SendSysMessage(buf);
        }
        else
        {
            // Try the invoker's own session first
            Player* invoker = h->GetSession() ? h->GetSession()->GetPlayer() : nullptr;
            Session* s = invoker ? sDungeonMasterMgr->GetSessionByPlayer(invoker->GetGUID()) : nullptr;

            // Fall back to selected player's session
            if (!s)
            {
                Player* t = h->getSelectedPlayer();
                s = t ? sDungeonMasterMgr->GetSessionByPlayer(t->GetGUID()) : nullptr;
            }

            if (!s) { h->SendSysMessage("Not in a DM session. Select a player or provide session ID."); return false; }
            uint32 id = s->SessionId;
            sDungeonMasterMgr->EndSession(id, false);
            snprintf(buf, sizeof(buf), "Session %u ended (all players teleported out).", id); h->SendSysMessage(buf);
        }
        return true;
    }

    static bool HandleClearCD(ChatHandler* h)
    {
        Player* invoker = h->GetSession() ? h->GetSession()->GetPlayer() : nullptr;
        if (!invoker) { h->SendSysMessage("In-game only."); return false; }

        // If invoker is in a group, clear cooldown for ALL group members
        Group* g = invoker->GetGroup();
        if (g)
        {
            uint32 cleared = 0;
            for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (member)
                {
                    sDungeonMasterMgr->ClearCooldown(member->GetGUID());
                    ++cleared;
                }
            }
            char buf[128];
            snprintf(buf, sizeof(buf), "Cooldown cleared for %u group member(s).", cleared);
            h->SendSysMessage(buf);
        }
        else
        {
            // Solo — clear for self or selected player
            Player* t = h->getSelectedPlayer();
            if (!t) t = invoker;
            sDungeonMasterMgr->ClearCooldown(t->GetGUID());
            char buf[128];
            snprintf(buf, sizeof(buf), "Cooldown cleared for %s.", t->GetName().c_str());
            h->SendSysMessage(buf);
        }
        return true;
    }
};

void AddSC_dm_command_script()
{
    new dm_command_script();
}
