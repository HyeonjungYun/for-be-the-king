-- 아이템 종류 id 체계 이전 — 2026-10 (design/quick-specs/floor-item-pool-mvp-2026-10-02.md S-6)
--
-- 옛 시드 체계: 1xx 무기 · 2xx 갑옷 · 3xx 신발 · 4xx 투구
-- 새 체계:      item_type_id = EquipSlot × 100 + 일련번호
--               1xx 무기 · 3xx 투구 · 4xx 갑옷 · 5xx 신발 · 6xx 장신구 (2xx 는 비운다)
--
-- 🔴 401 이 두 의미로 겹친다(옛 투구 → 새 갑옷). 순차 UPDATE 를 쓰면 201→401 로 바뀐 행이
--    다음 문장의 401→301 에 다시 걸려 갑옷이 투구가 된다. 반드시 CASE 한 문장으로 바꾼다.
--
-- 🔴 한 번만 돌린다. 두 번 돌리면 새 401(갑옷)이 301(투구)이 된다.
--    가드: 옛 체계에만 있는 2xx 가 하나도 없으면 아무것도 바꾸지 않는다.
--
-- 사용법 (PowerShell)
--    Get-Content .\Server\Tools\migrate_item_type_ids.sql |
--        & "C:\mysql-8.0.46-winx64\bin\mysql.exe" -u root -p -D forbetheking
USE forbetheking;

SET @old = (SELECT COUNT(*) FROM item_instances WHERE item_type_id BETWEEN 200 AND 299);
SELECT @old AS old_scheme_rows_2xx;

START TRANSACTION;

UPDATE item_instances
SET item_type_id = CASE item_type_id
        WHEN 201 THEN 401   -- 갑옷 → 두정갑(중갑)
        WHEN 301 THEN 501   -- 신발 → 짚신(질주형)
        WHEN 302 THEN 502   -- 신발 → 목화(지구형)
        WHEN 401 THEN 301   -- 투구 → 전립(정찰형)
        ELSE item_type_id
    END
WHERE @old > 0
  AND item_type_id IN (201, 301, 302, 401);

COMMIT;

-- 확인 1: 남은 종류 id. 2xx 가 없어야 한다
SELECT item_type_id, COUNT(*) n FROM item_instances GROUP BY item_type_id ORDER BY item_type_id;

-- 확인 2: 착용 중인 행은 첫 자리가 슬롯과 같아야 한다 (무기 1 · 갑옷 4 · 신발 5). 0 이어야 한다
SELECT COUNT(*) AS equipped_slot_mismatch FROM item_instances WHERE state = 2 AND item_type_id DIV 100 <> slot;
