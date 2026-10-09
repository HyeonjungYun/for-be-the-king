-- 2026-10-10 skill roster swap (src/skill list, 76 skills) - move old skill ids on existing rows to the new table.
-- Run once with the server stopped. Re-running is harmless: every UPDATE keys on the OLD ids. Backup: skillmig_items_bak.
--
--   old                                  new (Server/Data/skills.csv - skill_pools.csv)
--   1001 (weapon Shift)                  per weapon type: Shift / Q pair
--   2001 (boots dash)                    1003
--   3001 (armor area slow)               armor pool is empty -> 0, trinket -> 1002
--   weapon 101..104 keep their ids - only the meaning changes (101 sword, 102 hammer, 103 spear, 104 dagger)
--
-- Do not run while the game server is up (item_rows_outside_game_server, docs/registry/architecture.yaml).

CREATE TABLE IF NOT EXISTS skillmig_items_bak AS SELECT * FROM item_instances;

UPDATE item_instances SET skill_id_primary = 1007, skill_id_secondary = 1004 WHERE item_type_id = 101 AND skill_id_primary IN (1001, 2001, 3001);
UPDATE item_instances SET skill_id_primary = 1005, skill_id_secondary = 1026 WHERE item_type_id = 102 AND skill_id_primary IN (1001, 2001, 3001);
UPDATE item_instances SET skill_id_primary = 1018, skill_id_secondary = 1016 WHERE item_type_id = 103 AND skill_id_primary IN (1001, 2001, 3001);
UPDATE item_instances SET skill_id_primary = 1036, skill_id_secondary = 1035 WHERE item_type_id = 104 AND skill_id_primary IN (1001, 2001, 3001);

UPDATE item_instances SET skill_id_primary = 1002, skill_id_secondary = 0 WHERE item_type_id IN (301, 302, 601) AND skill_id_primary IN (0, 1001, 2001, 3001);
UPDATE item_instances SET skill_id_primary = 1003, skill_id_secondary = 0 WHERE item_type_id IN (501, 502) AND skill_id_primary IN (1001, 2001, 3001);
UPDATE item_instances SET skill_id_primary = 0,    skill_id_secondary = 0 WHERE item_type_id IN (401, 402) AND skill_id_primary IN (1001, 2001, 3001);

SELECT COUNT(*) AS old_ids_left FROM item_instances WHERE skill_id_primary IN (1001, 2001, 3001) OR skill_id_secondary IN (1001, 2001, 3001);
SELECT item_type_id, skill_id_primary, skill_id_secondary, COUNT(*) AS n FROM item_instances GROUP BY 1, 2, 3 ORDER BY 1, 2, 3;
