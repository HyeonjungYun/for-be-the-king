-- 안티치트 (2026-10-19, design/quick-specs/anticheat-2026-10-19.md · ADR-0019)
--   accounts.banned_until  계정 정지 끝 시각 (NULL = 정지 아님). 인증 서버 로그인 · 게임 서버 C_LOGIN 이 본다
--   cheat_strikes          CheatGuard 가 끊을 때마다 한 줄 - 창(anticheat.csv ban_window_hours) 안의 줄 수로 정지를 정한다
-- 다시 돌려도 된다 (이미 있으면 건너뛴다)
USE forbetheking;

SET @has := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = 'forbetheking' AND TABLE_NAME = 'accounts' AND COLUMN_NAME = 'banned_until');
SET @sql := IF(@has = 0,
    'ALTER TABLE accounts ADD COLUMN banned_until DATETIME NULL DEFAULT NULL',
    'SELECT ''accounts.banned_until already there''');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

CREATE TABLE IF NOT EXISTS cheat_strikes
(
    strike_id       BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    account_id      BIGINT UNSIGNED NOT NULL,
    character_id    BIGINT UNSIGNED NOT NULL,
    created_at      DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,
    score           FLOAT           NOT NULL,
    detail          VARCHAR(255)    NOT NULL,      -- CheatGuard::Summary - "move_speed:31 move_wall:2"

    PRIMARY KEY (strike_id),
    INDEX idx_cheat_strikes_account (account_id, created_at)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;

SELECT (SELECT COUNT(*) FROM cheat_strikes) AS strikes,
       (SELECT COUNT(*) FROM accounts WHERE banned_until > NOW()) AS banned_now;
