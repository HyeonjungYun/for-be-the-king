-- 지급 벤치마크 회차 사이 리셋.
--
-- 🔴 리셋하지 않고 다시 돌리면 이전 request_id 가 그대로 남아 있어
--    전부 GRANT_ALREADY 로 빠진다. 멱등성이 작동한다는 증거이긴 하지만
--    지급 경로(INSERT → UPDATE)를 한 번도 지나지 않으므로 측정이 안 된다.
--
-- 사용법
--    db reset_grants.sql

USE forbetheking;

DELETE FROM reward_grants;
UPDATE characters SET gold = 0;

SELECT COUNT(*) grants,
       (SELECT SUM(gold) FROM characters) total_gold
FROM reward_grants;
