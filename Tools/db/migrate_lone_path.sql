-- 고독한 길 옵션 열 (2026-10-17). schema_item.sql 과 같은 자리(skill_id_secondary 뒤)에 넣는다.
-- 하네스 백업 표(bc_items_bak)가 있으면 그것도 - restore.ps1 이 SELECT * 로 되돌리므로 열 순서가 같아야 한다.
-- 다시 돌려도 된다 (이미 있으면 건너뛴다)
USE forbetheking;

SET @has := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = 'forbetheking' AND TABLE_NAME = 'item_instances' AND COLUMN_NAME = 'lone_path');
SET @sql := IF(@has = 0,
    'ALTER TABLE item_instances ADD COLUMN lone_path TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER skill_id_secondary',
    'SELECT ''item_instances.lone_path already there''');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @bak := (SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = 'forbetheking' AND TABLE_NAME = 'bc_items_bak');
SET @hasBak := (SELECT COUNT(*) FROM information_schema.COLUMNS
                WHERE TABLE_SCHEMA = 'forbetheking' AND TABLE_NAME = 'bc_items_bak' AND COLUMN_NAME = 'lone_path');
SET @sql := IF(@bak = 1 AND @hasBak = 0,
    'ALTER TABLE bc_items_bak ADD COLUMN lone_path TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER skill_id_secondary',
    'SELECT ''bc_items_bak skipped''');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SELECT COUNT(*) AS items, SUM(lone_path) AS lone FROM item_instances;
