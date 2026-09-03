# ADR-0004: 아이템 인스턴스 ID · 가방 ID 발급 — 대역 분리와 부팅 시 카운터 복원

## Status

**Proposed (2026-09-04)**

## Date

2026-09-04

## Last Verified

2026-09-04

## Decision Makers

윤현중 (사용자) · Claude Code
근거 검토: `/design-review` 2026-09-03 (network-programmer) → S3 제기

## Summary

아이템 인스턴스와 사망 가방에 어떤 식별자를 줄지 정한다. **아이템 `instance_id`는
`object_id`와 완전히 별개인 공간**으로 두고(아이템은 `Object`가 아니다), **가방은
`Object`이므로 `object_id` 공간의 신규 대역(20억~)을 쓰되 DB PK를 그대로 통신 ID로
사용**한다. 두 카운터 모두 서버 전역 atomic이며 **부팅 시 DB에서 최댓값을 읽어 복원**한다.

## Engine Compatibility

| Field | Value |
|-------|-------|
| **Engine** | Unreal Engine 5.8 (클라) / 자체 C++ IOCP 서버 (이 ADR의 대상) |
| **Domain** | Core (식별자 · 영속성) |
| **Knowledge Risk** | LOW — UE API 무관 |
| **References Consulted** | `Server/GameServer/ObjectUtils.cpp` · `Common/.../Enum.proto` · `Struct.proto` |
| **Post-Cutoff APIs Used** | None |
| **Verification Required** | None |

## ADR Dependencies

| Field | Value |
|-------|-------|
| **Depends On** | None |
| **Enables** | ADR-0005 (가방 영속화 — 이 ADR이 정한 `bag_id` 위에 스키마를 짠다) |
| **Blocks** | P2 아이템 & 장비 구현 (`.proto` · DB 스키마) |
| **Ordering Note** | ADR-0003과 독립. 병행 가능 |

## Context

### Problem Statement

아이템 인스턴스는 `instance_id`로 식별된다(`equipment-skill-binding.md` B9의 `ItemInstance`).
사망 가방은 월드에 놓이고 다른 플레이어가 10분간 루팅한다. **둘 다 지금 서버에 식별자
체계가 없다.**

`item-equipment-system.md` S3이 정한 정책은 **"`instance_id`는 서버 전역·영구 유일"**
이다. 아이템이 Room 경계를 넘나들기 때문이다(탈출 → 베이스캠프 → 다른 층). Room 로컬
카운터만 쓰면 PK 충돌로 AC-IE-01(인스턴스 독립성)이 깨진다.

**지금 정하지 않으면**: DB 스키마의 PK 타입·범위를 정할 수 없고, `.proto`의 필드 폭도
정할 수 없다.

### Current State

서버에 ID 공간이 **이미 둘** 있고, 대역으로 갈려 있다.

```cpp
// ObjectUtils.cpp:6 — 런타임 오브젝트 전용
atomic<int64> ObjectUtils::s_idGenerator = 1'000'000'000;

// 플레이어는 DB PK 를 그대로 쓴다 (b62dde2)
PlayerRef player = ObjectUtils::CreatPlayer(gameSession, character.object_info().object_id());
```

| | 발급자 | 수명 | 대역 |
|---|---|---|---|
| 플레이어 `object_id` | **DB** (`characters.character_id`) | **영구** | 1 ~ 999,999,999 |
| 런타임 `object_id` | **프로세스** (atomic) | **프로세스 수명** | 1,000,000,000 ~ |

`ObjectType`은 현재 `NONE`·`CREATURE`·`PROJECTILE`·`ENV` 넷이다. 가방에 맞는 타입이 없다.

**한 숫자 공간을 나눠 쓰는 이유**: `S_SPAWN`·`S_MOVE`·`S_DAMAGE`가 전부 `object_id`
**한 필드**만 싣는다. 공간을 분리하면 패킷마다 "어느 종류 ID인가" 구분자가 필요해진다.

### Constraints

- 드랍은 **핫패스**다. 아이템 생성마다 DB 왕복을 넣을 수 없다
- 가방은 **10분간 살아야 하고 그 사이 서버가 재시작될 수 있다** — 런타임 카운터로는
  정체성이 유지되지 않는다
- ADR-0003이 던전 내 아이템을 DB에 쓰지 않기로 했다 — 그래도 `instance_id`는 **런 도중에도
  유일**해야 한다 (가방에 들어가고 남이 루팅한다)
- 단일 서버 프로세스다. 다중 프로세스는 현재 계획에 없다

### Requirements

- 아이템 `instance_id`는 **서버 전역·영구 유일**. 소유자가 바뀌어도 유지(행 UPDATE)
- 가방 ID는 **재시작을 넘어 유지**되어야 한다
- 아이템 생성 경로에 **DB 왕복이 없어야** 한다
- 로그에서 **ID만 보고 무엇인지 구분**할 수 있어야 한다

## Decision

### ① 아이템은 `Object`가 아니다 — `instance_id`는 별도 공간이다

```
인벤토리 안의 아이템   월드 좌표 없음 → Room::_objects 에 안 들어감
가방 안의 아이템       가방이 Object 이고, 아이템은 그 내용물
착용 중인 아이템       플레이어의 일부. 별도 오브젝트가 아님
```

**`ObjectInfo`에 실릴 일이 없으므로 `object_id` 공간과 대역을 나눌 필요조차 없다.**
`C_LOOT`·`C_EQUIP` 같은 아이템 전용 패킷이 `instance_id`를 직접 싣는다.

### ② 가방은 `Object`다 — `object_id` 공간의 신규 대역을 쓴다

가방은 월드 좌표가 있고, 보여야 하고, 상호작용된다. 기존 `S_SPAWN`/`S_DESPAWN`
브로드캐스트를 그대로 재사용한다.

**그런데 가방은 영속적인 Object다** — 몬스터와 다르다. 10분 만료 전에 서버가 재시작되면
복구되어야 하므로(ADR-0005), DB에 행이 있고 그 PK가 안정적이어야 한다.

**플레이어에게 한 것과 같은 패턴을 쓴다: DB PK를 그대로 통신 ID로.** 매핑 테이블이
필요 없고, 로그의 ID로 곧장 DB를 조회할 수 있다.

### 결과 ID 지도

```
object_id 공간  (ObjectInfo 에 실린다 · 한 필드로 화면 위 무엇이든 지목)
  1 ~ 999,999,999          플레이어    = characters.character_id     영속 (DB)
  1,000,000,000 ~          런타임      몬스터 · 투사체                프로세스 수명
  2,000,000,000 ~          가방        = bags.bag_id                 영속 (DB)   ← 신규

instance_id 공간  (아이템 전용 — ObjectInfo 에 안 실린다)
  1 ~                      아이템 인스턴스                            영속 (DB)   ← 신규
```

### ③ 두 카운터 모두 전역 atomic + 부팅 시 DB 복원

```cpp
// 부팅 시 1회
_nextInstanceId = SELECT COALESCE(MAX(instance_id), 0) FROM item_instances;
_nextBagId      = SELECT COALESCE(MAX(bag_id), 2'000'000'000) FROM bags;

// 이후 발급은 메모리에서 — DB 왕복 없음
uint64 newId = _nextInstanceId.fetch_add(1) + 1;
```

### Architecture

```
부팅
  ├─ SELECT MAX(instance_id) ──▶ atomic<uint64> GNextInstanceId
  └─ SELECT MAX(bag_id)      ──▶ atomic<uint64> GNextBagId
                                      │
                          이후 모든 발급은 메모리에서 (fetch_add)
                                      │
        ┌─────────────────────────────┴─────────────────────────────┐
        ▼                                                           ▼
   아이템 생성 (드랍·상자)                                    사망 가방 생성
   instance_id = GNextInstanceId++                          bag_id = GNextBagId++
   ADR-0003: 런 중엔 DB 에 안 씀                             ADR-0005: DB 우선 커밋
   탈출·사망 시 커밋                                          object_id 로도 그대로 사용
```

### Key Interfaces

```protobuf
// Enum.proto — 신규 타입 추가
enum ObjectType
{
    OBJECT_TYPE_NONE       = 0;
    OBJECT_TYPE_CREATURE   = 1;
    OBJECT_TYPE_PROJECTILE = 2;
    OBJECT_TYPE_ENV        = 3;
    OBJECT_TYPE_BAG        = 4;   // 🔴 신규 — 사망 가방
}
```

```cpp
// ID 발급 — ObjectUtils 옆에 두거나 별도 유틸
extern atomic<uint64> GNextInstanceId;   // 아이템. 1 부터
extern atomic<uint64> GNextBagId;        // 가방.   2,000,000,000 부터

// 부팅 시 반드시 호출. 안 하면 기존 행과 PK 충돌한다
bool RestoreIdCountersFromDB(DBConnection* conn);
```

### Implementation Guidelines

**ID 구멍은 문제가 아니다.** 아이템 생성 후 커밋 전에 크래시하면 그 ID는 영영 안 쓰인다.
`uint64`는 그 정도 낭비를 감당한다.

**부팅 복원은 접속 수락 전에 끝나야 한다.** 복원 전에 아이템이 생성되면 기존 행과
PK가 충돌한다. ADR-0005의 가방 만료 복구와 같은 구간에서 처리한다.

**대역 경계를 상수로 둔다.** `2'000'000'000`을 여러 곳에 흩뿌리지 말고 한 곳에서
정의한다 — 몬스터 대역(10억)이 이미 `ObjectUtils.cpp`에 하드코딩돼 있는데, 셋이 되면
한 파일에 모으는 편이 낫다.

**`instance_id`는 소유자가 바뀌어도 유지한다.** 루팅은 행의 `owner_character_id`를
UPDATE하는 것이지 새 행을 만드는 것이 아니다. 이것이 AC-IE-01(인스턴스 독립성)과
"기물은 주인을 기억한다"는 세계관 규칙을 동시에 만족시킨다.

## Alternatives Considered

### Alternative 1: 아이템도 `object_id` 공간에 대역으로 편입

- **Description**: `instance_id`를 없애고 아이템에도 `object_id`를 주되 대역을 나눈다
  (예: 30억~).
- **Pros**: ID 공간이 하나로 통일된다. 로그에서 대역만 보면 종류를 안다
- **Cons**: **아이템은 `Object`가 아닌데 `object_id`를 갖게 된다.** 인벤토리 안의 아이템은
  월드 좌표도 없고 `Room::_objects`에도 없다 — "오브젝트 ID를 가졌지만 오브젝트가 아닌 것"이
  생겨 개념이 흐려진다. 나중에 누군가 `_objects.find(instance_id)`를 시도하게 된다
- **Estimated Effort**: 같음
- **Rejection Reason**: 통일의 이득보다 **개념 혼동의 비용**이 크다. 두 종류의 식별자는
  실제로 다른 것을 가리킨다 — 하나는 "화면 위의 무엇", 하나는 "소지품 하나".

### Alternative 2: Room별 ID 블록 예약

- **Description**: 각 Room이 DB에서 ID 블록(예: 1000개)을 미리 받아 로컬에서 나눠준다.
  소진하면 다음 블록을 받는다.
- **Pros**: DB 접근이 블록당 1회로 더 적다. 다중 프로세스로 확장할 때 유리하다
- **Cons**: 재시작 시 미사용 블록이 통째로 버려진다. 블록 크기 튜닝이 필요하다.
  **단일 프로세스에서는 전역 atomic 대비 이득이 없다** — atomic `fetch_add`는 이미
  수 나노초다
- **Estimated Effort**: 채택안의 2배
- **Rejection Reason**: 현재 단일 프로세스다. **다중 프로세스로 갈 때 이 ADR을 대체하는
  새 ADR을 쓰면 된다** — 그때 필요한 정보가 지금보다 많을 것이다.

### Alternative 3: UUID / GUID

- **Description**: 128비트 랜덤 ID. 조율이 필요 없다.
- **Pros**: 발급에 상태가 없다. 다중 프로세스·샤딩에 그대로 확장된다
- **Cons**: **8바이트 → 16바이트.** `object_id`가 `uint64`인 기존 프로토콜과 안 맞아
  `ObjectInfo` 스키마를 바꿔야 한다. DB 인덱스가 커지고 정렬 지역성이 나빠진다.
  로그 가독성이 크게 떨어진다 (`160` vs `f47ac10b-58cc-...`)
- **Estimated Effort**: 채택안보다 크다 (프로토콜 변경 파급)
- **Rejection Reason**: 지금 규모(120 동접·단일 프로세스)에서 얻는 것이 없고,
  **로그로 DB를 곧장 조회하는 이점**(b62dde2에서 의도적으로 얻은 것)을 잃는다.

### Alternative 4: DB `AUTO_INCREMENT`

- **Description**: 아이템·가방 행을 INSERT하고 `LAST_INSERT_ID()`로 ID를 받는다.
- **Pros**: 유일성이 DB에서 보장된다. 부팅 복원 코드가 불필요하다
- **Cons**: **생성마다 DB 왕복.** 드랍은 핫패스이고, ADR-0003이 던전 내 DB 접근을 0으로
  만들기로 한 것과 정면 충돌한다
- **Estimated Effort**: 가장 적음
- **Rejection Reason**: `item-equipment-system.md` S3이 **"드랍 핫패스의 DB 왕복 회피"**를
  명시적 요구로 적었다. ADR-0003과도 충돌한다.

## Consequences

### Positive

- **아이템 생성에 DB 왕복이 없다** — 드랍 핫패스가 메모리 연산 하나로 끝난다
- 로그의 ID만 보고 **종류·수명·DB 조회 가능 여부**를 안다
- 가방이 기존 `S_SPAWN`/`S_DESPAWN` 경로를 그대로 탄다 — 새 브로드캐스트 코드가 없다
- 매핑 테이블이 없다 (`bag_id` = `object_id`)

### Negative

- **부팅 시 복원을 빠뜨리면 조용히 PK가 충돌한다.** 코드 한 줄 누락이 데이터 손상으로
  이어지는 종류의 위험이다 — § Risks 참조
- ID 공간이 셋이 되어 **대역 경계를 문서 없이는 알기 어렵다**
- 다중 프로세스로 확장하면 **이 ADR을 대체해야 한다**

### Neutral

- ID 구멍이 생긴다 (크래시로 버려진 ID). `uint64` 범위에서 무의미하다
- `ObjectType`에 값이 하나 늘어 클라이언트도 재생성이 필요하다

## Risks

| Risk | Probability | Impact | Mitigation |
|------|------------|--------|-----------|
| **부팅 복원 누락 → PK 충돌** | 중간 (한 줄 빠뜨리면 발생) | **매우 높음 — 데이터 손상** | 복원 실패 시 **서버를 기동하지 않는다.** 부분 기동보다 낫다 |
| 대역 경계 하드코딩이 흩어짐 | 중간 | 중간 (나중에 바꾸기 어려움) | 상수 한 곳에 모은다. 몬스터 대역도 함께 |
| 가방 ID가 20억을 소진 | 매우 낮음 | 낮음 | `uint64` 상한까지 여유가 압도적이다 |
| 다중 프로세스 확장 시 충돌 | 낮음 (계획 없음) | 높음 | 그 시점에 새 ADR. 블록 예약(Alt 2)이 후보 |

## Performance Implications

| Metric | Before | Expected After | Budget |
|--------|--------|---------------|--------|
| 아이템 생성당 DB 왕복 | — (미구현) | **0회** | — |
| 아이템 생성 비용 | — | atomic `fetch_add` 1회 (수 ns) | — |
| 부팅 시 추가 DB 조회 | 0회 | **2회** (`MAX` 조회) | 부팅 1회뿐 |
| `object_id` 필드 폭 | `uint64` | **`uint64` 유지** | 변화 없음 |

## Migration Plan

기존 ID 체계를 바꾸지 않는다 — 대역을 **추가**할 뿐이다.

1. `Enum.proto`에 `OBJECT_TYPE_BAG = 4` 추가 → `GenPackets.bat` → 4개 프로젝트 재빌드
2. 대역 상수를 한 파일로 모은다 (`ObjectUtils.cpp`의 `1'000'000'000` 포함)
3. `GNextInstanceId` · `GNextBagId` 선언 + 부팅 복원 함수
4. `GameServer.cpp` 기동 순서에 복원 삽입 — **`Listener` 시작 전**

**Rollback plan**: 대역 추가는 기존 값에 영향이 없다. `OBJECT_TYPE_BAG`은 새 enum 값이라
기존 클라가 모르는 값을 받으면 무시한다(proto3 기본 동작). 되돌리기 비용이 낮다.

## Validation Criteria

- [ ] 아이템 1,000개를 연속 생성해도 **DB 조회가 0건** 발생한다
- [ ] 서버를 재시작한 뒤 새 아이템을 만들면 `instance_id`가 **기존 최댓값보다 크다**
- [ ] 서버를 재시작한 뒤 가방을 만들면 `bag_id`가 **기존 최댓값보다 크다**
- [ ] 복원 쿼리를 인위적으로 실패시키면 **서버가 기동하지 않는다**
- [ ] 가방이 `S_SPAWN`으로 방송되고 클라가 `OBJECT_TYPE_BAG`으로 구분한다
- [ ] 아이템을 루팅해 소유자가 바뀌어도 `instance_id`가 **동일하다** (AC-IE-01)

## GDD Requirements Addressed

| GDD Document | System | Requirement | How This ADR Satisfies It |
|-------------|--------|-------------|--------------------------|
| `design/gdd/item-equipment-system.md` | 아이템 & 장비 | S3 — "`instance_id`는 서버 전역·영구 유일. 소유자가 바뀌어도 ID 유지" | 전역 atomic + 부팅 복원. 루팅은 행 UPDATE |
| `design/gdd/item-equipment-system.md` | 아이템 & 장비 | AC-IE-01 — 같은 종류의 두 인스턴스가 독립된 `instance_id`를 갖는다 | 카운터가 전역이라 종류와 무관하게 유일 |
| `design/gdd/item-equipment-system.md` | 아이템 & 장비 | § States — `DroppedInWorld` 가방이 월드 오브젝트로 존재 | 가방을 `Object`로 두고 `OBJECT_TYPE_BAG` 신설 |
| `design/gdd/equipment-skill-binding.md` | 장비-스킬 결속 | B9 — `ItemInstance { instance_id, item_type_id, skills }` | `instance_id` 공간을 이 ADR이 정의 |

## Related

- **ADR-0003** (레이드 경계 커밋) — 독립. 이 ADR의 "DB 왕복 없는 발급"이 그쪽의
  "던전 내 DB 접근 0" 요구를 만족시킨다
- **ADR-0005** (가방 영속화) — 이 ADR이 정한 `bag_id` 위에 `bags` 스키마를 짠다
- `design/gdd/item-equipment-system.md` § Dependencies > 서버 측 선행 작업 S3
- 구현 대상: `Server/Common/.../Enum.proto` · `Server/GameServer/ObjectUtils.cpp` ·
  `GameServer.cpp` (🔴 전부 사용자 소유 영역)
