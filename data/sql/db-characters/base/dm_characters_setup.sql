-- =============================================
-- Dungeon Master Module - Characters DB Setup
-- =============================================
-- Run this on your CHARACTERS database (not world).
-- Creates tables for persistent player statistics
-- and dungeon clear-time leaderboards.
-- =============================================

CREATE TABLE IF NOT EXISTS `dm_player_stats` (
    `guid`               INT UNSIGNED NOT NULL,
    `total_runs`         INT UNSIGNED NOT NULL DEFAULT 0,
    `completed_runs`     INT UNSIGNED NOT NULL DEFAULT 0,
    `failed_runs`        INT UNSIGNED NOT NULL DEFAULT 0,
    `total_mobs_killed`  INT UNSIGNED NOT NULL DEFAULT 0,
    `total_bosses_killed` INT UNSIGNED NOT NULL DEFAULT 0,
    `total_deaths`       INT UNSIGNED NOT NULL DEFAULT 0,
    `fastest_clear`      INT UNSIGNED NOT NULL DEFAULT 0,  -- seconds (across all dungeons)
    PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `dm_leaderboard` (
    `id`            INT UNSIGNED NOT NULL AUTO_INCREMENT,
    `guid`          INT UNSIGNED NOT NULL,
    `char_name`     VARCHAR(48)  NOT NULL DEFAULT '',
    `map_id`        INT UNSIGNED NOT NULL,
    `difficulty_id` INT UNSIGNED NOT NULL,
    `clear_time`    INT UNSIGNED NOT NULL,  -- seconds
    `party_size`    TINYINT UNSIGNED NOT NULL DEFAULT 1,
    `scaled`        TINYINT UNSIGNED NOT NULL DEFAULT 0,  -- 1 = scaled to party
    `completed_at`  DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    INDEX `idx_map_diff` (`map_id`, `difficulty_id`, `clear_time`),
    INDEX `idx_guid` (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
