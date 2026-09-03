-- 아이템 & 장비 스키마 
USE forbetheking;

CREATE TABLE IF NOT EXISTS item_instances
(
    instance_id         BIGINT UNSIGNED NOT NULL,
    item_type_id        INT UNSIGNED    NOT NULL,
    grade               TINYINT UNSIGNED NOT NULL,
    level               INT UNSIGNED    NOT NULL DEFAULT 0,
    state               TINYINT UNSIGNED NOT NULL,
    slot                TINYINT UNSIGNED NOT NULL DEFAULT 0,

    -- 소유자. 가방에 있으면 NULL 이고 bag_id 가 채워진다
    owner_character_id  BIGINT UNSIGNED NULL,
    bag_id              BIGINT UNSIGNED NULL,

    -- 무기는 2개. equipment-skill-binding B2 — 인스턴스 데이터다
    skill_id_primary    INT UNSIGNED    NOT NULL DEFAULT 0,
    skill_id_secondary  INT UNSIGNED    NOT NULL DEFAULT 0,

    created_at          DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at          DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP
                                        ON UPDATE CURRENT_TIMESTAMP,

    PRIMARY KEY (instance_id),
    INDEX idx_owner (owner_character_id),
    INDEX idx_bag (bag_id),

    -- 🔴 소유자와 가방은 상호배타다. 둘 다 NULL 이면 고아 행이다
    CONSTRAINT chk_owner_xor_bag CHECK (
        (owner_character_id IS NOT NULL AND bag_id IS NULL) OR
        (owner_character_id IS NULL     AND bag_id IS NOT NULL)
    )
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;

CREATE TABLE IF NOT EXISTS bags
(
    bag_id      BIGINT UNSIGNED NOT NULL,   -- = object_id (20억~, ADR-0004)
    floor_id    INT UNSIGNED    NOT NULL,
    x           FLOAT           NOT NULL,
    y           FLOAT           NOT NULL,
    z           FLOAT           NOT NULL,
    created_at  DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,
    expires_at  DATETIME        NOT NULL,   -- created_at + 10분

    PRIMARY KEY (bag_id),
    INDEX idx_expires (expires_at)          -- 부팅 시 만료 정리에 쓴다
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;

SELECT COUNT(*) items FROM item_instances;
SELECT COUNT(*) bags  FROM bags;
