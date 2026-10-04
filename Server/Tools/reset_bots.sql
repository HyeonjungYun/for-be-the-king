-- 부하 측정 전 봇 초기 상태 리셋.
--
-- 🔴 왜 필요한가
--    캐릭터 영속화(8ec324d) 이후 봇이 퇴장 위치를 저장한다. 리셋하지 않으면
--    측정이 이전 측정의 결과를 물려받는다 — 봇은 서로에게 몰려들어 뭉치고,
--    뭉치면 서로 기절시켜 멈추고, 멈추면 이동 패킷을 보내지 않는다.
--
--    흩뿌린 직후   STDDEV(pos_x) ~1100     per-client 125 kbps
--    3회 실행 후   STDDEV(pos_x)   247     per-client   6.4 kbps
--
--    수치만 보면 부하가 19배 준 것처럼 보이지만 봇이 멈춘 것이다.
--    상세: production/roadmap.md § 0-4
--
-- 🔴 2026-10-05 부터 입장 위치는 진입 지점(entry_points.csv)이다. 여기서 흩뿌린 좌표를 쓰려면
--    서버를 FBTK_ENTRY_FROM_DB=1 로 띄울 것 — 아니면 봇 120명이 층마다 5곳에 몰려 이전 측정과 비교가 안 된다.
--
-- 사용법 (PowerShell)
--    Get-Content .\Server\Tools\reset_bots.sql |
--        & "C:\mysql-8.0.46-winx64\bin\mysql.exe" -u root -p -D forbetheking

USE forbetheking;

UPDATE characters
SET hp    = max_hp,
    pos_x = -2000 + RAND() * 4000,
    pos_y = -2000 + RAND() * 4000,
    pos_z = 100,
    yaw   = RAND() * 360
WHERE name LIKE 'bot\_%';

START TRANSACTION;

-- 🔴 가방은 전부 지운다 (2026-10-04, G2)
--    봇이 죽으면 장비가 사망 가방으로 간다(owner = NULL, bag_id = X). 아래 주인 기준 DELETE 는 이 행을
--    못 지워서 INSERT 가 같은 instance_id 로 부딪힌다. bags 에는 주인 칼럼이 없어 봇 것만 고를 수 없다 -
--    측정 DB 전용 스크립트라 가방은 전부 지운다. 회차마다 가방 0 에서 시작해야 A/B 비교도 공정하다.
DELETE FROM item_instances WHERE bag_id IS NOT NULL;
DELETE FROM bags;

DELETE i FROM item_instances i
JOIN characters c ON c.character_id = i.owner_character_id
WHERE c.name LIKE 'bot\_%';

INSERT INTO item_instances
    (instance_id, item_type_id, grade, level, state, slot,
     owner_character_id, skill_id_primary, skill_id_secondary)
SELECT 800000 + c.character_id * 10 + k.n, k.type_id, 1, 0, 2, k.slot,
       c.character_id, k.skill_id, 0
FROM characters c
CROSS JOIN (
    SELECT 1 AS n, 101 AS type_id, 1 AS slot, 1001 AS skill_id UNION ALL   -- 무기 · 단일 스턴
    SELECT 2,      401,            4,         3001             UNION ALL   -- 갑옷 · 광역 감속
    SELECT 3,      501,            5,         2001                          -- 신발 · 대시
) k
WHERE c.name LIKE 'bot\_%';

COMMIT;

-- 확인: sdx/sdy 가 1100 부근이면 균등분포다. 200대면 아직 뭉쳐 있다.
SELECT COUNT(*) n,
       ROUND(STDDEV(pos_x)) sdx,
       ROUND(STDDEV(pos_y)) sdy,
       SUM(pos_x = 0 AND pos_y = 0) at_origin,
       SUM(hp < max_hp) hurt,
       (SELECT COUNT(*) FROM bags) bags
FROM characters
WHERE name LIKE 'bot\_%';
