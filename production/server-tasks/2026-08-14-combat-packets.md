# 서버 작업 제안 — P1 전투 패킷 정의

**작성**: 2026-08-14 · **소유**: 🔴 사용자 · **Phase**: P1 (전투, W3~W5 · 08-25 ~ 09-14)
**게이트**: G1 — SV 전체 재측정 + 8인 사다리 + **전투 패킷 대역폭 증가분 기록**

> 이 문서의 코드는 **2026-08-14 시점의 실제 `.proto` · `ServerPacketHandler.cpp` 를 읽고** 작성했습니다.
> 근거는 전부 `design/gdd/combat-system.md` 와 `design/registry/entities.yaml` 입니다.

---

## 선행 확정 사항 (전부 완료)

| 항목 | 값 | 출처 |
|---|---|---|
| 맨몸 기본 스탯 | 공격력 10 · 공속 1.0 · HP 500 · 저항 0 · 방어 0/0 | `entities.yaml: base_stats_naked` |
| 맨몸 공격 사거리 | **150 cm** | `entities.yaml: attack_range_naked` |
| 저항력 | 상한 1.0 · 최대 경감 **40%** | `cc_effective_duration` (공식 C4) |
| 하드 CC 중첩 | **최댓값 방식** (`max_end_time`) | `hard_cc_stack_model` |
| 소프트 CC 감속 | **최대값만**, 상한 40% | `soft_cc_slow_cap` (공식 C5) |
| 침묵 | **스킬만 차단, 평타 허용** | `silence_semantics` |
| 스킬 | **P1.5 로 분리** — 이 문서 범위 밖 | `roadmap.md` §8 결정 1 |

---

## 설계 원칙 3가지

### 1. 전투 이벤트는 손실 불가 — 이동과 구조가 다르다

2026-08-14 패킷 병합에서 이동은 **`set<id>` + 상태 스냅샷** 으로 바꿨다.
33ms 안에 세 번 움직이면 최종 위치 1개만 보낸다. 중간 경로는 버려도 되기 때문이다.

**전투는 버리면 안 된다.** 3연타를 1타로 합치면 데미지가 사라진다.

| | 합치기 | 자료구조 |
|---|---|---|
| 이동 | ✅ 마지막 것만 남김 | `unordered_set<uint64> _dirtyMovers` + `posInfo` 스냅샷 |
| **전투** | 🔴 **전부 보존** | `vector<Event>` — 쌓은 것을 전부 전송 |

### 2. 같은 33ms 배치에 태우되, 빈 배열은 보내지 않는다

이벤트마다 브로드캐스트하면 방금 없앤 O(N²)가 그대로 재발한다
(30인 기준 `Session::Send` 26,100/초 → 900/초 로 줄여 p95 를 330배 개선했다).

전투 없는 틱에는 패킷이 **0개** 나간다. 30인 난전이라도 공속 1.0 이면
초당 30회 공격 = **33ms 틱당 평균 1회** 다. "틱당 4~5개 패킷"은 최악의 경우다.

### 3. CC 는 이벤트 + **저주기** 스냅샷 (하이브리드)

| | 주기 | 목적 |
|---|---|---|
| `S_CC` (applied/expired) | **33ms 배치** | 적용 *순간* 을 알려 연출(피격 이펙트·사운드) 타이밍을 맞춘다 |
| `S_CC_STATE` (스냅샷) | **1초** | 정합성 보험. 이벤트를 놓쳤거나 늦게 들어온 클라를 자가 복구 |

스냅샷이 33ms 일 필요가 없다. 자가 복구가 목적이라 1Hz 면 충분하고 대역폭이 사실상 0이다.

---

## 왜 "타입별 배치" 인가 (단일 `oneof` 대신)

| 검토 | 결론 |
|---|---|
| 타입 분리가 지연을 만드나 | **아니다.** 4개든 1개든 같은 flush 에서 같은 순간에 나간다 |
| 33ms 배치가 전투에 느린가 | **아니다.** LoL 서버 틱이 정확히 30Hz 다. 우리 이동도 이미 30Hz |
| 내 공격이 굼떠 보이나 | **아니다.** GDD Rule 10 — 클라는 **내 windup 애니메이션을 입력 즉시** 재생한다. 배치는 *남의 행동을 내가 보는* 지연에만 영향 |
| 이벤트 간 순서는 | 같은 flush 안에서 `Send` 순서대로 큐에 들어가고 TCP 가 순서를 보장한다 |

> 🔴 **"중요한 이벤트는 즉시 보내자"는 예외를 지금 만들지 않는다.**
> 그것이 O(N²)가 되돌아오는 뒷문이다. **P1 게이트에서 실측하고, 체감 문제가
> 숫자로 확인되면 그때 예외를 판다.**

---

# STEP 1 — `Enum.proto` 맨 아래에 추가

```proto
enum DamageType
{
	DAMAGE_TYPE_PHYSICAL = 0;
	DAMAGE_TYPE_MAGIC = 1;
}

// 비트마스크로도 쓸 수 있게 2의 거듭제곱. combat-system.md Rule 5 표와 1:1 대응.
enum CcType
{
	CC_TYPE_NONE           = 0;
	CC_TYPE_STUN           = 1;   // 하드 - 이동X 회전X 평타X 스킬X
	CC_TYPE_ROOT           = 2;   // 하드 - 이동X 회전O 평타O 스킬O
	CC_TYPE_KNOCKBACK      = 4;   // 하드 - 강제 변위 (공식 C6)
	CC_TYPE_LAUNCH         = 8;   // 하드 - Z축 없음. Stun 과 기능 동일
	CC_TYPE_SLOW           = 16;  // 소프트 - 최대값만 중첩 (공식 C5)
	CC_TYPE_HEAL_REDUCTION = 32;  // 소프트
	CC_TYPE_SILENCE        = 64;  // 소프트 - 스킬만 차단, 평타는 허용
}

// 타이머 만료 사망은 여기 없다. combat-system.md Rule 4 -
// OnDied 는 전투(PvP/몬스터)로 인한 사망에서만 발생한다.
enum DeathCause
{
	DEATH_CAUSE_NONE    = 0;
	DEATH_CAUSE_PLAYER  = 1;
	DEATH_CAUSE_MONSTER = 2;
}
```

# STEP 2 — `Struct.proto` 맨 아래에 추가

```proto
message AttackInfo
{
	uint64 attacker_id = 1;
	uint64 target_id = 2;
	uint32 windup_ms = 3;      // 공식 C3. 클라는 이 길이에 애니메이션을 맞춘다
}

message DamageInfo
{
	uint64 attacker_id = 1;
	uint64 target_id = 2;
	int32 damage = 3;          // 완화 후 최종 (공식 C1)
	int32 remaining_hp = 4;    // 적용 후 남은 체력. 체력바는 이 값만 쓴다
	bool is_crit = 5;          // 공식 C2
	DamageType damage_type = 6;
}

message CcEventInfo
{
	uint64 target_id = 1;
	uint64 instigator_id = 2;
	CcType cc_type = 3;
	uint32 duration_ms = 4;    // 저항력 적용 후 실효값 (공식 C4)
	float magnitude = 5;       // Slow 감속률 등. 해당 없으면 0
}

message KnockbackInfo
{
	uint64 target_id = 1;
	uint64 instigator_id = 2;
	float dir_x = 3;
	float dir_y = 4;
	float distance = 5;        // 저항 경감 후 (공식 C6)
	float speed = 6;
}

message CcSlot
{
	CcType cc_type = 1;
	uint32 remaining_ms = 2;
	float magnitude = 3;
}

message CcStateInfo
{
	uint64 target_id = 1;
	repeated CcSlot slots = 2;
	float active_slow = 3;     // 집계 결과 (공식 C5). 클라 이동 애니메이션 속도용
}

message DiedInfo
{
	uint64 victim_id = 1;
	uint64 instigator_id = 2;
	DeathCause cause = 3;
}
```

# STEP 3 — `Protocol.proto` 의 `S_MOVE` 아래 · `C_CHAT` 위에 추가

```proto
// attacker_id 가 없다. 서버가 세션에서 가져온다 -
// Handle_C_MOVE 의 set_object_id(realId) 와 같은 원칙 (위조 차단).
message C_ATTACK
{
	uint64 target_id = 1;
}

message S_ATTACK
{
	repeated AttackInfo attacks = 1;
}

message S_ATTACK_CANCEL
{
	repeated uint64 attacker_ids = 1;
}

message S_DAMAGE
{
	repeated DamageInfo damages = 1;
}

message S_CC
{
	repeated CcEventInfo applied = 1;
	repeated CcEventInfo expired = 2;
	repeated KnockbackInfo knockbacks = 3;
}

// 1초 주기 정합성 스냅샷. 이벤트를 놓쳤거나 늦게 들어온 클라를 자가 복구시킨다.
message S_CC_STATE
{
	repeated CcStateInfo states = 1;
}

message S_DIED
{
	repeated DiedInfo deaths = 1;
}
```

---

## 각 필드가 GDD 의 어느 줄에 대응하는가

| 패킷 · 필드 | 근거 |
|---|---|
| `C_ATTACK` 에 `attacker_id` 없음 | `ServerPacketHandler.cpp:83-90` 의 위조 차단 패턴 계승 |
| `S_ATTACK.windup_ms` | Rule 1 · 공식 C3 — 클라가 windup 길이를 알아야 애니메이션이 맞는다 |
| `S_ATTACK_CANCEL` | Rule 1 — 사거리 이탈 · LoS 상실 · Stun 시 취소(쿨다운 미소모) |
| `DamageInfo.remaining_hp` | Rule 10 — **클라가 체력을 스스로 빼면 안 된다.** 서버 값을 그대로 표시 |
| `DamageInfo.is_crit` 별도 필드 | Rule 10 — 크리 시각 강조는 클라 몫, **판정은 서버** |
| `KnockbackInfo` 분리 | Rule 11 — 넉백만 방향·거리·속도가 필요하고 `moveException` 등록과 짝을 이룬다 |
| `CcStateInfo.active_slow` | 공식 C5 — 집계는 서버, 클라는 애니메이션 속도만 맞춘다 |
| `DeathCause` 에 타이머 없음 | Rule 4 — 타이머 만료 사망은 `OnDied` 를 거치지 않는다 |
| `CcType` 비트값 | Rule 5 표 7종과 1:1. 스냅샷에서 마스크로 쓰기 위함 |

---

## 적용 순서

```
1  Enum.proto → Struct.proto → Protocol.proto   (의존 순서 고정)
2  Server\Common\protoc-21.12-win64\bin\GenPackets.bat  더블클릭
3  ServerPacketHandler.cpp 에 Handle_C_ATTACK 구현              ← GameServer
4  ClientPacketHandler.cpp 에 S_* 핸들러 6개 구현               ← DummyClient · S1 양쪽
5  솔루션 다시 빌드 (Release|x64)
```

> 🔴 **② 를 하면 `.h` 에 핸들러 선언이 생긴다. `.cpp` 에 구현이 없으면 링크 에러다.**
> ③④ 는 껍데기(`return true;`)로 먼저 채워 빌드를 통과시킨 뒤 내용을 붙이는 게 안전하다.

> ⚠️ **`GenPackets.bat` 은 빌드의 사전 이벤트로 돌지 않는다.** 2026-08-14 빌드 로그에
> `'protoc.exe'은(는) 내부 또는 외부 명령...` 이 5회 찍혔다. `.proto` 를 바꿀 때마다
> **탐색기에서 직접 더블클릭**할 것. (bat 끝의 `PAUSE` 로 결과를 눈으로 확인할 수 있다.)

## 검증

| 항목 | 기대 |
|---|---|
| 빌드 | 서버 · DummyClient · S1 전부 에러 0 |
| 왕복 | `C_ATTACK` → `S_ATTACK` → `S_DAMAGE` 가 로그에 순서대로 |
| **G1 대역폭** | 30인 전투 중 `sent` 증가분 기록 — SV-5 기준 12 Mbps 대비 여유 확인 |
| SV-1 | 전투 추가 후에도 30인 p95 < 16,600 us 유지 |

> **G1 은 "전투 패킷 대역폭 증가분 기록"을 명시적으로 요구한다.** 이동만 있을 때의
> 30인 기준선은 `sent 3.20 Mbps · p95 ≤100 us` 다 (`roadmap.md` §0-1). 이 값과 비교할 것.

---

## 다음 단계 (이 문서 범위 밖)

1. `Room` 전투 배치 구조 — `vector<AttackInfo> _pendingAttacks` 등 + `FlushCombat()`
2. `Handle_C_ATTACK` — 세션에서 attacker 파생 · 사거리 150cm · LoS 재검증
3. `Creature` 에 `health` · `attack_power` · `attack_speed` · `resistance` 어트리뷰트
4. CC 컨테이너 — 하드 CC `max_end_time` 중첩 · 소프트 CC 소스별 독립 + 최대값 집계
5. **SV-3 재설계** — 소프트 CC 를 이동 검증의 `GetSpeedCeiling` 에 반영 (P1 필수)
