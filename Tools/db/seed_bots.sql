USE forbetheking;

-- 재실행 가능하게. CASCADE 로 characters · login_sessions 도 함께 지워진다
DELETE FROM accounts WHERE username LIKE 'bot\_%';

INSERT INTO accounts (username, password_hash, password_salt)
WITH RECURSIVE seq AS (
    SELECT 1 AS n
    UNION ALL
    SELECT n + 1 FROM seq WHERE n < 120
)
SELECT CONCAT('bot_', n), UNHEX(REPEAT('00', 32)), UNHEX(REPEAT('00', 16))
FROM seq;

-- 계정당 캐릭터 1개. 이름은 계정명과 같게
INSERT INTO characters (account_id, name, hp, max_hp, floor_id, pos_x, pos_y, pos_z)
SELECT account_id, username, 500, 500, 0, 0, 0, 100
FROM accounts
WHERE username LIKE 'bot\_%';

-- 토큰 = SHA2(계정명, 256). DB 에는 그 토큰의 해시 SHA2(토큰, 256) 를 넣는다 (2026-10-17 토큰 해시 저장 - migrate_hash_tokens.sql)
--   🔴 봇이 자기 번호로 토큰을 계산할 수 있게 하려는 것이다. 서버에 우회를 넣지 않고도
--      봇 120개가 각자 로그인한다. 실제 웹 서버는 예측 불가능한 난수를 쓴다
INSERT INTO login_sessions (token, account_id, expires_at)
SELECT SHA2(SHA2(username, 256), 256), account_id, DATE_ADD(NOW(), INTERVAL 365 DAY)
FROM accounts
WHERE username LIKE 'bot\_%';

-- 이 시드는 처음부터 해시로 넣는다 - 토큰 해시 이전 스크립트가 다시 해시하지 않게 적용 표시를 남긴다
CREATE TABLE IF NOT EXISTS schema_migrations (name VARCHAR(64) NOT NULL PRIMARY KEY, applied_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP);
INSERT IGNORE INTO schema_migrations (name) VALUES ('hash_login_tokens');

SELECT COUNT(*) AS accounts   FROM accounts       WHERE username LIKE 'bot\_%';
SELECT COUNT(*) AS characters FROM characters     WHERE name     LIKE 'bot\_%';
SELECT COUNT(*) AS tokens     FROM login_sessions;
