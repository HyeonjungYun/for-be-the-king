-- 착용 칸 유일성 (2026-10-21 리뷰 후속).
--   같은 캐릭터 · 같은 칸에 EQUIPPED(state 2) 행이 둘이면 입장 적재가 뒤 행으로 앞 행을 덮어썼고(메모리에서만 사라짐),
--   탈출 커밋의 owned count mismatch 는 로그만 남겼다. 표 자체가 막게 한다.
--   MySQL 은 부분 인덱스가 없으므로 생성 열로 만든다: state = 2 일 때만 slot, 아니면 NULL (UNIQUE 는 NULL 을 여러 개 허용한다).
-- 다시 돌려도 된다 (열 · 인덱스가 있으면 건너뛴다). 이미 중복이 있는 DB 에서는 ALTER 가 실패한다 - 먼저 아래 SELECT 로 찾아 손으로 정리한다:
--   SELECT owner_character_id, slot, COUNT(*) FROM item_instances WHERE state = 2 GROUP BY owner_character_id, slot HAVING COUNT(*) > 1;
USE forbetheking;

SET @has_col := (SELECT COUNT(*) FROM information_schema.COLUMNS
                 WHERE TABLE_SCHEMA = 'forbetheking' AND TABLE_NAME = 'item_instances' AND COLUMN_NAME = 'equip_slot_key');
SET @sql := IF(@has_col = 0,
    'ALTER TABLE item_instances ADD COLUMN equip_slot_key TINYINT UNSIGNED GENERATED ALWAYS AS (IF(state = 2, slot, NULL)) STORED',
    'SELECT ''item_instances.equip_slot_key already there''');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @has_idx := (SELECT COUNT(*) FROM information_schema.STATISTICS
                 WHERE TABLE_SCHEMA = 'forbetheking' AND TABLE_NAME = 'item_instances' AND INDEX_NAME = 'uk_equip_slot');
SET @sql := IF(@has_idx = 0,
    'ALTER TABLE item_instances ADD UNIQUE KEY uk_equip_slot (owner_character_id, equip_slot_key)',
    'SELECT ''uk_equip_slot already there''');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SELECT COUNT(*) AS duplicate_equip_slots FROM (
    SELECT owner_character_id, slot FROM item_instances WHERE state = 2 GROUP BY owner_character_id, slot HAVING COUNT(*) > 1
) d;
