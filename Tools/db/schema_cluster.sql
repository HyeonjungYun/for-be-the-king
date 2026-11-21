-- 서버 여러 대 (2026-11-22 동접 1만 2 단계 - docs/server-scaling-10k.md § 5)
--   game_servers     게임 서버 등록부 - 서버마다 5 초에 한 번 인원 · 채널 수를 적는다. 인증 서버의 방 배정(/lobby/assign)이 읽는다
--   online_accounts  지금 접속한 계정이 어느 서버에 있나 - 한 계정은 한 서버에만. 다른 서버가 kick_requested 를 세우면 그 서버가 1 초 안에 옛 연결을 끊는다
-- 공용 저장소를 따로(Redis 등) 두지 않고 이미 모든 서버가 붙는 MySQL 에 둔다 - 의존성 하나를 늘리지 않는다
-- 다시 돌려도 된다
USE forbetheking;

CREATE TABLE IF NOT EXISTS game_servers
(
    server_id       INT UNSIGNED    NOT NULL,
    host            VARCHAR(64)     NOT NULL,
    port            INT UNSIGNED    NOT NULL,
    admin_port      INT UNSIGNED    NOT NULL DEFAULT 0,
    ccu             INT UNSIGNED    NOT NULL DEFAULT 0,
    max_sessions    INT UNSIGNED    NOT NULL DEFAULT 0,
    channels        INT UNSIGNED    NOT NULL DEFAULT 0,
    status          VARCHAR(16)     NOT NULL DEFAULT 'up',       -- up · down
    started_at      DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at      DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,

    PRIMARY KEY (server_id)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;

CREATE TABLE IF NOT EXISTS online_accounts
(
    account_id      BIGINT UNSIGNED NOT NULL,
    server_id       INT UNSIGNED    NOT NULL,
    character_id    BIGINT UNSIGNED NOT NULL DEFAULT 0,
    kick_requested  TINYINT         NOT NULL DEFAULT 0,
    since           DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,

    PRIMARY KEY (account_id),
    INDEX idx_online_server (server_id, kick_requested)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;

SELECT (SELECT COUNT(*) FROM game_servers) AS servers, (SELECT COUNT(*) FROM online_accounts) AS online;
