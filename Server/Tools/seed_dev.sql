-- 개발자 테스트 계정. 봇(bot_%)과 겹치지 않으므로 봇을 돌리면서 같이 접속할 수 있다.
USE forbetheking;

START TRANSACTION;

-- 재실행 가능하게 먼저 지운다.
-- characters · login_sessions 는 account_id FK ON DELETE CASCADE 라 함께 사라진다
DELETE FROM accounts WHERE username = 'dev_1';

-- password_hash / salt 는 게임 서버가 쓰지 않는다(토큰만 검증).
-- 웹 서버가 생기면 그쪽이 채운다. 지금은 자리만 채워 둔다
INSERT INTO accounts (username, password_hash, password_salt)
VALUES ('dev_1', UNHEX(SHA2('dev_1_placeholder', 256)), UNHEX(REPEAT('00', 16)));

SET @account_id = LAST_INSERT_ID();

INSERT INTO characters (account_id, name, hp, max_hp, floor_id, pos_x, pos_y, pos_z, yaw)
VALUES (@account_id, 'dev_1', 500, 500, 0, 200.0, 200.0, 100.0, 0.0);

-- 토큰 = SHA2('dev_1', 256). 봇과 같은 규칙이라 계산 방식을 기억할 필요가 없다
INSERT INTO login_sessions (token, account_id, expires_at)
VALUES (SHA2('dev_1', 256), @account_id, DATE_ADD(NOW(), INTERVAL 1 YEAR));

COMMIT;

SELECT a.username, c.name, c.floor_id, s.token, s.expires_at
FROM accounts a
JOIN characters c ON c.account_id = a.account_id
JOIN login_sessions s ON s.account_id = a.account_id
WHERE a.username = 'dev_1';
