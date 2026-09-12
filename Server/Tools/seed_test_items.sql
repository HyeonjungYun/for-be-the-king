-- 아이템 검증용 시딩 — 4계정 동시.
--
-- M1(스킬 결속 플레이 검증)용이다. 네 계정에 서로 다른 조합을 주어
-- "장비를 바꾸면 내가 바뀐다"를 비교로 확인할 수 있게 한다.
--
--   dev_1  풀 세트(기준)          무기 스턴 · 갑옷 광역 · 신발 대시
--   bot_1  광역 없음               갑옷을 인벤에서 착용하면 생긴다
--   bot_2  Shadowed 사례          무기가 2001 을 가져가 신발 스킬이 죽어 있다
--   bot_3  대시 없음               신발을 인벤에서 착용하면 생긴다
--
-- 🔴 네 계정 모두 무기에 1001(하드 CC) 을 갖는다 — AC-5 4인 체인 CC 검증에 필요하다.
--
-- 🔴 instance_id 를 900001 부터 쓴다. 런타임 발급(GNextInstanceId)과 섞이지 않게
--    하기 위해서다 — 부팅 시 MAX(instance_id)+1 로 복원되므로 시딩 번호가 작으면
--    런타임 발급이 그 바로 위에서 시작해 로그만 보고 구분할 수 없다.
--    대역: 계정별 900000 + n*100
USE forbetheking;

SET @c0 = (SELECT c.character_id FROM characters c JOIN accounts a ON a.account_id=c.account_id WHERE a.username='dev_1' LIMIT 1);
SET @c1 = (SELECT c.character_id FROM characters c JOIN accounts a ON a.account_id=c.account_id WHERE a.username='bot_1' LIMIT 1);
SET @c2 = (SELECT c.character_id FROM characters c JOIN accounts a ON a.account_id=c.account_id WHERE a.username='bot_2' LIMIT 1);
SET @c3 = (SELECT c.character_id FROM characters c JOIN accounts a ON a.account_id=c.account_id WHERE a.username='bot_3' LIMIT 1);

-- 하나라도 NULL 이면 아래 INSERT 가 chk_owner_xor_bag 위반으로 실패하고
-- 트랜잭션이 통째로 롤백된다. 어느 계정이 없는지는 이 표로 확인한다.
SELECT 'dev_1' acct, @c0 char_id UNION ALL
SELECT 'bot_1',      @c1        UNION ALL
SELECT 'bot_2',      @c2        UNION ALL
SELECT 'bot_3',      @c3;

START TRANSACTION;

DELETE FROM item_instances WHERE owner_character_id IN (@c0, @c1, @c2, @c3);

INSERT INTO item_instances
    (instance_id, item_type_id, grade, level, state, slot,
     owner_character_id, skill_id_primary, skill_id_secondary)
VALUES
    -- ═══ dev_1 — 풀 세트 (기준) ═══════════════════════════════════════
    (900001, 101, 1, 0, 2, 1, @c0, 1001, 0),      -- 무기 COMMON  단일+스턴
    (900002, 201, 1, 0, 2, 4, @c0, 3001, 0),      -- 갑옷 COMMON  광역+슬로우
    (900003, 301, 1, 0, 2, 5, @c0, 2001, 0),      -- 신발 COMMON  대시
    (900004, 102, 2, 0, 1, 0, @c0, 3001, 2001),   -- 인벤 무기 RARE  (2키)
    (900005, 302, 3, 0, 1, 0, @c0, 1001, 0),      -- 인벤 신발 EPIC
    (900006, 401, 1, 0, 1, 0, @c0, 0,    0),      -- 인벤 투구 — 스킬 없음

    -- ═══ bot_1 — 광역 없음 ════════════════════════════════════════════
    (900101, 101, 1, 0, 2, 1, @c1, 1001, 0),      -- 무기 스턴
    (900102, 301, 1, 0, 2, 5, @c1, 2001, 0),      -- 신발 대시
    (900103, 201, 1, 0, 1, 0, @c1, 3001, 0),      -- 인벤 갑옷 — 끼면 광역이 생긴다
    (900104, 102, 2, 0, 1, 0, @c1, 3001, 2001),   -- 인벤 무기 RARE (2키)

    -- ═══ bot_2 — Shadowed 사례 ════════════════════════════════════════
    -- 무기가 1001·2001 두 칸을 채우므로 신발의 2001 이 슬롯2 에 밀린다
    (900201, 102, 2, 0, 2, 1, @c2, 1001, 2001),   -- 무기 RARE — 2키
    (900202, 201, 1, 0, 2, 4, @c2, 3001, 0),      -- 갑옷 광역
    (900203, 301, 1, 0, 2, 5, @c2, 2001, 0),      -- 신발 — 🔴 S<-2 로 죽어 있다
    (900204, 302, 3, 0, 1, 0, @c2, 1001, 0),      -- 인벤 신발 EPIC — 바꾸면 살아난다

    -- ═══ bot_3 — 대시 없음 ════════════════════════════════════════════
    (900301, 101, 1, 0, 2, 1, @c3, 1001, 0),      -- 무기 스턴
    (900302, 201, 1, 0, 2, 4, @c3, 3001, 0),      -- 갑옷 광역
    (900303, 301, 1, 0, 1, 0, @c3, 2001, 0);      -- 인벤 신발 — 끼면 대시가 생긴다

COMMIT;

SELECT owner_character_id, instance_id, state, slot,
       skill_id_primary skill_p, skill_id_secondary skill_s
FROM item_instances
WHERE owner_character_id IN (@c0, @c1, @c2, @c3)
ORDER BY owner_character_id, instance_id;
