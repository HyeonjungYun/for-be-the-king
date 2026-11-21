-- 동접 1만 부하 시험용 봇 계정 bot_121 .. bot_10000 (2026-11-21 · docs/server-scaling-10k.md)
-- seed_bots.sql(bot_1 .. bot_120) 다음에 흘린다. 운영 DB 가 아니라 시험 스키마(fbtk_scale 등)에만:
--   .\Tools\db\setup_db.ps1 -Password … -Database fbtk_scale   그 다음
--   mysql -uroot -p fbtk_scale < Tools\db\seed_bots_scale.sql
-- 토큰 규칙은 seed_bots.sql 과 같다 - SHA2(계정명) 을 봇이 스스로 계산한다
USE forbetheking;

-- MySQL 재귀 CTE 기본 한도는 1000 단계
SET SESSION cte_max_recursion_depth = 20000;

DELETE FROM accounts WHERE username LIKE 'bot\_%' AND CAST(SUBSTRING(username, 5) AS UNSIGNED) > 120;

INSERT INTO accounts (username, password_hash, password_salt)
WITH RECURSIVE seq AS (
    SELECT 121 AS n
    UNION ALL
    SELECT n + 1 FROM seq WHERE n < 10000
)
SELECT CONCAT('bot_', n), UNHEX(REPEAT('00', 32)), UNHEX(REPEAT('00', 16))
FROM seq;

INSERT INTO characters (account_id, name, hp, max_hp, floor_id, pos_x, pos_y, pos_z)
SELECT a.account_id, a.username, 500, 500, 0, 0, 0, 100
FROM accounts a
LEFT JOIN characters c ON c.account_id = a.account_id
WHERE a.username LIKE 'bot\_%' AND c.character_id IS NULL;

INSERT INTO login_sessions (token, account_id, expires_at)
SELECT SHA2(SHA2(a.username, 256), 256), a.account_id, DATE_ADD(NOW(), INTERVAL 365 DAY)
FROM accounts a
LEFT JOIN login_sessions s ON s.account_id = a.account_id
WHERE a.username LIKE 'bot\_%' AND s.account_id IS NULL;

SELECT COUNT(*) AS accounts   FROM accounts       WHERE username LIKE 'bot\_%';
SELECT COUNT(*) AS characters FROM characters     WHERE name     LIKE 'bot\_%';
SELECT COUNT(*) AS tokens     FROM login_sessions;
