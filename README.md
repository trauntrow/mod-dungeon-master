# mod-dungeon-master

A procedural dungeon challenge system for **AzerothCore** (WotLK 3.3.5a). Built and tested on the [playerbots fork](https://github.com/liyunfan1223/azerothcore-wotlk).

Players talk to a Dungeon Master NPC, pick a difficulty, creature theme, and dungeon — then get teleported into a cleared instance repopulated with level-scaled themed enemies and a boss. Kill everything to earn rewards. Roguelike mode chains dungeons together with escalating difficulty, affixes, and stacking stat buffs until the party wipes.

> ⚠️ **Early development** — expect bugs and rough edges. Bug reports welcome via Issues.

---

## Features

### Core System
- **37 dungeons** — Classic, TBC, and WotLK 5-man instances
- **9 creature themes** — Beast Hunt, Dragon's Lair, Demonic Invasion, Elemental Chaos, Giant's Keep, Undead Rising, Humanoid Stronghold, Mechanical Mayhem, Random Chaos
- **6 difficulty tiers** — Novice through Grandmaster with tuned HP/damage/reward multipliers
- **Level scaling** — Scale creatures to your party's level or use the tier's natural range
- **Any dungeon, any level** — A level 80 can run Deadmines scaled to 80, or at its original difficulty
- **Real dungeon bosses** — Final bosses are pulled from a global pool of all dungeon bosses across Classic, TBC, and WotLK instances, matched to the session's theme. Bosses keep their original model and name but fight with properly level-scaled melee damage (native spell scripts are replaced to prevent unscaled one-shots). Multi-phase bosses are fully supported — if a boss transitions to a new phase (spawning a new creature), the system detects and promotes it automatically
- **Party support** — Solo or groups up to 5, fully Playerbot-compatible
- **Group Loot support** — Items dropped by enemies support the Group Loot game mechanic where party members roll Need or Greed on qualifying items
- **Per-player difficulty** — HP and damage scale with party size; solo players get a reduction
- **Auto-resurrect** — Dead players revive at the entrance when combat ends
- **Environmental damage scaling** — Native dungeon hazards are scaled down for level-mismatched parties, hard-capped at 3% max HP per tick
- **Cooldown system** — Configurable per-character cooldown between runs
- **Persistent stats** — Tracks runs, kills, deaths, fastest clear times per character
- **Leaderboards** — Server-wide fastest clears viewable from the NPC
- **GM commands** — `.dm reload`, `.dm status`, `.dm list`, `.dm end`, `.dm clearcooldown`

### Roguelike Mode
- **Infinite progression** — Clear a dungeon, get teleported to the next one, repeat until you wipe
- **Escalating difficulty** — Enemy HP and damage scale per tier (linear, then exponential)
- **Stacking stat buff** — +10% all stats per dungeon cleared (Blessing of Kings aura, reapplied on death)
- **Mythic+ style affixes** — Fortified, Tyrannical, Raging, Bolstering, Savage; layered at configurable tier thresholds
- **Random dungeon rotation** — Each floor picks a new dungeon appropriate for your level band
- **Tier-scaled rewards** — Better loot at higher tiers, with increasing epic drop chance
- **Roguelike leaderboard** — Separate leaderboard tracking highest tier reached
- **NPC-based flow** — Between floors, players return to the world to interact with the NPC for the next dungeon (stable, no instance-to-instance teleport issues)

---

## Installation

1. **Clone into modules:**
   ```bash
   cd azerothcore-wotlk/modules
   git clone https://github.com/your-username/mod-dungeon-master.git
   ```

2. **Re-run CMake and rebuild** the server.

3. **Run the world database SQL:**
   ```sql
   SOURCE data/sql/db-world/base/dm_setup.sql
   ```

4. **Run the characters database SQL:**
   ```sql
   SOURCE data/sql/db-characters/base/dm_characters_setup.sql
   ```

5. **Copy the config:**
   ```bash
   cp modules/mod-dungeon-master/conf/mod_dungeon_master.conf.dist etc/mod_dungeon_master.conf
   ```

6. **Restart the server.** The Dungeon Master NPC spawns automatically in all major cities.

---

## NPC Spawn Locations

The Dungeon Master NPC (entry `500000`) spawns in every major city. You can also spawn one manually with `.npc add 500000`.

### Alliance

| City | Location | Map | Coordinates |
|------|----------|-----|-------------|
| Stormwind | Trade District, near fountain | 0 | -8842.0, 626.0, 94.3 |
| Ironforge | The Commons, near central forge | 0 | -4918.0, -957.0, 501.5 |
| Darnassus | Tradesman's Terrace | 1 | 9869.0, 2494.0, 1316.2 |
| Exodar | The Crystal Hall | 530 | -3862.7, -11645.8, -137.6 |

### Horde

| City | Location | Map | Coordinates |
|------|----------|-----|-------------|
| Orgrimmar | Valley of Strength, near bank | 1 | 1676.0, -4316.0, 61.8 |
| Thunder Bluff | High Rise, central platform | 1 | -1277.6, 73.0, 128.8 |
| Undercity | Trade Quarter | 0 | 1637.2, 240.1, -43.1 |
| Silvermoon | Walk of Elders | 530 | 9738.0, -7454.0, 13.6 |

### Neutral

| City | Location | Map | Coordinates |
|------|----------|-----|-------------|
| Shattrath | Terrace of Light | 530 | -1850.0, 5436.0, -12.1 |
| Dalaran | Runeweaver Square | 571 | 5807.0, 506.2, 657.6 |
| Booty Bay | Docks / Inn level | 0 | -14406.0, 420.0, 23.7 |

---

## How It Works

### Standard Mode

1. Talk to the Dungeon Master NPC
2. Choose **difficulty tier** (Novice → Grandmaster)
3. Choose **scaling mode**:
   - **Scale to Party** — creatures match your group's average level
   - **Use Dungeon Difficulty** — creatures stay at the tier's natural level range
4. Choose **creature theme** (Undead, Demons, Beasts, etc.)
5. Choose **dungeon**
6. Confirm → teleport in → kill everything → collect rewards → teleport out

### Roguelike Mode

1. Talk to the NPC → select **Roguelike Challenge**
2. Choose difficulty, scaling, and theme (or random theme each floor)
3. Clear floor 1 → earn +10% all stats buff → auto-transition to floor 2
4. Enemies get harder each tier; affixes kick in at tier 3+
5. Keep going until you wipe — highest tier reached is saved to the leaderboard

### Level Scaling

Creatures are force-scaled to the session's target level regardless of their original template. A level 80 Icecrown ghoul becomes a proper level 14 mob with correct HP, damage, armor, and nameplate display.

- **Effective Level** = party average (Scale to Party) or tier midpoint (Use Dungeon Difficulty)
- **Level Band** = EffectiveLevel ± `LevelBand` (default ±3), clamped to the tier's range
- Stats are recalculated from `creature_classlevelstats` for the target level

---

## Configuration

All settings live in `mod_dungeon_master.conf`. Key options:

### Scaling

| Setting | Default | Description |
|---------|---------|-------------|
| `Scaling.LevelBand` | 3 | Creature level window: ±N levels from party |
| `Scaling.SoloMultiplier` | 0.5 | HP/damage reduction for solo players |
| `Scaling.PerPlayerHealth` | 0.25 | HP added per extra party member (25%) |
| `Scaling.BossHealthMult` | 8.0 | Boss HP multiplier (on top of party scaling) |
| `Scaling.BossDamageMult` | 1.5 | Boss damage multiplier (party scaling only, not stacked with tier) |
| `Scaling.EliteHealthMult` | 2.0 | Elite trash HP multiplier |

### Dungeon

| Setting | Default | Description |
|---------|---------|-------------|
| `Dungeon.BossCount` | 1 | Number of bosses per run |
| `Dungeon.EliteChance` | 20 | % chance a trash mob spawns as elite |
| `Dungeon.AggroRadius` | 15.0 | Detection range in yards |
| `Dungeon.Whitelist` | (empty) | Comma-separated map IDs to allow (empty = all) |
| `Dungeon.Blacklist` | (empty) | Comma-separated map IDs to exclude |

### Roguelike

| Setting | Default | Description |
|---------|---------|-------------|
| `Roguelike.Enable` | 1 | Master switch for Roguelike Mode |
| `Roguelike.TransitionDelay` | 30 | Seconds between floors |
| `Roguelike.HpScalingPerTier` | 0.10 | +10% enemy HP per tier |
| `Roguelike.DmgScalingPerTier` | 0.08 | +8% enemy damage per tier |
| `Roguelike.ExponentialThreshold` | 5 | Tier where scaling goes exponential |
| `Roguelike.ExponentialFactor` | 1.15 | Exponential growth factor |
| `Roguelike.AffixStartTier` | 3 | First tier with an affix |
| `Roguelike.SecondAffixTier` | 7 | Second simultaneous affix |
| `Roguelike.ThirdAffixTier` | 10 | Third simultaneous affix |

### Rewards

| Setting | Default | Description |
|---------|---------|-------------|
| `Rewards.BaseGold` | 50000 | Completion gold reward (copper) |
| `Rewards.XPMultiplier` | 1.0 | Kill XP multiplier |
| `Rewards.ItemChance` | 80 | % chance of item reward on completion |
| `Rewards.RareChance` | 40 | % chance reward is rare quality |
| `Rewards.EpicChance` | 15 | % chance reward is epic quality |

See `mod_dungeon_master.conf.dist` for the full list with descriptions.

---

## Roguelike Affixes

Affixes are Mythic+ style modifiers applied at higher tiers:

| Affix | Effect |
|-------|--------|
| **Fortified** | Trash mobs: +30% HP, +15% damage |
| **Tyrannical** | Bosses: +40% HP, +20% damage |
| **Raging** | All enemies: +25% damage |
| **Bolstering** | All enemies: +20% HP |
| **Savage** | Double elite spawn chance |

Affixes stack — at tier 10+ you might face Fortified + Tyrannical + Raging simultaneously.

---

## GM Commands

| Command | Access | Description |
|---------|--------|-------------|
| `.dm status` | GM | Show module status and active session count |
| `.dm list` | GM | List all active sessions with details |
| `.dm end [id]` | Admin | Force-end a session (defaults to your own) |
| `.dm clearcooldown` | GM | Clear cooldown for target's whole group |
| `.dm reload` | Admin | Hot-reload configuration |

---

## Technical Notes

- **AzerothCore playerbots fork** — Built against the [liyunfan1223 fork](https://github.com/liyunfan1223/azerothcore-wotlk). Uses `id1` column names in creature queries for compatibility.
- **Thread safety** — Session maps and cooldowns are mutex-guarded for multi-player safety.
- **Async teleport handling** — 30-second grace period after teleports to prevent false "abandoned" detection.
- **InstanceScript neutralization** — All boss encounters are marked DONE on populate to prevent native scripts from interfering.
- **Debuff purging** — Lingering debuffs from despawned creatures are removed before each floor.
- **Custom creature AI** — All spawned creatures (trash and bosses) use `DungeonMasterCreatureAI` which patrols a 5 yd radius around spawn points, actively scans for players within aggro range (with a 1-second fallback timer for grid edge cases), and hooks `JustDied` for proper loot timing. Boss native spell scripts are replaced because they contain hard-coded damage values designed for their original level range; bosses fight with properly scaled melee instead.
- **Multi-phase boss detection** — When a boss dies, the system waits 5 seconds and scans for new elite/boss creatures near the death location. If a phase-2 creature is detected, it is automatically promoted to boss status and the original death does not count as a kill.
- **Group Loot support** — Creature loot triggers the group's Need/Greed roll system for items above the group's loot quality threshold.
- **Boss damage scaling** — Boss damage uses only party-size scaling (not stacked with the difficulty tier's DamageMultiplier) to prevent excessive damage when combined with level scaling.
- **Standard AzerothCore** — Should work on vanilla AzerothCore with minor query adjustments (`id1` → `entry` in creature table joins). Not tested.

---

## File Structure

```
mod-dungeon-master/
├── CMakeLists.txt
├── conf/
│   └── mod_dungeon_master.conf.dist
├── data/sql/
│   ├── db-world/base/dm_setup.sql
│   └── db-characters/base/dm_characters_setup.sql
└── src/
    ├── DMConfig.cpp / .h          # Config loader
    ├── DMTypes.h                   # Shared data structures
    ├── DungeonMasterMgr.cpp / .h   # Core session manager
    ├── RoguelikeMgr.cpp / .h       # Roguelike run manager
    ├── RoguelikeTypes.h            # Roguelike data structures
    ├── DungeonMaster_loader.cpp    # Module entry point
    └── scripts/
        ├── npc_dungeon_master.cpp  # NPC gossip menus
        ├── dm_allmap_script.cpp    # Map entry trigger
        ├── dm_command_script.cpp   # GM commands
        ├── dm_player_script.cpp    # Player death handling
        ├── dm_unit_script.cpp      # Environmental damage scaling
        └── dm_world_script.cpp     # Server lifecycle hooks
```

---

## Credits

- **[AzerothCore](https://github.com/azerothcore/azerothcore-wotlk)** — Open-source WoW 3.3.5a server emulator
- **[AzerothCore-playerbots](https://github.com/liyunfan1223/azerothcore-wotlk)** — Playerbots fork used as the primary dev target
- **[mod-playerbots](https://github.com/liyunfan1223/mod-playerbots)** by liyunfan1223 — AI player bots for solo and small-group play

## Tips

https://ko-fi.com/trauntrow

## License

AGPL 3.0 — see [LICENSE](LICENSE).
