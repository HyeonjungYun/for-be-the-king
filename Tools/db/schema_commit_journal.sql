-- 런 끝 저장 기록의 적용 표시 (ADR-0010 § ⑥, 2026-10-08)
--
-- 기다리는 사망 · 탈출 · 만료 저장은 Server/Logs/pending-commits/*.journal 로 남고, 그 저장이 커밋될 때 같은 트랜잭션 안에서
-- 여기에 기록 id 를 넣는다. 커밋은 됐는데 파일을 지우기 전에 서버가 꺼졌으면, 다음 부팅이 이 표시를 보고 그 기록을 건너뛴다
-- (탈출 골드 두 번 지급 같은 일을 막는다). 부팅이 모든 기록을 정리한 뒤 표를 비운다 - 표시는 파일이 남은 저장을 위해서만 있다.
--
-- 🔴 journal_id 가 PRIMARY KEY 인 것이 멱등성의 전부다 (schema_reward.sql reward_grants 와 같은 방식).
-- 서버가 부팅 때 같은 정의로 CREATE TABLE IF NOT EXISTS 한다 (ObjectUtils.cpp EnsureCommitJournalTable) - 손으로 돌릴 필요는 없다.

USE forbetheking;

CREATE TABLE IF NOT EXISTS applied_commits
(
    journal_id   VARCHAR(64)     NOT NULL,      -- DEATH-128-1791459044123-7
    character_id BIGINT UNSIGNED NOT NULL,
    kind         VARCHAR(16)     NOT NULL,      -- DEATH · ESCAPE · EXPIRE
    applied_at   DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,

    PRIMARY KEY (journal_id)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;
