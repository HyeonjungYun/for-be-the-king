-- 경제 1차 (2026-10-17): 전리품 겹침 개수 열. schema_item.sql 과 같은 자리(lone_path 뒤). 다시 돌려도 된다
USE forbetheking;

SET @has := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = 'forbetheking' AND TABLE_NAME = 'item_instances' AND COLUMN_NAME = 'stack_count');
SET @sql := IF(@has = 0,
    'ALTER TABLE item_instances ADD COLUMN stack_count INT UNSIGNED NOT NULL DEFAULT 1 AFTER lone_path',
    'SELECT ''item_instances.stack_count already there''');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

-- 하네스 백업 표 (restore.ps1 이 SELECT * 로 되돌린다 - 열 순서가 같아야 한다)
SET @bak := (SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = 'forbetheking' AND TABLE_NAME = 'bc_items_bak');
SET @hasBak := (SELECT COUNT(*) FROM information_schema.COLUMNS
                WHERE TABLE_SCHEMA = 'forbetheking' AND TABLE_NAME = 'bc_items_bak' AND COLUMN_NAME = 'stack_count');
SET @sql := IF(@bak = 1 AND @hasBak = 0,
    'ALTER TABLE bc_items_bak ADD COLUMN stack_count INT UNSIGNED NOT NULL DEFAULT 1 AFTER lone_path',
    'SELECT ''bc_items_bak skipped''');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SELECT COUNT(*) AS items, SUM(stack_count) AS stacked FROM item_instances;