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

-- 토큰 = SHA2(계정명, 256)
--   🔴 봇이 자기 번호로 토큰을 계산할 수 있게 하려는 것이다. 서버에 우회를 넣지 않고도
--      봇 120개가 각자 로그인한다. 실제 웹 서버는 예측 불가능한 난수를 쓴다
INSERT INTO login_sessions (token, account_id, expires_at)
SELECT SHA2(username, 256), account_id, DATE_ADD(NOW(), INTERVAL 365 DAY)
FROM accounts
WHERE username LIKE 'bot\_%';

SELECT COUNT(*) AS accounts   FROM accounts       WHERE username LIKE 'bot\_%';
SELECT COUNT(*) AS characters FROM characters     WHERE name     LIKE 'bot\_%';
SELECT COUNT(*) AS tokens     FROM login_sessions;
