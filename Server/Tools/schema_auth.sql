USE forbetheking;

-- PBKDF2 반복 횟수 열 (ADR-0011). 다시 돌려도 된다 - 열이 이미 있으면 건너뛴다 (2026-10-18, setup_db.ps1 전체 순서)
SET @has := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'accounts' AND COLUMN_NAME = 'password_iterations');
SET @sql := IF(@has = 0,
    'ALTER TABLE accounts ADD COLUMN password_iterations INT UNSIGNED NOT NULL DEFAULT 600000 AFTER password_salt',
    'SELECT ''password_iterations already present'' AS note');
PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
