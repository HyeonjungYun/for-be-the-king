-- 지급 트랜잭션 + 멱등성.
--
-- 🔴 request_id 가 PRIMARY KEY 인 것이 멱등성의 전부다.
--    같은 요청이 두 번 와도 두 번째 INSERT 가 중복 키(1062)로 실패하고,
--    그 실패가 곧 "이미 지급됨"을 뜻한다. 별도 조회가 필요 없다.
--
--    조회로 먼저 확인하고 없으면 INSERT 하는 방식은 두 요청이 동시에 오면
--    둘 다 "없음"을 보고 둘 다 지급한다. 유일 인덱스만이 이것을 막는다.
--
-- 사용법
--    db schema_reward.sql

USE forbetheking;

-- 🔴 MySQL 8.0 에는 ADD COLUMN IF NOT EXISTS 가 없다 (MariaDB 전용).
--    information_schema 를 보고 조건부로 실행한다 — 이 파일을 여러 번
--    돌려도 안전해야 하기 때문이다.
SET @has_gold = (
    SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = 'forbetheking'
      AND TABLE_NAME   = 'characters'
      AND COLUMN_NAME  = 'gold'
);

SET @sql = IF(@has_gold = 0,
    'ALTER TABLE characters ADD COLUMN gold BIGINT UNSIGNED NOT NULL DEFAULT 0',
    'SELECT ''gold column already exists'' AS note'
);

PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

CREATE TABLE IF NOT EXISTS reward_grants
(
    -- 요청자가 발급한다. 재시도 시 같은 값을 보내야 멱등이 성립한다
    request_id   CHAR(36)        NOT NULL,

    character_id BIGINT UNSIGNED NOT NULL,
    gold         BIGINT          NOT NULL,
    reason       VARCHAR(32)     NOT NULL,
    granted_at   DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,

    PRIMARY KEY (request_id),
    INDEX idx_character (character_id),

    -- 캐릭터가 지워지면 지급 이력도 함께 사라진다
    CONSTRAINT fk_grant_character FOREIGN KEY (character_id)
        REFERENCES characters (character_id) ON DELETE CASCADE
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;

SELECT COUNT(*) grants FROM reward_grants;
SELECT COUNT(*) chars, SUM(gold) total_gold FROM characters;
