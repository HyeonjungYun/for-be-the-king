-- 부하 측정용 봇 리셋 - 무기 7종 섞어서 (skill-roster 1 단계 회귀, 2026-10-10).
--
-- reset_bots.sql 과 같되 무기를 character_id % 7 로 돌려 가며 준다. 봇(DummyClient)은 주무기 슬롯 하나와 신발만 누르므로
-- 주무기 스킬로 1 단계 효과가 전부 나가게 골랐다. 보조 슬롯은 패시브(평타 적중 · 치명상) - 키를 안 눌러도 발동한다.
--
--    m  무기   주무기 (누름)                  보조 (안 누름 - 패시브만 의미)   레벨
--    0  101 검    1013 흡인      끌기 + 피해            1015 출혈                      0
--    1  102 망치  1006 강타      부채꼴 피해 + 넉백     1034 맹수의 본능 (치명상 방어)  1
--    2  103 창    1017 점지      피해 + 창 표식         1024 뇌신 (평타 적중 번개)      0
--    3  104 단검  1036 독침      피해 + 중독(중첩)      1043 사냥 종료                  0
--    4  105 활    1052 강궁      피해 + 넉백 (사거리 9 m) 1047 속사 (버프)              0
--    5  106 너클  1058 밀어내기  직선 피해 + 가까운 적 넉백 1067 화신 (평타 적중 화염)   0
--    6  107 스태프 1077 마나 갑주 버프 3종 (강화 레벨 2 → +21 %) 1068                  2
--
-- 사용법 (PowerShell) - 서버를 끈 상태에서만 (아이템 행은 게임 서버 밖에서 바꾸지 않는다는 규칙의 측정 전용 예외)
--    Get-Content .\Server\Tools\reset_bots_mixed.sql |
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

DELETE FROM item_instances WHERE bag_id IS NOT NULL;
DELETE FROM bags;

DELETE i FROM item_instances i
JOIN characters c ON c.character_id = i.owner_character_id
WHERE c.name LIKE 'bot\_%';

-- 무기 - character_id % 7. 두 스킬 모두 그 무기 풀 안에 있어야 부팅 검사(item rows checked)가 통과한다 (skill_pools.csv)
INSERT INTO item_instances
    (instance_id, item_type_id, grade, level, state, slot,
     owner_character_id, skill_id_primary, skill_id_secondary)
SELECT 800000 + c.character_id * 10 + 1, w.type_id, 1, w.lvl, 2, 1,
       c.character_id, w.skill_id, w.skill_id2
FROM characters c
JOIN (
    SELECT 0 AS m, 101 AS type_id, 1013 AS skill_id, 1015 AS skill_id2, 0 AS lvl UNION ALL
    SELECT 1,      102,            1006,            1034,              1        UNION ALL
    SELECT 2,      103,            1017,            1024,              0        UNION ALL
    SELECT 3,      104,            1036,            1043,              0        UNION ALL
    SELECT 4,      105,            1052,            1047,              0        UNION ALL
    SELECT 5,      106,            1058,            1067,              0        UNION ALL
    SELECT 6,      107,            1077,            1068,              2
) w ON w.m = (c.character_id % 7)
WHERE c.name LIKE 'bot\_%';

-- 갑옷 · 신발은 기본 장비 그대로 (starter_kit.csv)
INSERT INTO item_instances
    (instance_id, item_type_id, grade, level, state, slot,
     owner_character_id, skill_id_primary, skill_id_secondary)
SELECT 800000 + c.character_id * 10 + 2, 401, 1, 0, 2, 4, c.character_id, 0, 0
FROM characters c
WHERE c.name LIKE 'bot\_%';

INSERT INTO item_instances
    (instance_id, item_type_id, grade, level, state, slot,
     owner_character_id, skill_id_primary, skill_id_secondary)
SELECT 800000 + c.character_id * 10 + 3, 501, 1, 0, 2, 5, c.character_id, 1003, 0
FROM characters c
WHERE c.name LIKE 'bot\_%';

COMMIT;

SELECT i.item_type_id, COUNT(*) AS bots, i.skill_id_primary, i.skill_id_secondary, i.level
FROM item_instances i
JOIN characters c ON c.character_id = i.owner_character_id
WHERE c.name LIKE 'bot\_%' AND i.slot = 1
GROUP BY i.item_type_id, i.skill_id_primary, i.skill_id_secondary, i.level
ORDER BY i.item_type_id;
