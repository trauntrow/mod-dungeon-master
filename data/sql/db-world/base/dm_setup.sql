-- =============================================
-- Dungeon Master Module - Production SQL Setup
-- =============================================
-- Safe to run on a fresh AzerothCore world database.
-- Idempotent: clears previous data before inserting.
--
-- This script creates:
--   1. creature_template         - NPC template (entry 500000)
--   2. creature_template_model   - Stormwind Guard display model
--   3. creature                  - Spawn entries in every major city
--
-- Spawn coordinates sourced from verified working NPC positions
-- (cross-referenced with PortalMaster module and existing city NPCs).
--
-- IMPORTANT: spawnMask=1 and phaseMask=1 are explicitly set to ensure
-- visibility across all AzerothCore builds (some default these to 0).
-- =============================================

-- -----------------------------------------------
-- Step 1: Clean slate  (safe DELETE, not TRUNCATE)
-- -----------------------------------------------
DELETE FROM `creature`               WHERE `id1` = 500000;
DELETE FROM `creature_template_model` WHERE `CreatureID` = 500000;
DELETE FROM `creature_template`      WHERE `entry` = 500000;

-- -----------------------------------------------
-- Step 2: NPC Template
-- -----------------------------------------------
-- npcflag  = 1          (UNIT_NPC_FLAG_GOSSIP)
-- faction  = 190        (Neutral / yellow nameplate — safe in all cities)
-- unit_flags = 2        (NON_ATTACKABLE)
-- flags_extra = 2       (CIVILIAN - no aggro radius)
-- type = 7              (Humanoid)
-- unit_class = 1        (Warrior - simplest stats)
-- ScriptName must match the CreatureScript registered in C++
INSERT INTO `creature_template` (
    `entry`, `name`, `subname`,
    `minlevel`, `maxlevel`,
    `faction`, `npcflag`,
    `speed_walk`, `speed_run`, `scale`,
    `unit_class`, `unit_flags`, `unit_flags2`,
    `type`, `flags_extra`,
    `ScriptName`
) VALUES (
    500000,
    'The Dungeon Master',
    'Challenge Awaits',
    80, 80,
    190,  -- neutral (yellow nameplate)
    1,    -- gossip flag
    1.0, 1.14286,
    1.2,  -- slightly larger than normal
    1,    -- warrior
    2,    -- NON_ATTACKABLE
    0,
    7,    -- humanoid
    2,    -- CIVILIAN
    'npc_dungeon_master'
);

-- -----------------------------------------------
-- Step 3: Display Model  (Stormwind Guard)
-- -----------------------------------------------
-- DisplayID 3167 = Stormwind City Guard (plate armor, sword & shield)
-- This is a universally recognizable, faction-neutral appearance.
INSERT INTO `creature_template_model` (
    `CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`
) VALUES (
    500000, 0, 3167, 1.0, 1.0
);

-- -----------------------------------------------
-- Step 4: City Spawns
-- -----------------------------------------------
-- Coordinates verified against known working NPC spawns (PortalMaster,
-- existing city vendors/guards, and EmuCoach reference data).
--
-- CRITICAL: spawnMask=1 and phaseMask=1 are required for visibility.
-- Some AzerothCore builds default these to 0 if omitted, which makes
-- the creature exist in the DB but appear invisible in-game.
--
-- guid range: 8000001-8000011  (high range to avoid collisions)
-- spawntimesecs = 180           (3 min respawn if somehow killed)
--

INSERT INTO `creature` (
    `guid`, `id1`, `map`, `spawnMask`, `phaseMask`,
    `position_x`, `position_y`, `position_z`, `orientation`,
    `spawntimesecs`
) VALUES
-- =========================================================================
-- ALLIANCE CITIES
-- =========================================================================

-- Stormwind City - Trade District, near the fountain
-- Ref: PortalMaster (-8845.09, 624.828, 94.2999) / game_tele offset
(8000001, 500000, 0, 1, 1,  -8842.0, 626.0, 94.30, 0.44,  180),

-- Ironforge - The Commons, near the central forge area
-- Ref: game_tele (-4981.25, -881.542, 502.66) / PortalMaster (-4898, -965, 501.4)
(8000002, 500000, 0, 1, 1,  -4918.0, -957.0, 501.53, 2.26,  180),

-- Darnassus - Tradesman's Terrace
-- Ref: game_tele (9869.91, 2493.58, 1315.88)
(8000003, 500000, 1, 1, 1,  9869.0, 2494.0, 1316.20, 3.1,  180),

-- Exodar - The Crystal Hall central area
-- Ref: PortalMaster (-3862.69, -11645.8, -137.629)
(8000004, 500000, 530, 1, 1,  -3862.7, -11645.8, -137.63, 2.38,  180),

-- =========================================================================
-- HORDE CITIES
-- =========================================================================

-- Orgrimmar - Valley of Strength, near the bank
-- Ref: game_tele (1676.21, -4315.29, 61.52) with Z verified
(8000005, 500000, 1, 1, 1,  1676.0, -4316.0, 61.80, 5.7,  180),

-- Thunder Bluff - High Rise central platform
-- Ref: PortalMaster (-1277.65, 72.9751, 128.742)
(8000006, 500000, 1, 1, 1,  -1277.6, 73.0, 128.75, 5.96,  180),

-- Undercity - Trade Quarter
-- Ref: PortalMaster (1637.21, 240.132, -43.1034) — original was at -52, ~9 units underground!
(8000007, 500000, 0, 1, 1,  1637.2, 240.1, -43.10, 3.14,  180),

-- Silvermoon City - near the Walk of Elders
-- Ref: PortalMaster (9741.67, -7454.19, 13.5572) / game_tele (9338, -7277, 13.78)
(8000008, 500000, 530, 1, 1,  9738.0, -7454.0, 13.56, 0.5,  180),

-- =========================================================================
-- NEUTRAL CITIES
-- =========================================================================

-- Shattrath City - Terrace of Light
-- Ref: game_tele (-1850.21, 5435.82, -12.4281) with Z bump for safety
(8000009, 500000, 530, 1, 1,  -1850.0, 5436.0, -12.10, 5.5,  180),

-- Dalaran (Northrend) - Runeweaver Square / The Eventide
-- Ref: PortalMaster (5807.06, 506.244, 657.576) — original was at 647.77, ~10 units underground!
(8000010, 500000, 571, 1, 1,  5807.0, 506.2, 657.58, 5.54,  180),

-- Booty Bay - Inn / dock level
-- Ref: game_tele (-14406.6, 419.353, 23.3907) — keep close but bump Z slightly
(8000011, 500000, 0, 1, 1,  -14406.0, 420.0, 23.70, 4.0,  180);


-- =============================================
-- Roguelike Vendor NPC (entry 500001)
-- =============================================
-- Friendly vendor that spawns at dungeon entrances during roguelike runs.
-- Provides: sell/buy (general goods, reagents, ammo), repair.
-- Spawned programmatically by PopulateDungeon() — no world spawn entries needed.
-- =============================================
 
-- -----------------------------------------------
-- Clean slate for vendor NPC
-- -----------------------------------------------
DELETE FROM `npc_vendor`             WHERE `entry` = 500001;
DELETE FROM `creature_template_model` WHERE `CreatureID` = 500001;
DELETE FROM `creature_template`      WHERE `entry` = 500001;
 
-- -----------------------------------------------
-- Vendor NPC Template
-- -----------------------------------------------
-- npcflag  = 6785       (GOSSIP|VENDOR|VENDOR_FOOD|VENDOR_REAGENT|REPAIR)
-- faction  = 35         (Friendly to all — green nameplate)
-- unit_flags = 2        (NON_ATTACKABLE)
-- flags_extra = 2       (CIVILIAN)
INSERT INTO `creature_template` (
    `entry`, `name`, `subname`,
    `minlevel`, `maxlevel`,
    `faction`, `npcflag`,
    `speed_walk`, `speed_run`, `scale`,
    `unit_class`, `unit_flags`, `unit_flags2`,
    `type`, `flags_extra`,
    `ScriptName`
) VALUES (
    500001,
    'Dungeon Provisioner',
    'Roguelike Supplies',
    80, 80,
    35,     -- friendly to all (green nameplate)
    6785,   -- GOSSIP(1) + VENDOR(128) + VENDOR_FOOD(512) + VENDOR_REAGENT(2048) + REPAIR(4096)
    1.0, 1.14286,
    1.0,
    1,      -- warrior (simplest stats)
    2,      -- NON_ATTACKABLE
    0,
    7,      -- humanoid
    2,      -- CIVILIAN
    ''      -- no custom script; built-in vendor/repair handling
);
 
-- -----------------------------------------------
-- Vendor Display Model
-- -----------------------------------------------
-- DisplayID 4307 = Generic merchant (human male with apron)
INSERT INTO `creature_template_model` (
    `CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`
) VALUES (
    500001, 0, 4307, 1.0, 1.0
);
 
-- -----------------------------------------------
-- Vendor Items (npc_vendor)
-- -----------------------------------------------
-- All items have unlimited stock (maxcount=0, incrtime=0).
-- Covers food, water, reagents, ammo, and potions for all level ranges.
 
INSERT INTO `npc_vendor` (`entry`, `slot`, `item`, `maxcount`, `incrtime`, `ExtendedCost`) VALUES
-- =========================================================================
-- FOOD (level-tiered)
-- =========================================================================
(500001, 0, 117,    0, 0, 0),   -- Tough Jerky           (lvl 1)
(500001, 0, 2287,   0, 0, 0),   -- Haunch of Meat        (lvl 5)
(500001, 0, 3770,   0, 0, 0),   -- Mutton Chop           (lvl 15)
(500001, 0, 4599,   0, 0, 0),   -- Cured Ham Steak       (lvl 25)
(500001, 0, 8952,   0, 0, 0),   -- Roasted Quail         (lvl 35)
(500001, 0, 27854,  0, 0, 0),   -- Smoked Talbuk Venison (lvl 55)
(500001, 0, 33449,  0, 0, 0),   -- Crusty Flatbread      (lvl 65)
-- =========================================================================
-- WATER (level-tiered)
-- =========================================================================
(500001, 0, 159,    0, 0, 0),   -- Refreshing Spring Water (lvl 1)
(500001, 0, 1179,   0, 0, 0),   -- Ice Cold Milk           (lvl 5)
(500001, 0, 1205,   0, 0, 0),   -- Melon Juice             (lvl 15)
(500001, 0, 1708,   0, 0, 0),   -- Sweet Nectar            (lvl 25)
(500001, 0, 8766,   0, 0, 0),   -- Morning Glory Dew       (lvl 35)
(500001, 0, 27860,  0, 0, 0),   -- Purified Draenic Water  (lvl 55)
(500001, 0, 33444,  0, 0, 0),   -- Pungent Seal Whey       (lvl 65)
-- =========================================================================
-- REAGENTS (all classes)
-- =========================================================================
(500001, 0, 17020,  0, 0, 0),   -- Arcane Powder          (Mage)
(500001, 0, 17021,  0, 0, 0),   -- Wild Berries           (Druid)
(500001, 0, 17026,  0, 0, 0),   -- Wild Thornroot         (Druid)
(500001, 0, 17028,  0, 0, 0),   -- Holy Candle            (Priest)
(500001, 0, 17029,  0, 0, 0),   -- Sacred Candle          (Priest)
(500001, 0, 17030,  0, 0, 0),   -- Ankh                   (Shaman)
(500001, 0, 17031,  0, 0, 0),   -- Rune of Teleportation  (Mage)
(500001, 0, 17032,  0, 0, 0),   -- Rune of Portals        (Mage)
(500001, 0, 17033,  0, 0, 0),   -- Symbol of Divinity     (Paladin)
(500001, 0, 17034,  0, 0, 0),   -- Maple Seed             (Druid)
(500001, 0, 17035,  0, 0, 0),   -- Stranglethorn Seed     (Druid)
(500001, 0, 17036,  0, 0, 0),   -- Ashwood Seed           (Druid)
(500001, 0, 21177,  0, 0, 0),   -- Symbol of Kings        (Paladin)
(500001, 0, 44615,  0, 0, 0),   -- Devout Candle          (Priest)
-- =========================================================================
-- AMMO — Arrows
-- =========================================================================
(500001, 0, 2512,   0, 0, 0),   -- Rough Arrow            (DPS 2.8)
(500001, 0, 2515,   0, 0, 0),   -- Sharp Arrow            (DPS 4.2)
(500001, 0, 3030,   0, 0, 0),   -- Razor Arrow            (DPS 7.5)
(500001, 0, 11285,  0, 0, 0),   -- Jagged Arrow           (DPS 13.0)
(500001, 0, 28053,  0, 0, 0),   -- Warden's Arrow         (DPS 22.0)
(500001, 0, 41584,  0, 0, 0),   -- Terrorshaft Arrow      (DPS 32.5)
-- =========================================================================
-- AMMO — Bullets
-- =========================================================================
(500001, 0, 2516,   0, 0, 0),   -- Light Shot             (DPS 2.8)
(500001, 0, 2519,   0, 0, 0),   -- Heavy Shot             (DPS 4.2)
(500001, 0, 3033,   0, 0, 0),   -- Solid Shot             (DPS 7.5)
(500001, 0, 11284,  0, 0, 0),   -- Accurate Slugs         (DPS 13.0)
(500001, 0, 28060,  0, 0, 0),   -- Impact Shot            (DPS 22.0)
(500001, 0, 41586,  0, 0, 0),   -- Frostbite Bullets      (DPS 32.5)
-- =========================================================================
-- POTIONS
-- =========================================================================
(500001, 0, 118,    0, 0, 0),   -- Minor Healing Potion
(500001, 0, 929,    0, 0, 0),   -- Healing Potion
(500001, 0, 1710,   0, 0, 0),   -- Greater Healing Potion
(500001, 0, 13446,  0, 0, 0),   -- Major Healing Potion
(500001, 0, 33447,  0, 0, 0),   -- Runic Healing Potion
(500001, 0, 2455,   0, 0, 0),   -- Minor Mana Potion
(500001, 0, 3385,   0, 0, 0),   -- Lesser Mana Potion
(500001, 0, 3827,   0, 0, 0),   -- Mana Potion
(500001, 0, 6149,   0, 0, 0),   -- Greater Mana Potion
(500001, 0, 13444,  0, 0, 0),   -- Major Mana Potion
(500001, 0, 33448,  0, 0, 0);   -- Runic Mana Potion
