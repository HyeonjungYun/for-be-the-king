USE forbetheking;

SET @account_name = 'auth_test';

SET @account_id = (SELECT account_id FROM accounts WHERE username = @account_name);

SELECT IF(@account_id IS NULL,
    CONCAT(@account_name, ' 계정이 없다 — POST /auth/register 를 먼저 하라'),
    CONCAT('ok - account_id=', @account_id)) AS precheck;

START TRANSACTION;

-- 재실행 가능하게. item_instances 는 characters 에 FK 가 없으므로 직접 지운다
DELETE FROM item_instances
WHERE owner_character_id IN (SELECT character_id FROM characters WHERE account_id = @account_id);

DELETE FROM characters WHERE account_id = @account_id;

-- 계정이 없으면 0행이 들어간다. 아래 SELECT 가 비면 그것이 신호다
INSERT INTO characters (account_id, name, hp, max_hp, floor_id, pos_x, pos_y, pos_z, yaw)
SELECT @account_id, @account_name, 500, 500, 0, 200.0, 200.0, 100.0, 0.0
WHERE @account_id IS NOT NULL;

COMMIT;

SELECT a.username, c.character_id, c.name, c.floor_id, c.pos_x, c.pos_y
FROM accounts a JOIN characters c ON c.account_id = a.account_id
WHERE a.username = @account_name;
