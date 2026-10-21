CREATE DATABASE IF NOT EXISTS forbetheking
    DEFAULT CHARACTER SET utf8mb4
    DEFAULT COLLATE utf8mb4_general_ci;

USE forbetheking;

CREATE TABLE IF NOT EXISTS accounts
(
    account_id      BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    username        VARCHAR(32)     NOT NULL,

    -- PBKDF2-SHA256. 평문 저장은 하지 않는다
    password_hash   BINARY(32)      NOT NULL,
    password_salt   BINARY(16)      NOT NULL,

    created_at      DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,
    last_login_at   DATETIME        NULL,

    PRIMARY KEY (account_id),
    UNIQUE KEY uk_username (username)
) ENGINE = InnoDB;

CREATE TABLE IF NOT EXISTS characters
(
    character_id    BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    account_id      BIGINT UNSIGNED NOT NULL,
    name            VARCHAR(32)     NOT NULL,

    -- 맨몸 기준값. 장비가 생기면 max_hp 는 장비 합산으로 바뀐다
    hp              INT             NOT NULL DEFAULT 500,
    max_hp          INT             NOT NULL DEFAULT 500,

    -- 마지막으로 있던 위치. 재접속 시 여기서 시작한다
    floor_id        INT UNSIGNED    NOT NULL DEFAULT 0,
    pos_x           FLOAT           NOT NULL DEFAULT 0,
    pos_y           FLOAT           NOT NULL DEFAULT 0,
    pos_z           FLOAT           NOT NULL DEFAULT 100,
    yaw             FLOAT           NOT NULL DEFAULT 0,

    created_at      DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at      DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP
                                    ON UPDATE CURRENT_TIMESTAMP,

    PRIMARY KEY (character_id),
    UNIQUE KEY uk_name (name),
    KEY idx_account (account_id),

    CONSTRAINT fk_char_account
        FOREIGN KEY (account_id) REFERENCES accounts (account_id)
        ON DELETE CASCADE
) ENGINE = InnoDB;

CREATE TABLE IF NOT EXISTS login_sessions
(
    token           CHAR(64)        NOT NULL,       -- 랜덤 32바이트를 hex 로
    account_id      BIGINT UNSIGNED NOT NULL,

    issued_at       DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,
    expires_at      DATETIME        NOT NULL,

    PRIMARY KEY (token),
    KEY idx_account (account_id),
    KEY idx_expires (expires_at),

    CONSTRAINT fk_session_account
        FOREIGN KEY (account_id) REFERENCES accounts (account_id)
        ON DELETE CASCADE
) ENGINE = InnoDB;
