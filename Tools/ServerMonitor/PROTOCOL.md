# 관리 포트 프로토콜 (GameServer ↔ ServerMonitor, 2026-10-21)

- **TCP `127.0.0.1:<FBTK_ADMIN_PORT>`** (기본 7790, `0` = 끔). 이 PC 에서만 열린다 — 바인드 주소가 루프백이다
- 한 줄에 JSON 하나 (UTF-8, `\n` 끝). 양쪽 다 같다
- **인증**: 서버가 부팅 때 `Logs/admin.json` (`FBTK_ADMIN_FILE` 로 바꿀 수 있다)에 `{"port", "token", "pid", "boot", "dataDir", "eventsFile"}` 를 쓴다.
  토큰은 부팅마다 새 32 바이트 난수(hex 64). 첫 줄이 `hello` + 맞는 토큰이 아니면 서버가 끊는다 (이 PC 의 다른 프로그램도 막는다)
- 접속자는 동시에 4 개까지

## 클라이언트 → 서버 (`op`)

| op | 필드 | 응답 |
|---|---|---|
| `hello` | `token` · `client` | `hello` |
| `events` | `backlog` (0 ~ 20000) | 최근 이벤트 `backlog` 개를 `ev` 로 보낸 뒤 실시간 `ev` 를 계속 보낸다 |
| `players` | | `players` |
| `player` | `id` (캐릭터 id) | `player` |
| `rooms` | | `rooms` |
| `map` | `room` (`""` = 그만) | 그 방의 `map` 을 초당 5 번 |
| `metrics` | | `metrics` (그 뒤 10 초 통계마다 자동으로) |
| `kick` | `id` · `reason` | `result` |
| `unban` | `account` 또는 `name` | `result` |
| `notice` | `text` | `result` |
| `ping` | | `pong` |

## 서버 → 클라이언트 (`t`)

```jsonc
{"t":"hello","ok":true,"pid":1234,"port":7777,"boot":"2026-10-21 20:00:00","dataDir":"C:\\...\\Server\\Data","tags":["connect", ...]}
{"t":"ev","seq":42,"ts":"2026-10-21 20:01:40.313","lv":"I","th":8,"tag":"combat","sub":"skill_fire","a":[128],"tg":[171],"acc":0,"room":"w0","src":"log","msg":"[SKILL FIRE] caster=128 ..."}
{"t":"ev", ..., "src":"ev","sub":"damage","a":[128],"tg":[171],"d":{"dmg":37,"hp":120,"mhp":559,"crit":0,"skill":1007},"msg":"128 → 171 피해 37 (120/559)"}
{"t":"players","list":[{"id":128,"name":"bot_1","acc":12,"room":"w0","state":"room","hp":540,"mhp":559,"x":0,"y":0,"bot":true,"party":0}]}
{"t":"player","p":{"id":128,"name":"bot_1","acc":12,"room":"w0","x":..,"y":..,"yaw":..,"hp":..,"mhp":..,"alive":true,"gold":0,"party":0,"vision":1500,"cheat":3.2,
                   "gear":[{"slot":1,"type":101,"grade":1,"level":0,"skill":1007,"skill2":1015}], "skills":[{"slot":1,"skill":1007,"bind":1}]}}
{"t":"rooms","list":[{"tag":"w0","kind":"pvp","floor":0,"players":30,"monsters":48,"bags":30}]}
{"t":"map","room":"w0","objs":[{"id":128,"k":"p","x":..,"y":..,"yaw":..,"hp":..,"mhp":..,"n":"bot_1","bot":1,"party":0,"vr":1500},
                               {"id":4000000009,"k":"m","x":..,"y":..,"hp":..,"mhp":..,"n":"monster.9001","st":"chase","pack":602},
                               {"id":3000000212,"k":"bag","x":..,"y":..,"open":0,"chest":1},
                               {"id":77,"k":"zone","x":..,"y":..,"r":250},{"id":88,"k":"proj","x":..,"y":..},
                               {"id":0,"k":"tower","x":..,"y":..,"n":"Ta","gauge":350}]}
{"t":"metrics","m":{"sessions":31,"flush_p95_us":500,"flush_p99_us":1000,"flush_max_us":6900,"recv_mbps":0.3,"sent_mbps":2.1,"commit_mb":93,"db_pending":0,"rooms":4,"instances":0}}
{"t":"result","op":"kick","ok":true,"msg":"..."}
{"t":"error","msg":"..."}
```

- **태그 13 종** (`Server/Data/event_tags.csv` — 로그 줄머리 + 핵심어 → 태그 · 하위 종류): `connect` 접속 · `move` 이동 · `combat` 전투 · `status` 상태이상 ·
  `item` 아이템 · `storage` 창고 · `economy` 경제 · `party` 파티 · `escape` 탈출 · `cheat` 안티치트 · `monster` 몬스터 · `db` DB 커밋 · `system` 시스템
- `src` — `log` 는 서버 텍스트 로그 한 줄을 구조화한 것 (`a` · `tg` 는 줄 안의 `char=` · `caster=` · `victim=` 같은 키에서), `ev` 는 텍스트 로그가 없는 사건을 서버가 직접 낸 것 (피해 · CC · 관리 명령)
- 같은 내용이 `Logs/events/events-<부팅 시각>.jsonl` 에 `t` 없이 한 줄씩 남는다 — ServerMonitor 의 "지난 로그 열기" 가 이 파일을 읽는다
