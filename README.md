# mod-dungeon-master

## THIS IS VERY EARLY IN DEVELOPMENT, THERE WILL BE BUGS

A procedural dungeon challenge system for **AzerothCore** playerbots fork (WotLK 3.3.5a).

Players interact with a Dungeon Master NPC to select a difficulty tier, creature theme, and dungeon instance.  They're teleported into a cleared dungeon repopulated with level-appropriate themed creatures and a boss.  Kill everything to earn gold and item rewards.

## Features

- **9 creature themes** — Beasts, Dragons, Demons, Elementals, Giants, Undead, Humanoids, Mechanicals, Random
- **6 difficulty tiers** — Novice through Grandmaster, each with tuned health/damage/reward multipliers
- **45 dungeons** — Classic, TBC, and WotLK 5-man instances
- **Player-level scaling** — Choose to scale creatures to your party's level or use the tier's natural range
- **Any dungeon, any level** — Level 80s can run Deadmines at its original difficulty or scaled to 80
- **Party support** — Solo, groups up to 5, and Playerbot-compatible
- **Elite & boss modifiers** — Configurable health/damage multipliers for elites and bosses
- **Per-player difficulty scaling** — HP/damage scale with party size; solo players get a reduction
- **Auto-resurrect** — Dead players revive at the entrance when the group leaves combat
- **Class-appropriate rewards** — Guaranteed gold on every kill + armor-proficiency-filtered gear (no plate on druids)
- **Cooldown system** — Configurable per-character cooldown between runs
- **Persistent statistics** — Tracks runs, kills, deaths, and fastest clear times per character
- **Leaderboard** — Server-wide fastest clear times viewable from the NPC
- **GM commands** — `.dm reload`, `.dm status`, `.dm list`, `.dm end`, `.dm clearcooldown`

## Installation

1. Clone into `modules/`:
   ```
   cd azerothcore-wotlk/modules
   git clone <this-repo> mod-dungeon-master
   ```

2. Re-run CMake and rebuild the server.

3. Run the SQL setup on your **world** database:
   ```sql
   source data/sql/db-world/base/dm_setup.sql
   ```

4. Run the SQL setup on your **characters** database:
   ```sql
   source data/sql/db-characters/base/dm_characters_setup.sql
   ```

5. Copy the config file:
   ```
   conf/mod_dungeon_master.conf.dist and change to conf/mod_dungeon_master.conf
   ```

6. Restart the server.

## Dungeon Master Spawns

The Dungeon Master NPC spawns in every major city.

You may also spawn them with .npc add 500000

### Alliance

| City | Zone | Map | X | Y | Z |
|------|------|-----|---|---|---|
| Stormwind City | Trade District, near the fountain | 0 | -8842.0 | 626.0 | 94.30 |
| Ironforge | The Commons, near the central forge | 0 | -4918.0 | -957.0 | 501.53 |
| Darnassus | Tradesman's Terrace | 1 | 9869.0 | 2494.0 | 1316.20 |
| Exodar | The Crystal Hall | 530 | -3862.7 | -11645.8 | -137.63 |

### Horde

| City | Zone | Map | X | Y | Z |
|------|------|-----|---|---|---|
| Orgrimmar | Valley of Strength, near the bank | 1 | 1676.0 | -4316.0 | 61.80 |
| Thunder Bluff | High Rise, central platform | 1 | -1277.6 | 73.0 | 128.75 |
| Undercity | Trade Quarter | 0 | 1637.2 | 240.1 | -43.10 |
| Silvermoon City | Walk of Elders | 530 | 9738.0 | -7454.0 | 13.56 |

### Neutral

| City | Zone | Map | X | Y | Z |
|------|------|-----|---|---|---|
| Shattrath City | Terrace of Light | 530 | -1850.0 | 5436.0 | -12.10 |
| Dalaran | Runeweaver Square | 571 | 5807.0 | 506.2 | 657.58 |
| Booty Bay | Docks / Inn level | 0 | -14406.0 | 420.0 | 23.70 |

## Level Scaling (How It Works)

After selecting a difficulty tier, players choose a scaling mode:

**Scale to Party Level** — Creatures match your group's average level. A level 80 running Deadmines gets level 80 creatures. Great for a challenge in any dungeon.

**Use Dungeon Difficulty** — Creatures stay at the tier's natural range. A level 80 running Novice (10-19) gets level 10-19 creatures. Easy mode / farming.

### Technical details

1. **Effective Level** = party average (Scale to Party) or tier midpoint (Dungeon Difficulty).
2. **Level Band** = EffectiveLevel ± `LevelBand` (default 3), clamped to the tier's range.
3. Only creatures whose level range overlaps the band are eligible for spawning.
4. Creatures are force-scaled to the target level with recalculated HP, damage, armor, and defense stats.

## Loot

Every creature drops gold (minimum 1 silver).  Item drops use WoW-like rates:

| Creature Type | Gold | Items |
|---------------|------|-------|
| Trash mob | Always | 15% grey/white junk, 3% green equipment |
| Elite | Always | 40% green equipment |
| Boss | Always | 2 guaranteed rare/epic equipment pieces |

All equipment drops respect the party's class proficiencies — druids get leather, warriors get plate, etc.

## Configuration

All settings are in `mod_dungeon_master.conf`.  Key settings:

| Setting | Default | Description |
|---------|---------|-------------|
| `Scaling.LevelBand` | 3 | Creature selection window: ±N levels |
| `Scaling.SoloMultiplier` | 0.5 | HP/damage reduction for solo players |
| `Scaling.BossHealthMult` | 5.0 | Boss HP multiplier (on top of difficulty) |
| `Scaling.EliteHealthMult` | 2.0 | Elite trash HP multiplier |
| `Dungeon.EliteChance` | 20 | % chance a trash mob is elite |
| `Dungeon.BossCount` | 1 | Number of bosses per run |
| `Dungeon.AggroRadius` | 15.0 | Detection range in yards (lower = less swarming) |

## GM Commands

| Command | Access | Description |
|---------|--------|-------------|
| `.dm status` | GM | Show module status |
| `.dm list` | GM | List active sessions |
| `.dm end [id]` | Admin | Force-end a session |
| `.dm clearcooldown` | GM | Clear cooldown for target |
| `.dm reload` | Admin | Hot-reload configuration |

## Credits

- **[AzerothCore](https://github.com/azerothcore/azerothcore-wotlk)** — The open-source WoW 3.3.5a server emulator that makes this all possible
- **[AzerothCore-playerbots](https://github.com/liyunfan1223/azerothcore-wotlk)** — The playerbots fork used as the primary development target
- **[mod-playerbots](https://github.com/liyunfan1223/mod-playerbots)** by liyunfan1223 — AI-controlled player bots for solo and small-group play

## Tips

https://ko-fi.com/trauntrow

## License

AGPL 3.0 — see [LICENSE](LICENSE).
