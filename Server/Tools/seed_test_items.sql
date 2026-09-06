USE forbetheking;

SET @account_name = 'dev_1';

SET @char_id = (
    SELECT c.character_id FROM characters c
    JOIN accounts a ON a.account_id = c.account_id
    WHERE a.username = @account_name LIMIT 1);

-- 캐릭터가 없으면 조용히 0행을 넣고 끝나 버린다. 그건 디버깅이 어렵다
SELECT IF(@char_id IS NULL,
    CONCAT(@account_name, ' 의 캐릭터가 없다 — seed_auth_character.sql 을 먼저 하라'),
    CONCAT('ok - character_id=', @char_id)) AS precheck;

START TRANSACTION;

-- 재실행 가능하게. 이 캐릭터 아이템만 지운다
DELETE FROM item_instances WHERE owner_character_id = @char_id;

INSERT INTO item_instances
    (instance_id, item_type_id, grade, level, state, slot,
     owner_character_id, skill_id_primary, skill_id_secondary)
VALUES
    -- ── 착용 (state=2 EQUIPPED) — 옛 하드코딩 재현 ──────────────
    (1, 101, 1, 0, 2, 1, @char_id, 1001, 0),   -- 무기   COMMON
    (2, 201, 1, 0, 2, 4, @char_id, 3001, 0),   -- 갑옷   COMMON
    (3, 301, 1, 0, 2, 5, @char_id, 2001, 0),   -- 신발   COMMON

    -- ── 인벤 (state=1 CARRIED) — 착용/해제 검증용 ───────────────
    -- 🔴 4번은 Shadowed 검증기다. 착용하면 무기가 3001·2001 을 가져가
    --    이미 그 스킬을 주고 있는 갑옷(2)·신발(3)이 SHADOWED 로 밀린다
    (4, 102, 2, 0, 1, 0, @char_id, 3001, 2001),   -- 무기   RARE
    (5, 302, 3, 0, 1, 0, @char_id, 1001, 0),      -- 신발   EPIC
    (6, 401, 1, 0, 1, 0, @char_id, 0,    0);      -- 투구   COMMON · 스킬 없음

COMMIT;

SELECT instance_id, item_type_id, grade, state, slot,
       skill_id_primary AS skill_p, skill_id_secondary AS skill_s
FROM item_instances WHERE owner_character_id = @char_id
ORDER BY instance_id;
