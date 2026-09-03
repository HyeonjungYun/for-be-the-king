# ADR-0005: 사망 트랜잭션 · 가방 영속화 · 베이스캠프 세션 상태

## Status

**Proposed (2026-09-04)**

## Date

2026-09-04

## Last Verified

2026-09-04

## Decision Makers

윤현중 (사용자) · Claude Code
근거 검토: `/design-review` 2026-09-03 (network-programmer · creative-director) → S4 제기
`/design-review` 재검토 2026-09-04 → S5 신설 (초안에 베이스캠프 정의가 없었다)

## Summary

사망 시 아이템을 어떻게 영속화하고, 사망 가방을 어떤 자료구조로 남기며, 베이스캠프를
서버에서 무엇으로 표현할지 정한다. **사망은 DB 우선**(ADR-0003의 던전 내 메모리 우선과
반대), **가방은 별도 `bags` 테이블 + 부팅 시 만료 복구**, **베이스캠프는 Room이 아니라
`GameSession`의 장소 상태**로 둔다.

## Engine Compatibility

| Field | Value |
|-------|-------|
| **Engine** | Unreal Engine 5.8 (클라) / 자체 C++ IOCP 서버 (이 ADR의 대상) |
| **Domain** | Core (영속성 · 세션 상태) |
| **Knowledge Risk** | LOW — UE API 무관 |
| **References Consulted** | `Server/GameServer/Room.cpp`(`_pendingDeaths`) · `GameServer.cpp`(기동 순서) · `GameSession.h` |
| **Post-Cutoff APIs Used** | None |
| **Verification Required** | None |

## ADR Dependencies

| Field | Value |
|-------|-------|
| **Depends On** | **ADR-0003** (커밋 지점 정의) · **ADR-0004** (`bag_id` 발급) — 둘 다 Accepted 필요 |
| **Enables** | None |
| **Blocks** | P2 아이템 & 장비 구현 중 사망·루팅·출정 선택 경로 |
| **Ordering Note** | ADR-0003·0004 이후에 구현한다. 스키마가 그 둘의 결정 위에 선다 |

## Context

### Problem Statement

ADR-0003이 **던전 내 아이템 조작을 DB에 쓰지 않기로** 했다. 그런데 **사망만은 예외여야
한다** — 사망 가방은 내 세션 밖에서 산다. 다른 플레이어가 10분간 루팅하고, 그 사이 서버가
재시작될 수 있다. 레이드 경계에 묶을 수 없는 유일한 대상이다.

그리고 `/design-review` 재검토(2026-09-04)가 **베이스캠프가 서버에 정의되어 있지 않다**는
것을 찾았다. `item-equipment-system.md`의 Edge Cases와 AC-IE-50(출정 선택)이 **정의되지
않은 전제 위에 서 있었다.**

**지금 정하지 않으면**: 가방 스키마를 짤 수 없고, 창고·출정 선택 경로가 어디서 도는지
모른다.

### Current State

```cpp
// Room.cpp:567, 1247 — 사망은 브로드캐스트만 한다
Protocol::DiedInfo& died = _pendingDeaths.emplace_back();
// 가방 생성 · 아이템 이전 · 영속화 전부 없음 (아이템 시스템 자체가 미구현)

// GameServer.cpp — 기동 순서
GDBPool.Connect(...)      // 63행
GDBQueue.Init(...)        // 69행
service->Start()          // 83행  ← 이 사이에 복구가 들어갈 자리가 있다

// GRooms — 층 4개뿐. 베이스캠프 Room 도 세션 플래그도 없다
constexpr int32 FLOOR_COUNT = 4;
extern RoomRef GRooms[FLOOR_COUNT];

// GameSession — 장소를 나타내는 필드가 없다
atomic<uint64> accountId;
vector<Protocol::CharacterInfo> characters;
atomic<shared_ptr<Player>> player;
```

### Constraints

- 가방은 **10분간 유효**하며 그 사이 서버 재시작을 견뎌야 한다
- 사망 판정은 **Room JobQueue 안**에서 일어난다. 여기서 DB를 기다리면 층 전체가 멈춘다
- `Vaulted` ↔ `Carried` 이동(출정 선택)은 **실시간 전투가 없다** — 메모리 우선이 불필요
- ADR-0003이 **던전 내 DB 접근 0**을 요구한다. 사망은 "던전 내"이지만 **런의 종료**이므로
  예외다

### Requirements

- 사망 시 **레벨 초기화 + 가방 생성이 원자적**이어야 한다 (한쪽만 반영되면 아이템 증발/복제)
- 가방은 **재시작 후에도 같은 정체성**으로 복구되어야 한다
- 다운타임 중 만료된 가방은 **부팅 시 정리**되어야 한다
- 베이스캠프에서 창고↔인벤 이동이 가능해야 한다

## Decision

### ① 사망은 DB 우선이다 — 던전 내 조작과 순서가 반대다

```
던전 내 조작 (ADR-0003)      메모리 → 응답 → (DB 없음)
사망                         DB 커밋 → 콜백 → 메모리 반영 · 브로드캐스트
```

**왜 반대인가**: 던전 내 조작은 실패해도 되돌릴 대상이 있다(런 무효). **사망은 되돌릴 수
없다** — 가방이 남에게 보이는 순간 그건 이미 세상에 나간 사실이다. 메모리에 먼저 만들고
DB가 실패하면 **다른 플레이어가 이미 루팅한 가방이 재시작 후 사라진다.**

```sql
START TRANSACTION;
  UPDATE item_instances SET level = 0 WHERE owner_character_id = ?;   -- C4 레벨 초기화
  INSERT INTO bags (bag_id, floor_id, x, y, expires_at) VALUES (...);
  UPDATE item_instances SET owner_character_id = NULL, bag_id = ? WHERE ...;
COMMIT;
```

커밋이 끝난 뒤에야 `room->DoAsync(&Room::OnBagCreated, ...)`로 되튕겨 `S_SPAWN`을
방송한다 (ADR-0003이 만든 배선).

### ② 가방은 별도 테이블이다

```sql
CREATE TABLE bags (
    bag_id      BIGINT UNSIGNED NOT NULL PRIMARY KEY,   -- = object_id (ADR-0004, 20억~)
    floor_id    INT UNSIGNED    NOT NULL,
    x           FLOAT           NOT NULL,
    y           FLOAT           NOT NULL,
    z           FLOAT           NOT NULL,
    created_at  DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,
    expires_at  DATETIME        NOT NULL,
    INDEX idx_expires (expires_at)
) ENGINE = InnoDB;
```

**아이템 행에 필드를 병기하지 않는 이유**: 가방은 **위치와 만료시각을 가진 실체**다.
아이템마다 병기하면 같은 좌표가 최대 30번 중복되고, "빈 가방"(내용물이 다 루팅된 상태,
§Edge Cases에서 즉시 소멸)을 표현할 방법이 없어진다.

### ③ 부팅 복구는 접속 수락 전에 끝낸다

```
GDBPool.Connect()
GDBQueue.Init()
  ├─ ADR-0004: ID 카운터 복원 (MAX 조회 2회)
  └─ 이 ADR: 만료 가방 정리 + 생존 가방 Room 적재
service->Start()          ← 여기부터 접속을 받는다
```

- `DELETE FROM bags WHERE expires_at <= NOW()` — 다운타임 중 만료된 것
- 남은 가방은 `floor_id`로 해당 `GRooms[floorId]`에 `Object`로 적재
- **복구 실패 시 서버를 기동하지 않는다.** 만료된 가방을 잠깐이라도 서빙하거나
  살아 있는 가방을 잃는 것보다 낫다

### ④ 베이스캠프는 Room이 아니라 세션의 장소 상태다

`item-equipment-system.md` S5가 정한 정책을 그대로 구현한다.

```cpp
enum class SessionPlace : uint8
{
    None = 0,      // 로그인 직후, 캐릭터 미선택
    BaseCamp,      // 베이스캠프 — Room 없음
    Dungeon,       // 던전 — GRooms[floorId] 에 소속
};

class GameSession
{
    atomic<SessionPlace> place = SessionPlace::None;
};
```

**Room을 만들지 않는 이유**: 베이스캠프에는 실시간 전투도 이동 동기화도 브로드캐스트도
없다. Room은 그것들을 위한 직렬 큐인데, 여기선 쓸 일이 없다. 5번째 Room을 만들면
**빈 JobQueue가 100ms마다 도는 비용**만 생긴다.

```
베이스캠프 요청 (C_STASH — 창고↔인벤)
   → Room 우회, GDBQueue 직행
   → 멱등은 reward_grants PK 패턴 (ADR-0003 S2)
   → 그래서 이 경로엔 ADR-0003의 "메모리 우선" 문제가 없다
```

### Architecture

```
        [던전]                                    [베이스캠프]
   GRooms[0..3]                              Room 없음 · GameSession.place
        │                                            │
   사망 발생                                    C_STASH (창고↔인벤)
        │                                            │
        ▼                                            ▼
   ┌─────────────────────┐                  ┌──────────────────┐
   │ GDBQueue            │                  │ GDBQueue         │
   │ 트랜잭션            │                  │ DB 직행          │
   │  레벨 초기화        │                  │ 멱등 = PK 패턴   │
   │  가방 INSERT        │                  └──────────────────┘
   │  아이템 소유권 이전 │
   └──────────┬──────────┘
              │ room->DoAsync(OnBagCreated)
              ▼
   ┌─────────────────────┐
   │ Room                │
   │  가방을 _objects 에 │
   │  S_SPAWN 방송       │
   └─────────────────────┘

   [부팅]  DELETE 만료 가방 → 생존 가방을 각 Room 에 적재 → 그다음 접속 수락
```

### Key Interfaces

```cpp
// Room — DB 콜백 수신 (ADR-0003 배선)
void Room::OnBagCreated(uint64 bagId, float x, float y, bool ok);

// 부팅 복구 — service->Start() 전에 호출
bool RecoverBagsFromDB(DBConnection* conn);   // 만료 정리 + 생존 적재

// 가방 만료 — Room 타이머에 편입
void Room::UpdateBagExpiry(uint64 nowUs);     // UpdateTick 에서 호출
```

### Implementation Guidelines

**사망 경로에서 Room을 막지 않는다.** 사망 판정은 Room에서 하되, DB 작업은
`GDBQueue.Push`로 넘기고 Room은 즉시 다음 Job으로 넘어간다. 가방이 화면에 뜨는 것은
콜백 이후다 — **사망 연출과 가방 등장 사이에 짧은 간격이 생기며, 이는 허용된다.**

**빈 가방 즉시 소멸**(§Edge Cases)은 메모리에서 판정하고 DB 삭제는 비동기로 보낸다.
가방이 비었다는 것은 되돌릴 필요가 없는 사실이다.

**만료 검사는 Room 타이머에서 한다.** 이미 `UpdateTick`이 100ms마다 돈다. 별도 스케줄러를
만들지 않는다.

**`SessionPlace` 전이는 세 지점뿐이다** — 로그인 완료(→BaseCamp) · 던전 입장(→Dungeon) ·
탈출/사망 완료(→BaseCamp). 그 외에는 바뀌지 않는다.

## Alternatives Considered

### Alternative 1: 가방을 아이템 행의 필드로 (별도 테이블 없음)

- **Description**: `item_instances`에 `bag_x`·`bag_y`·`bag_expires_at`을 두고, 같은
  가방의 아이템들은 같은 값을 갖는다.
- **Pros**: 테이블이 하나 적다. 조인이 없다
- **Cons**: 좌표·만료시각이 **아이템마다 최대 30번 중복**된다. 만료 시각을 바꾸려면 30행을
  UPDATE해야 한다. **빈 가방을 표현할 수 없다** — 내용물이 0이면 행이 없어 가방의 존재
  자체가 사라진다(즉시 소멸이 우연히 맞지만, "빈 가방이 잠깐 존재하는" 어떤 규칙도 못 쓴다)
- **Estimated Effort**: 약간 적음
- **Rejection Reason**: 가방은 **위치와 수명을 가진 독립된 실체**다. 아이템의 속성이 아니다.

### Alternative 2: 사망도 메모리 우선

- **Description**: 던전 내 조작과 같이 메모리에 가방을 만들고 응답한 뒤 DB에 비동기로 쓴다.
- **Pros**: 사망 처리가 빠르다. 경로가 하나로 통일된다
- **Cons**: **DB 쓰기가 실패하면 이미 남이 루팅한 가방이 재시작 후 사라진다.**
  루팅한 사람의 인벤에는 아이템이 있는데 DB엔 그 아이템이 원 주인 소유로 남아 있다 —
  **복제**다
- **Estimated Effort**: 같음
- **Rejection Reason**: 익스트랙션 경제에서 복제는 시스템을 무너뜨린다. **사망은 되돌릴
  수 없는 사건**이라 커밋 후에 세상에 알려야 한다.

### Alternative 3: 베이스캠프를 5번째 Room으로

- **Description**: `GRooms[4]`를 베이스캠프로 두고 기존 Room 기계장치를 그대로 쓴다.
- **Pros**: 경로가 통일된다. 세션 상태 필드가 불필요하다
- **Cons**: 베이스캠프엔 이동·전투·브로드캐스트가 없는데 **JobQueue가 100ms마다 돈다.**
  `FLOOR_COUNT` 상수의 의미가 오염된다("층 수"인가 "Room 수"인가). `GetRoomForFloor`가
  베이스캠프를 층으로 반환하게 된다
- **Estimated Effort**: 적음
- **Rejection Reason**: `item-equipment-system.md` S5가 이미 **"Room이 없는 세션 상태"**로
  정책을 정했다. Room은 실시간 동기화를 위한 장치이며, 그것이 필요 없는 곳에 쓰면
  개념이 흐려진다.

### Alternative 4: 지연 복구 (부팅 시 정리하지 않음)

- **Description**: 가방을 부팅 시 적재하지 않고, 플레이어가 그 층에 들어올 때 조회한다.
- **Pros**: 부팅이 빠르다
- **Cons**: **만료된 가방을 잠깐 서빙할 수 있다.** 층 입장마다 가방 조회가 필요해
  입장 경로에 DB가 들어간다(현재 `[ENTER] avg 57~87us`가 무너진다)
- **Estimated Effort**: 비슷함
- **Rejection Reason**: 입장 경로에 DB를 넣지 않는다는 원칙(ADR-0003의 측정 결과)과 충돌한다.
  부팅 비용은 1회지만 입장 비용은 매번이다.

## Consequences

### Positive

- **사망이 원자적이다** — 레벨 초기화와 가방 생성이 한 트랜잭션이라 중간 상태가 없다
- 가방이 **재시작을 넘어 살아남는다** — 10분 계약이 서버 수명과 무관해진다
- 빈 JobQueue가 돌지 않는다 (베이스캠프에 Room 없음)
- 베이스캠프 경로가 **기존 지급 트랜잭션 패턴을 그대로 재사용**한다

### Negative

- **사망 연출과 가방 등장 사이에 간격이 생긴다** (DB 왕복 수 ms)
- 부팅이 느려진다 (가방 조회 + 적재)
- 커밋 모델이 **경로마다 다르다** — 던전 내 메모리 우선, 사망 DB 우선, 베이스캠프 DB 직행.
  세 가지를 기억해야 한다
- `SessionPlace` 전이를 빠뜨리면 **엉뚱한 경로로 요청이 간다**

### Neutral

- `bags` 테이블이 하나 늘어난다
- `Room::UpdateTick`에 만료 검사가 얹힌다 (현재 주기 저장만 있다)

## Risks

| Risk | Probability | Impact | Mitigation |
|------|------------|--------|-----------|
| 사망 커밋 실패 | 낮음 | **높음 — 아이템 증발** | 재시도. 실패가 지속되면 사망 처리를 보류하고 로그 |
| 부팅 복구 실패 | 낮음 | **높음 — 가방 유실 또는 유령 가방** | **복구 실패 시 기동하지 않는다** (ADR-0004와 같은 원칙) |
| `SessionPlace` 전이 누락 | 중간 | 중간 | 전이 지점이 셋뿐이다. 각각에 AC를 건다 |
| 세 가지 커밋 모델 혼동 | **중간** | 중간 (잘못된 경로로 구현) | 이 ADR과 ADR-0003에 표로 명시. 코드 주석에도 |
| 만료 검사가 Room 틱 비용 증가 | 낮음 | 낮음 | 가방 수는 층당 수십 개 규모 |

## Performance Implications

| Metric | Before | Expected After | Budget |
|--------|--------|---------------|--------|
| 사망당 DB 트랜잭션 | — (미구현) | **1회** | — |
| 사망 → 가방 등장 지연 | — | DB 왕복 (수 ms) | 허용 |
| 부팅 시 추가 DB 조회 | 0회 | **2회** (만료 DELETE + 생존 SELECT) | 부팅 1회 |
| 층 입장 지연 | 57~87 μs | **변화 없음** (가방은 부팅 시 적재) | § 0-4 유지 |
| Room 틱 비용 | 주기 저장만 | + 만료 검사 (층당 수십 개 순회) | 16.6 ms (SV-1) |

## Migration Plan

1. `bags` 테이블 생성 (`Server/Tools/schema_item.sql`)
2. `SessionPlace` enum + `GameSession::place` 추가 → 기존 동작 무변화(기본 `None`)
3. 로그인·입장·퇴장 경로에 전이 삽입
4. 부팅 복구 함수를 `GDBQueue.Init()`와 `service->Start()` **사이**에 삽입
5. 사망 경로에 트랜잭션 + 콜백 배선 (ADR-0003의 배선이 선행)
6. `Room::UpdateBagExpiry`를 `UpdateTick`에 편입

**Rollback plan**: 가방·아이템이 신규 기능이라 되돌려도 기존 시스템에 영향이 없다.
`bags` 테이블을 두고 코드만 비활성화하면 데이터가 남는다.

## Validation Criteria

- [ ] 사망 시 **레벨 초기화와 가방 생성이 같은 트랜잭션**에서 일어난다 (한쪽만 반영 불가)
- [ ] 사망 커밋을 인위적으로 실패시키면 **가방이 화면에 나타나지 않는다**
- [ ] 가방 생성 직후 서버를 재시작하면 **같은 `bag_id`로 복구**된다
- [ ] 만료 시각이 지난 가방을 만들고 재시작하면 **복구되지 않고 DB에서도 삭제**된다
- [ ] 복구 쿼리를 실패시키면 **서버가 기동하지 않는다**
- [ ] 베이스캠프에서 `C_STASH`를 보내면 **Room JobQueue를 거치지 않는다**
- [ ] 층 입장 지연(`[ENTER] avg`)이 가방 도입 전후로 **변화 없다**
- [ ] 가방의 마지막 아이템을 루팅하면 **즉시 소멸**하고 `S_DESPAWN`이 나간다 (AC-IE-14)

## GDD Requirements Addressed

| GDD Document | System | Requirement | How This ADR Satisfies It |
|-------------|--------|-------------|--------------------------|
| `design/gdd/item-equipment-system.md` | 아이템 & 장비 | S4 — "사망은 DB 먼저, 가방은 엔티티로 복구. 트랜잭션 = 레벨 초기화 + 가방 생성 + `expires_at`" | 단일 트랜잭션 + 별도 `bags` 테이블 |
| `design/gdd/item-equipment-system.md` | 아이템 & 장비 | S4 — "다운타임 중 만료된 가방은 부팅 시 `Destroyed`" | 부팅 시 `DELETE ... WHERE expires_at <= NOW()` |
| `design/gdd/item-equipment-system.md` | 아이템 & 장비 | S5 — "베이스캠프는 Room이 없는 세션 상태" | `GameSession::place` enum |
| `design/gdd/item-equipment-system.md` | 아이템 & 장비 | AC-IE-11 — 10분 경과 시 가방과 잔여 내용물 소멸 | `Room::UpdateBagExpiry` |
| `design/gdd/item-equipment-system.md` | 아이템 & 장비 | AC-IE-14 — 내용물이 전부 루팅되면 가방 즉시 소멸 | 메모리에서 판정 후 비동기 DELETE |
| `design/gdd/item-equipment-system.md` | 아이템 & 장비 | AC-IE-50 — 출정 선택(`Vaulted` ↔ `Carried`) | 베이스캠프 DB 직행 경로 |

## Related

- **ADR-0003** (레이드 경계 커밋) — **선행 필수.** 사망은 그 ADR이 정한 커밋 지점 중 하나이며,
  DB→Room 콜백 배선도 그쪽이 만든다
- **ADR-0004** (ID 발급) — **선행 필수.** `bag_id` = `object_id` 20억 대역이 거기서 정해진다
- `design/gdd/item-equipment-system.md` § Dependencies > 서버 측 선행 작업 S4·S5
- 구현 대상: `Server/GameServer/Room.cpp` · `GameSession.h` · `GameServer.cpp` ·
  `Server/Tools/schema_item.sql` (🔴 전부 사용자 소유 영역)
