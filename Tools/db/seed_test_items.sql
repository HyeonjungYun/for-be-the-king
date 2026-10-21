-- 아이템 검증용 시딩 — 4계정 동시. (2026-10-10 스킬 로스터 id 로 갱신 - Server/Data/skills.csv)
--
-- M1(스킬 결속 플레이 검증)용이다. 네 계정에 서로 다른 조합을 주어
-- "장비를 바꾸면 내가 바뀐다"를 비교로 확인할 수 있게 한다.
--
--   dev_1  풀 세트(기준)          검(기습 · 휘두르기) · 갑옷 · 신발 질주
--   bot_1  휘두르기 없음           인벤의 다른 검을 끼면 Q 가 바뀐다
--   bot_2  Shadowed 사례          무기 Q 가 1003 을 가져가 신발 스킬이 죽어 있다
--   bot_3  질주 없음               신발을 인벤에서 착용하면 생긴다
--
-- 🔴 네 계정 모두 무기에 1007 기습(하드 CC 기절) 을 갖는다 — AC-5 4인 체인 CC 검증에 필요하다.
--
-- 🔴 instance_id 를 900001 부터 쓴다. 런타임 발급(GNextInstanceId)과 섞이지 않게
--    하기 위해서다 — 부팅 시 MAX(instance_id)+1 로 복원되므로 시딩 번호가 작으면
--    런타임 발급이 그 바로 위에서 시작해 로그만 보고 구분할 수 없다.
--    대역: 계정별 900000 + n*100
-- 🔴 서버를 끈 채로 (item_rows_outside_game_server - docs/registry/architecture.yaml)
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
    (900001, 101, 1, 0, 2, 1, @c0, 1007, 1004),   -- 검 COMMON   기습(단일 + 기절) · 휘두르기(부채꼴)
    (900002, 401, 1, 0, 2, 4, @c0, 0,    0),      -- 갑옷 COMMON 스킬 없음 (갑옷 풀 비어 있음)
    (900003, 501, 1, 0, 2, 5, @c0, 1003, 0),      -- 신발 COMMON 질주
    (900004, 102, 2, 0, 1, 0, @c0, 1005, 1026),   -- 인벤 망치 RARE  (머리 부수기 · 내리찍기)
    (900005, 502, 3, 0, 1, 0, @c0, 1003, 0),      -- 인벤 신발 EPIC
    (900006, 301, 1, 0, 1, 0, @c0, 1002, 0),      -- 인벤 투구 — 유령 신랑의 저주

    -- ═══ bot_1 — 휘두르기 없음 ═══════════════════════════════════════
    (900101, 101, 1, 0, 2, 1, @c1, 1007, 1010),   -- 검 기습 · 검의 환희
    (900102, 501, 1, 0, 2, 5, @c1, 1003, 0),      -- 신발 질주
    (900103, 401, 1, 0, 1, 0, @c1, 0,    0),      -- 인벤 갑옷
    (900104, 101, 2, 0, 1, 0, @c1, 1007, 1004),   -- 인벤 검 RARE — 끼면 Q 가 휘두르기

    -- ═══ bot_2 — Shadowed 사례 ════════════════════════════════════════
    -- 무기가 1007 · 1003 두 칸을 채우므로 신발의 1003 이 슬롯2 에 밀린다 (풀 밖 조합 - 시드 전용)
    (900201, 101, 2, 0, 2, 1, @c2, 1007, 1003),   -- 검 RARE — 2키
    (900202, 401, 1, 0, 2, 4, @c2, 0,    0),      -- 갑옷
    (900203, 501, 1, 0, 2, 5, @c2, 1003, 0),      -- 신발 — 🔴 S<-2 로 죽어 있다
    (900204, 502, 3, 0, 1, 0, @c2, 1003, 0),      -- 인벤 신발 EPIC — 바꿔도 같은 스킬이라 그대로 죽어 있다

    -- ═══ bot_3 — 질주 없음 ════════════════════════════════════════════
    (900301, 101, 1, 0, 2, 1, @c3, 1007, 1004),   -- 검
    (900302, 401, 1, 0, 2, 4, @c3, 0,    0),      -- 갑옷
    (900303, 501, 1, 0, 1, 0, @c3, 1003, 0);      -- 인벤 신발 — 끼면 질주가 생긴다
UPDATE characters SET pos_x = 0, pos_y = 0, pos_z = 100, hp = max_hp
WHERE character_id IN (@c0, @c1, @c2, @c3);

COMMIT;

SELECT owner_character_id, instance_id, state, slot,
       skill_id_primary skill_p, skill_id_secondary skill_s
FROM item_instances
WHERE owner_character_id IN (@c0, @c1, @c2, @c3)
ORDER BY owner_character_id, instance_id;
