USE forbetheking;

ALTER TABLE accounts
    ADD COLUMN password_iterations INT UNSIGNED NOT NULL DEFAULT 600000
    AFTER password_salt;

SELECT username, password_iterations FROM accounts LIMIT 5;
