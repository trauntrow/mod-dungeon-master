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
-- Spawn coordinates are taken from the default AzerothCore game_tele
-- table entries (the exact positions used by ".tele <CityName>").
-- Each NPC is offset +2.0 on X/Y so it stands *next to* the arrival
-- point rather than on top of teleporting players.
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
-- Coordinates sourced from standard AzerothCore game_tele entries.
-- Format: (guid, id1, map, position_x, position_y, position_z, orientation, spawntimesecs)
--
-- guid range: 8000001-8000011  (high range to avoid collisions)
-- spawntimesecs = 180           (3 min respawn if somehow killed)
-- All Z values verified above ground level for each location.
--
-- Note: This uses the minimal column set compatible with all AzerothCore versions.
--       Optional columns like spawndist, currentwaypoint, and MovementType
--       are not included as they don't exist in all builds.
--
-- Orientation notes:
--   Facing outward from the nearest wall / toward the expected player
--   approach direction. Each city is tuned individually.
--

-- Use minimal column set compatible with all AzerothCore versions
INSERT INTO `creature` (
    `guid`, `id1`, `map`,
    `position_x`, `position_y`, `position_z`, `orientation`,
    `spawntimesecs`
) VALUES
-- =========================================================================
-- ALLIANCE CITIES
-- =========================================================================

-- Stormwind City  (game_tele "Stormwind": -8833.38, 628.628, 94.00, map 0)
-- Trade District arrival area, offset to stand beside the landing zone
(8000001, 500000, 0,  -8831.38, 630.628, 94.0066, 3.6,  180),

-- Ironforge  (game_tele "Ironforge": -4981.25, -881.542, 501.66, map 0)
-- The Commons, central hub near the forge
(8000002, 500000, 0,  -4979.25, -879.542, 501.6536, 3.4,  180),

-- Darnassus  (game_tele "Darnassus": 9869.91, 2493.58, 1315.88, map 1)
-- Tradesman's Terrace arrival zone
(8000003, 500000, 1,  9871.91, 2495.58, 1315.882, 3.1,  180),

-- Exodar  (game_tele "Exodar": -3965.7, -11653.6, -138.844, map 530)
-- The Crystal Hall - central area
(8000004, 500000, 530, -3963.7, -11651.6, -138.844, 3.14,  180),

-- =========================================================================
-- HORDE CITIES
-- =========================================================================

-- Orgrimmar  (game_tele "Orgrimmar": 1676.21, -4315.29, 61.52, map 1)
-- Valley of Strength arrival point
(8000005, 500000, 1,  1678.21, -4313.29, 61.5222, 5.7,  180),

-- Thunder Bluff  (game_tele "ThunderBluff": -1277.37, 124.804, 131.287, map 1)
-- High Rise, central platform
(8000006, 500000, 1,  -1275.37, 126.804, 131.2866, 4.7,  180),

-- Undercity  (game_tele "Undercity": 1586.48, 239.562, -52.149, map 0)
-- Trade Quarter, below the ruins of Lordaeron
(8000007, 500000, 0,  1588.48, 241.562, -52.149, 0.6,  180),

-- Silvermoon City  (game_tele "SilvermoonCity": 9338.74, -7277.27, 13.7895, map 530)
-- The Bazaar, central thoroughfare
(8000008, 500000, 530, 9340.74, -7275.27, 13.7895, 0.5,  180),

-- =========================================================================
-- NEUTRAL CITIES
-- =========================================================================

-- Shattrath City  (game_tele "Shattrath": -1850.21, 5435.82, -12.4281, map 530)
-- Lower City / Terrace of Light approach
(8000009, 500000, 530, -1848.21, 5437.82, -12.4281, 5.5,  180),

-- Dalaran  (game_tele "Dalaran": 5804.14, 624.771, 647.767, map 571)
-- The Eventide, central platform
(8000010, 500000, 571, 5806.14, 626.771, 647.7672, 3.8,  180),

-- Booty Bay  (game_tele "BootyBay": -14406.6, 419.353, 23.3907, map 0)
-- Docks area, near the inn arrival point
(8000011, 500000, 0,  -14404.6, 421.353, 23.3907, 4.0,  180);
