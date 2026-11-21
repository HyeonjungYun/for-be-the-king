# loadtest — 동접 부하 시험 (2026-11-21)

봇 수천~1만을 붙여 서버의 수용 인원 · 지연을 잰다. 결과와 과정은 [`docs/server-scaling-10k.md`](../../docs/server-scaling-10k.md).

```powershell
# 준비 (한 번) - 시험 스키마 + 봇 계정 1만
.\Tools\db\setup_db.ps1 -Password $env:FBTK_DB_PASSWORD -Database fbtk_scale
# seed_bots_scale.sql 의 USE forbetheking; 를 fbtk_scale 로 바꿔 흘린다 (Tools/db/seed_bots_scale.sql 머리 주석)

# 서버 하나 · 봇 2,000 (기본 30 Hz)
.\Tools\loadtest\run-scale.ps1 -Bots 2000 -Procs 2 -Seconds 120 -Label my-run

# 1만 (서버 방송 10 Hz · 몬스터 이동 2 틱에 한 번 · 봇 이동 4 Hz · 0 층에 흩어짐)
.\Tools\loadtest\run-scale.ps1 -Bots 10000 -Procs 3 -ConnRate 200 -Ticks 2 -Io 2 -Seconds 150 -Label 10k `
    -ServerEnv @{ FBTK_WORKER_THREADS = '6'; FBTK_TICK_MS = '100'; FBTK_MONSTER_MOVE_EVERY = '2' } `
    -BotArgs @('-floor','0','-wander','8000','-movehz','4','-hb','2000')

# 2 단계 - 게임 서버 2 대 + 인증 서버의 방 배정 (봇 프로세스마다 /lobby/assign 에 묻는다)
.\Tools\loadtest\run-scale.ps1 -Bots 10000 -Procs 4 -Servers 2 ... -KeepRunning     # 끝나도 서버 · 봇을 남긴다 (ServerMonitor 녹화)
```

| 파일 | 하는 일 |
|---|---|
| `run-scale.ps1` | 서버(들) · 인증 서버 · 봇 프로세스를 띄우고 10 초마다 CPU · 메모리, 끝나면 Ctrl+C 정상 종료 → `reports/<시각>-<이름>/` |
| `summarize.py` | 서버 통계 CSV(서버마다) · 봇 `[SCALE]` 줄 · 표본을 모아 `summary.json` · `summary.md` (정상 상태 = 목표 인원의 95 % 이상이 게임 안인 구간) |

**봇 (`DummyClient -scale`) 옵션** — `-bots` · `-botbase` · `-connrate` (초당 접속) · `-ticks` · `-io` · `-movehz` · `-hb` (하트비트 ms) ·
`-wander` (배회 반경 cm) · `-floor` · `-scalepeace` (싸우지 않음) · `-srcip` (출발 주소 127.0.0.N) · `-assign <인증 서버 URL>` · `-recvkb` · `-noskip`

**함정** (전부 실제로 겪었다)

- 시험이 과부하로 무너지면 봇 계정이 안티치트에 **1 시간 정지**된다 — `run-scale.ps1` 이 시험 스키마의 정지 · strike · 등록부를 시작 전에 지운다
- 1만 접속을 같은 주소에서 연달아 걸면 직전 시험의 TIME_WAIT(120 s)에 걸릴 수 있다 — 봇 프로세스마다 출발 주소가 다르다
- 서버와 봇이 같은 PC 라 **봇이 밀리면 이동이 몰려 들어가 서버 안티치트가 정상적으로 끊는다** — 봇 `tick_max` · `late` 를 같이 본다
