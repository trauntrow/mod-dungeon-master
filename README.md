# mod-dungeon-master

A procedural dungeon challenge system for **AzerothCore** (WotLK 3.3.5a).

Players interact with a Dungeon Master NPC to select a difficulty tier, creature theme, and dungeon instance.  They're teleported into a cleared dungeon repopulated with level-appropriate themed creatures and a boss.  Kill everything to earn gold and item rewards.

## Features

- **9 creature themes** — Beasts, Dragons, Demons, Elementals, Giants, Undead, Humanoids, Mechanicals, Random
- **6 difficulty tiers** — Novice through Grandmaster, each with tuned health/damage/reward multipliers
- **45 dungeons** — Classic, TBC, and WotLK 5-man instances
- **Player-level scaling** — Creatures match the player's (or group average) level within a tight ±3 band
- **Party support** — Solo, groups up to 5, and Playerbot-compatible
- **Elite & boss modifiers** — Configurable health/damage multipliers for elites and bosses
- **Per-player difficulty scaling** — HP/damage scale with party size; solo players get a reduction
- **Auto-resurrect** — Dead players revive at the entrance when the group leaves combat
- **Rewards** — Level-scaled gold + random uncommon/rare/epic gear
- **Cooldown system** — Configurable per-character cooldown between runs
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

4. Copy the config file:
   ```
   cp conf/mod_dungeon_master.conf.dist etc/mod_dungeon_master.conf
   ```

5. Restart the server.

## Level Scaling (How It Works)

The core scaling fix ensures creatures match the actual player level:

1. **Effective Level** = solo player's level, or the group's average level.
2. **Level Band** = EffectiveLevel ± `LevelBand` (default 3), clamped to the difficulty tier's range.
3. Only creatures whose level range overlaps the band are eligible for spawning.

Example: A level 35 player on "Journeyman (30-44)" with LevelBand=3 → creatures are level 32-38 only.

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


## License

AGPL 3.0 — see [LICENSE](LICENSE).
