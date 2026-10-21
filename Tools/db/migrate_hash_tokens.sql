-- 로그인 토큰 해시 저장 (2026-10-17 사용자 결정 · account-login-mvp OQ-2).
-- login_sessions.token 에 원래 토큰 대신 SHA2(토큰, 256) 을 둔다. 게임 서버(C_LOGIN) · 인증 서버(AuthServer)가 받은 토큰을 같은 식으로 해시해 비교한다.
-- 🔴 한 번만 적용해야 한다 - 두 번 돌면 해시의 해시가 되어 모든 토큰이 무효가 된다. schema_migrations 표시로 막는다 (다시 돌려도 된다)
USE forbetheking;

CREATE TABLE IF NOT EXISTS schema_migrations
(
    name        VARCHAR(64) NOT NULL PRIMARY KEY,
    applied_at  DATETIME    NOT NULL DEFAULT CURRENT_TIMESTAMP
);

START TRANSACTION;

SET @done := (SELECT COUNT(*) FROM schema_migrations WHERE name = 'hash_login_tokens');

UPDATE login_sessions SET token = SHA2(token, 256) WHERE @done = 0;
SELECT ROW_COUNT() AS tokens_hashed;

INSERT IGNORE INTO schema_migrations (name) VALUES ('hash_login_tokens');

COMMIT;

SELECT name, applied_at FROM schema_migrations WHERE name = 'hash_login_tokens';
