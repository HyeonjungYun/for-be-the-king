# 서버 검증 툴 (`verify-server.ps1`)

서버를 열기 전에 문제가 있는지 자동으로 확인한다 (2026-10-21). **운영 DB(`forbetheking`) 와 운영 포트(7777)는 건드리지 않는다** —
DB 는 임시 스키마 `fbtk_verify_*` 를 만들었다 지우고, 스모크 서버는 7801 / 7802 에서 따로 돈다.

```powershell
.\Tools\verify\verify-server.ps1                     # 네 단계 전부 (약 3 ~ 4 분) - DB 비밀번호는 FBTK_DB_PASSWORD
.\Tools\verify\verify-server.ps1 -Stage static       # 정적 검사만 (몇 초)
.\Tools\verify\verify-server.ps1 -Stage smoke -Bots 60 -Seconds 300
.\Tools\verify\verify-server.ps1 -NoCoverage -KeepDb # 커버리지 생략 · 임시 스키마를 남겨 조사
```

판정 — **FAIL 이 하나라도 있으면 "열지 말 것"** (종료 코드 1). WARN 은 열어도 되지만 볼 것.
보고서는 `Tools/verify/reports/<시각>/report.md` (+ `results.json` · 각 단계 로그 · 스모크 서버 로그 · 통계 CSV). git 에는 안 들어간다.

| 단계 | 검사 | 실패 기준 |
|---|---|---|
| **static** | 워크플로 사본 (`Tools/ci/sync-workflows.ps1 -Check`) | `.github/workflows` 가 `Tools/ci/workflows` 와 다르다 |
| | 줄끝 · 인코딩 (`check_text.py`) | 한 파일에 CRLF · LF 가 섞였다 · UTF-8 이 아니다 · 한글이 든 Tools 의 ps1 에 BOM 이 없다 |
| | 패킷 생성물 (`check_proto.py`) | GameServer · DummyClient · S1 의 `*.pb.*` · 패킷 핸들러가 지금 `.proto` 로 만든 것과 다르다 |
| | 부팅 데이터 (`GameServer --validate`) | 부팅 검사(데이터 CSV 36 개 · 벽 · 순찰 길 …) 실패. DB · 소켓 · 기록 파일을 열지 않는다 |
| **build** | ServerCore (Release · Debug) · GameServer · DummyClient · GameServerTests · ServerCoreSTests · 인증 서버 | 빌드 오류 (경고 수는 보고만) |
| | GameServerTests · ServerCoreSTests | 실패 1 개 이상 |
| | 커버리지 (OpenCppCoverage) | ServerCore 라인 < 70 % (제외 목록은 technical-preferences.md 와 같다) |
| **db** | 빈 DB 에 `setup_db` | 스크립트 오류 |
| | 지금 DB 의 스키마 사본에 schema · migrate 다시 적용 | 이전 스크립트가 지금 DB 에서 실패 |
| | 새 DB = 이전한 DB | 표 · 열 · 인덱스 차이 (`db/schema-diff.txt`). 운영 DB 에만 있는 표(하네스 백업)는 비교에서 뺀다 |
| | 운영 DB 에 남은 이전 | (WARN) 운영 DB 에 아직 없는 열 · 인덱스 |
| | 코드가 쓰는 표 · 열 (`check_sql_refs.py`) | 서버 · 인증 서버 코드가 쓰는 표 · INSERT 열이 새 DB 에 없다 |
| **smoke** | 임시 DB(`fbtk_verify_smoke`, 봇 시드) · 포트 · 서버 부팅 | 포트를 누가 쓰고 있다 · 90 초 안에 listen 이 없다 |
| | 봇 N 명 × M 초 (`DummyClient -ai -port`) | 서버 · 봇이 도중에 죽었다 |
| | Ctrl+C 정상 종료 (`send-ctrlc.ps1`) | 20 초 안에 안 끝난다 · `[SHUTDOWN] done` 이 없다 |
| | 크래시 덤프 · 오류 줄 (DB · 커밋 · 내부 · ASSERT · 표준 오류) | 1 개 이상 (`smoke/error-lines.txt`) |
| | 입장 · 활동 | 입장 < 90 % · 스킬 0 |
| | 안티치트 · 이동 거부 | 봇 kick · ban 1 이상 · 이동 거부 > 봇·분당 3 |
| | Room 플러시 · 메모리 | P95 > 16.6 · P99 > 33 · max > 100 ms · 커밋 > 512 MB |

기준값은 `thresholds.json` 에서 바꾼다. CI(`Tools/ci/workflows/server-tests.yml`)는 static 의 앞 세 검사를 빌드 전에 돌린다.

**첫 실행에서 찾은 것 (2026-10-21)** — 둘 다 고쳤다
- 종료 때 `[ACCEPT] accept slot failed 8 times` 100 줄 — 리슨 소켓을 닫으면 걸려 있던 AcceptEx 가 전부 실패로 돌아오는데 다시 걸려고 세션을 800 개 만들었다 (`ServerCore/Listener.cpp` `_closed`)
- `[ASSERT] DealDamage outside a combat flush` — 재입력 스킬이 전투 Flush 밖에서 KillBatch 를 닫은 채 피해를 줬다 (`Room::HandleSkill`)
