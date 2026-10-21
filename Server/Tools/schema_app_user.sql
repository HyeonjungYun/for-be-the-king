-- 게임 서버 · 인증 서버가 쓰는 전용 DB 계정 (2026-10-21 리뷰 후속).
--   예전엔 두 서버가 root 로 붙었다 - 서버 프로세스 하나가 뚫리면 DB 전체(다른 스키마 · 계정 관리 · FILE)가 같이 넘어간다.
--   fbtk_app 은 forbetheking 스키마의 표를 읽고 · 쓰고 · 지우고, applied_commits 를 만들(CREATE TABLE IF NOT EXISTS) 수만 있다.
--   DROP · ALTER · GRANT · FILE · SUPER 없음. 스키마 변경(setup_db.ps1 · migrate_*.sql)은 그대로 root 로 한다.
--
-- 비밀번호는 setup_db.ps1 이 -Password 값으로 바꿔 넣는다 (__APP_PASSWORD__). 두 서버는 FBTK_DB_USER=fbtk_app (기본) + FBTK_DB_PASSWORD 로 붙는다.
-- 다시 돌려도 된다.
CREATE USER IF NOT EXISTS 'fbtk_app'@'localhost' IDENTIFIED BY '__APP_PASSWORD__';
CREATE USER IF NOT EXISTS 'fbtk_app'@'127.0.0.1' IDENTIFIED BY '__APP_PASSWORD__';
ALTER USER 'fbtk_app'@'localhost' IDENTIFIED BY '__APP_PASSWORD__';
ALTER USER 'fbtk_app'@'127.0.0.1' IDENTIFIED BY '__APP_PASSWORD__';

GRANT SELECT, INSERT, UPDATE, DELETE, CREATE ON forbetheking.* TO 'fbtk_app'@'localhost';
GRANT SELECT, INSERT, UPDATE, DELETE, CREATE ON forbetheking.* TO 'fbtk_app'@'127.0.0.1';
FLUSH PRIVILEGES;

SELECT user, host FROM mysql.user WHERE user = 'fbtk_app';
