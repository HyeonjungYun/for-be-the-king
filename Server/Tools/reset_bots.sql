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

-- 확인: sdx/sdy 가 1100 부근이면 균등분포다. 200대면 아직 뭉쳐 있다.
SELECT COUNT(*) n,
       ROUND(STDDEV(pos_x)) sdx,
       ROUND(STDDEV(pos_y)) sdy,
       SUM(pos_x = 0 AND pos_y = 0) at_origin,
       SUM(hp < max_hp) hurt
FROM characters
WHERE name LIKE 'bot\_%';
