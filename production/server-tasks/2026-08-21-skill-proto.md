# 스킬 시스템 `.proto` 제안서 — P1.5 착수 선행

> **작성** 2026-08-21 · **대상** 🔴 사용자 소유 영역 (`.proto` · `Server/**`)
> **근거 문서** `design/gdd/skill-system.md` B2 · `design/gdd/equipment-skill-binding.md` B8·B9
> **현재 코드 확인 완료** — `Enum.proto` · `Struct.proto` · `Protocol.proto` · `Room.h/cpp` · `pch.h` · `GenPackets.bat`

---

## 왜 지금인가

`.proto`에 **스킬 관련 정의가 0건**입니다. 지금이 가장 싼 시점입니다 — `GenPackets.bat`은
서버·DummyClient·UE 클라 **세 곳의 핸들러를 동시에 재생성**하므로, 나중에 고치면 세 곳을
다시 맞춰야 합니다.

---

## 🟢 먼저 — 리뷰가 지목한 선행 작업 1건이 불필요해졌습니다

리뷰에서 **"`Room::Broadcast`가 수신자별 차등 페이로드를 지원하지 않아 정보 비대칭을
구현할 수 없다"** 가 블로커로 나왔습니다. **코드를 읽어보니 그 작업이 필요 없습니다.**

```cpp
// Room.cpp:685 — 현재 코드
void Room::Broadcast(SendBufferRef sendBuffer, uint64 exceptId)
```

차등 페이로드가 필요 없는 이유는 **비대칭이 "서버가 무엇을 보내느냐"가 아니라
"각 클라가 무엇을 이미 아느냐"에서 나오기 때문**입니다.

```
서버는 전원에게 똑같은 것을 보낸다  →  { caster_id, is_stationary }

시전자 클라   자기 slot → skill_id 를 이미 안다 (S_EQUIP_SYNC)
              → skill_id → cast_ms 를 정적 스킬 테이블에서 조회
              → 남은 시간 게이지를 그린다                            ✅

타인 클라     그 caster 의 skill_id 를 모른다
              → "캐스트 중" 표식만 그린다                             ✅
```

**`cast_ms`를 아예 전송하지 않습니다.** 보내지 않으니 새어나갈 것도 없습니다.
`Room::Broadcast`는 지금 그대로 씁니다.

> 같은 원리로 `wall_pierce` 실루엣도 문제가 없습니다 — 자기 버프 상태는 소유자에게만
> 유니캐스트되므로 타인은 애초에 받지 않습니다.

---

# STEP 1 — `Enum.proto`

**파일** `Server/Common/protoc-21.12-win64/bin/Enum.proto`

## 현재 코드 (파일 끝부분)

```protobuf
enum DeathCause
{
	DEATH_CAUSE_NONE = 0;
	DEATH_CAUSE_PLAYER = 1;
	DEATH_CAUSE_MONSTER = 2;
}
```

## 바꿀 코드 — 위 블록 **뒤에** 추가

```protobuf
enum EquipSlot
{
	SLOT_NONE = 0;
	SLOT_WEAPON_PRIMARY = 1;
	SLOT_WEAPON_SECONDARY = 2;
	SLOT_HELMET = 3;
	SLOT_ARMOR = 4;
	SLOT_BOOTS = 5;
	SLOT_TRINKET = 6;
}

enum SlotBindState
{
	BIND_UNBOUND = 0;
	BIND_BOUND = 1;
	BIND_SHADOWED = 2;
}
```

## 왜

- `EquipSlot` — 6키를 타입으로 고정합니다. `equipment-skill-binding.md` B9.
  **`SLOT_NONE = 0`을 둔 이유**: proto3는 미설정 필드가 0이 되므로, 0을 실제 슬롯에
  주면 "안 채운 것"과 "Shift"가 구분되지 않습니다. 기존 `ObjectType`·`CcType`도 같은
  이유로 `_NONE = 0`을 씁니다 — **프로젝트 관행과 일치합니다.**
- `SlotBindState` — `Shadowed` 판정은 서버가 하고 클라는 받아서 회색 처리만 합니다
  (`equipment-skill-binding.md` B5·B7).

---

# STEP 2 — `Struct.proto`

**파일** `Server/Common/protoc-21.12-win64/bin/Struct.proto`

## 현재 코드 (파일 끝부분)

```protobuf
message DiedInfo
{
	uint64 victim_id = 1;
	uint64 instigator_id = 2;
	DeathCause cause = 3;
}
```

## 바꿀 코드 — 위 블록 **뒤에** 추가

```protobuf
message SkillInfo
{
	EquipSlot slot = 1;
	uint32 skill_id = 2;
	SlotBindState bind_state = 3;
	EquipSlot shadowed_by = 4;
}

message SkillCastInfo
{
	uint64 caster_id = 1;
	bool is_stationary = 2;
}

message SkillHitInfo
{
	uint64 caster_id = 1;
	uint32 effect_id = 2;
	uint64 target_id = 3;
	float impact_x = 4;
	float impact_y = 5;
}
```

## 왜

**`SkillInfo`** — `equipment-skill-binding.md` B9. 슬롯이 원소 안에 있으므로
배열 순서 규약이 필요 없고, 무기 2개·나머지 1개가 자연히 표현됩니다.
`shadowed_by`는 로드아웃 화면이 *"같은 스킬이 `Z`에 있음"* 을 표시하는 데 씁니다.

> 🔴 **GDD B9에는 `effect_id`가 `SkillInfo` 안에 있으나 여기서는 뺐습니다.**
> 소유자는 `skill_id`를 아니까 정적 스킬 테이블에서 `effect_id`를 조회할 수 있어
> 중복입니다. `effect_id`가 실제로 필요한 곳은 **`skill_id`를 모르는 관찰자**뿐이라
> `SkillHitInfo`에만 둡니다. GDD를 이 형태로 맞추겠습니다.

**`SkillCastInfo`** — 위 § 선행 작업 설명대로 **`skill_id`·`cast_ms`가 없습니다.**
`is_stationary`는 정지 캐스트 발밑 표식용입니다(`skill-system.md` § Visual/Audio).

**`SkillHitInfo`** — `effect_id`가 여기 있습니다. 관찰자도 고유 연출을 재생합니다
(`equipment-skill-binding.md` B8). `impact_x/y`는 `line`·`circle_point`의 착탄 지점,
`target_id`는 `single` 전용(없으면 0)입니다.

---

# STEP 3 — `Protocol.proto`

**파일** `Server/Common/protoc-21.12-win64/bin/Protocol.proto`

## 현재 코드

```protobuf
message S_DIED
{
	repeated DiedInfo deaths = 1;
}

message C_CHAT
{
	string msg = 1;
}
```

## 바꿀 코드 — `S_DIED`와 `C_CHAT` **사이에** 추가

```protobuf
message C_SKILL
{
	EquipSlot slot = 1;
	uint64 target_id = 2;
	float aim_x = 3;
	float aim_y = 4;
}

message C_SKILL_CANCEL
{

}

message S_SKILL_CAST
{
	repeated SkillCastInfo casts = 1;
}

message S_SKILL_CANCEL
{
	repeated uint64 caster_ids = 1;
}

message S_SKILL_HIT
{
	repeated SkillHitInfo hits = 1;
}

message S_EQUIP_SYNC
{
	repeated SkillInfo skills = 1;
}
```

## 왜

| 메시지 | 근거 |
|---|---|
| **`C_SKILL`** | **슬롯 번호만 보냅니다.** `skill_id`도 `caster_id`도 클라가 채우지 않습니다 — 서버가 세션에서 시전자를, 슬롯에서 스킬을 조회합니다 (`equipment-skill-binding.md` B7) |
| **`C_SKILL_CANCEL`** | 비어 있습니다. 서버가 이 세션의 진행 중 캐스트를 알고 있습니다. **30Hz 위치 스트림에서 추론하지 않는 것**이 핵심입니다 (`skill-system.md` B2 c) |
| **`S_SKILL_CAST`** | 33ms 배치 (`skill-system.md` B2 b). `S_ATTACK`과 같은 모양 |
| **`S_SKILL_CANCEL`** | `S_ATTACK_CANCEL`과 동일한 형태 — 관찰자가 "캐스트 중" 표식을 지웁니다 |
| **`S_SKILL_HIT`** | 판정 결과의 **연출용**입니다. 실제 데미지·CC는 기존 `S_DAMAGE`·`S_CC`가 그대로 나릅니다 |
| **`S_EQUIP_SYNC`** | **소유자에게만 유니캐스트.** `Broadcast`를 쓰지 않습니다 (B8) |

> **`aim_x/y`는 커서 월드 좌표입니다.** `movement-camera.md`의 `GetCursorWorldPosition()`이
> 항상 유효값을 반환하므로 실패 경로가 없습니다. `single`은 `target_id`만 쓰고 `aim`은 0입니다.

---

# STEP 4 — 패킷 생성

```
탐색기에서 Server\Common\protoc-21.12-win64\bin\GenPackets.bat 더블클릭
```

🔴 **빌드 전 이벤트로 돌리지 마세요.** 이 프로젝트에서 `protoc.exe`를 찾지 못해
5회 실패한 이력이 있습니다. **탐색기에서 직접 실행**해야 합니다.

## 성공 확인

```
Enum.proto · Struct.proto · Protocol.proto  세 줄 모두 오류 메시지 없음
GenPackets.exe 두 줄 실행됨
XCOPY 가 GameServer · DummyClient · S1/Source/S1/Network 세 곳에 복사
```

🔴 **`.pb.h`가 실제로 갱신됐는지 확인하세요.** `GenPackets.bat`은 `protoc` 실패를
`IF ERRORLEVEL 1`로 잡지만, **핸들러 `.h`만 새로 생기고 `.pb.h`가 낡은 채로 남는 사고가
실제로 있었습니다.**

```bash
# GameServer/Protocol.pb.h 에 새 메시지가 들어갔는지
grep -c "C_SKILL\|S_EQUIP_SYNC" Server/GameServer/Protocol.pb.h
```

---

# 빌드 순서

```
1  GenPackets.bat            (탐색기에서 더블클릭)
2  ServerCore    빌드         ← 먼저. 안 하면 구버전 .lib 링크
3  GameServer    빌드
4  DummyClient   빌드
5  S1 (UE)       빌드         ← Protocol.pb.* 가 바뀌었으므로 필수
```

> 🔴 `ServerCore` 선행은 이 프로젝트에서 **세 번 겪은 함정**입니다.
> `GameServer.vcxproj`에 ServerCore 프로젝트 참조가 없어 MSBuild가 변경을 감지하지 못합니다.

---

# 검증

## 1. 컴파일만으로 확인되는 것

```
서버·DummyClient·UE 클라 세 곳 모두 빌드 성공
→ .proto 문법 · 세 곳 동기화 확인 완료
```

## 2. 패킷 ID 확인

`ServerPacketHandler.h`·`ClientPacketHandler.h`가 재생성되며 새 패킷 ID가 부여됩니다.
**기존 패킷 ID가 밀렸는지 확인하세요** — `C_CHAT` 앞에 6개를 끼워 넣었으므로
`C_CHAT`·`S_CHAT`의 ID가 바뀝니다. 서버·클라를 **둘 다** 새로 빌드하면 문제없지만,
한쪽만 빌드하면 채팅이 깨집니다.

## 3. 이 단계에서 확인 못 하는 것

핸들러 구현이 아직 없으므로 **동작 검증은 다음 단계**입니다. 이번 STEP의 목표는
**"스킬 패킷이 세 곳에서 같은 모양으로 존재한다"** 까지입니다.

---

# 이번에 하지 않는 것 — 범위 밖

| 항목 | 왜 미루나 |
|---|---|
| `ItemInstance` · 인벤토리 메시지 | **P2(아이템 & 장비) 소관.** `equipment-skill-binding.md` EQ1이 "아이템에 인스턴스 개념이 있는가"를 P2 최우선 미결로 둠 |
| 핸들러 구현 (`Handle_C_SKILL` 등) | `.proto` 확정 후 별도 단계 |
| 서버 캐스트 상태기 · 쿨다운 슬롯 귀속 | 위와 같음 |
| `DummyClient` `C_SKILL` 발신 | AC 실행용. 패킷이 생긴 뒤 |

---

# 다음 단계 미리보기

이 STEP이 통과하면 순서는 이렇습니다.

```
STEP A  서버 스킬 데이터 테이블 + 슬롯 저장소 (SkillInfo 6칸)
STEP B  Handle_C_SKILL — 검증 6종 · 캐스트 타이머 · FlushCombat 배치 편입
STEP C  Handle_C_SKILL_CANCEL
STEP D  DummyClient C_SKILL 발신  →  SK-43/44 성능 측정 가능
STEP E  UE 클라 — 스킬 바 · 캐스트 게이지 · effect_id 연출
```
