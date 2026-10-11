-- 2026-10-11 overload (1073) and nihil (1074) move from the staff pool to the helmet pool (user request).
-- A staff that still holds one of them gets another staff skill instead.
-- Run with the game server stopped (item_rows_outside_game_server). Re-running is harmless: only staffs (107) still holding
-- 1073 / 1074 change. Backup of exactly those rows: overload_nihil_items_bak.
--
--   1073 overload -> 1072 radius   (1068 mana bolt if the other slot already has 1072)
--   1074 nihil    -> 1075 blink    (1068 mana bolt if the other slot already has 1075)
--   All three are non-hard-CC, so the E2 rule (at most one hard CC on the weapon keys) still holds.

CREATE TABLE IF NOT EXISTS overload_nihil_items_bak AS
	SELECT * FROM item_instances
	WHERE item_type_id = 107 AND (skill_id_primary IN (1073, 1074) OR skill_id_secondary IN (1073, 1074));

UPDATE item_instances SET skill_id_primary   = IF(skill_id_secondary = 1072, 1068, 1072) WHERE item_type_id = 107 AND skill_id_primary   = 1073;
UPDATE item_instances SET skill_id_secondary = IF(skill_id_primary   = 1072, 1068, 1072) WHERE item_type_id = 107 AND skill_id_secondary = 1073;
UPDATE item_instances SET skill_id_primary   = IF(skill_id_secondary = 1075, 1068, 1075) WHERE item_type_id = 107 AND skill_id_primary   = 1074;
UPDATE item_instances SET skill_id_secondary = IF(skill_id_primary   = 1075, 1068, 1075) WHERE item_type_id = 107 AND skill_id_secondary = 1074;

SELECT COUNT(*) AS staffs_backed_up FROM overload_nihil_items_bak;
SELECT COUNT(*) AS staffs_still_holding_1073_1074 FROM item_instances
	WHERE item_type_id = 107 AND (skill_id_primary IN (1073, 1074) OR skill_id_secondary IN (1073, 1074));
SELECT COUNT(*) AS non_helmet_items_holding_1073_1074 FROM item_instances
	WHERE item_type_id NOT IN (301, 302) AND (skill_id_primary IN (1073, 1074) OR skill_id_secondary IN (1073, 1074));
