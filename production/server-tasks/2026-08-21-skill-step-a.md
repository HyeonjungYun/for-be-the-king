# STEP A — 스킬 데이터 테이블 + 슬롯 저장소

> **작성** 2026-08-21 · **대상** 🔴 사용자 소유 영역 (`Server/GameServer/**`)
> **선행** `.proto` STEP 1~4 완료 (빌드 성공 확인됨)
> **근거** `equipment-skill-binding.md` B1·B5·B7·B9 · `skill-system.md` R1·R2·S3
> **현재 코드 확인 완료** — `Creature.h/cpp` · `Player.h` · `Room.cpp` · `ObjectUtils.cpp` · `GameServer.vcxproj`

---

## 이 STEP의 범위

**데이터 구조만 만듭니다. 발동 로직은 STEP B입니다.**

```
✅ 이번에 하는 것
   스킬 정의 테이블 (정적 데이터)
   Creature 에 슬롯 6칸 저장소
   Shadowed 판정 (고정 우선순위)
   S_EQUIP_SYNC 송신 (입장 시)

❌ 이번에 안 하는 것
   Handle_C_SKILL · 캐스트 타이머 · 판정 · 쿨다운 소모   →  STEP B
   아이템에서 skill_id 를 가져오는 것                    →  P2 (지금은 하드코딩)
```

> 🔴 **스킬 콘텐츠는 아직 미정입니다** (`skill-system.md` Q6). 이 STEP은 **배관**을 만드는
> 것이고, 테이블에 넣는 스킬 2종은 **배관을 흐르게 할 임시 데이터**입니다. 손맛이 정해지면
> 그때 교체합니다 — 구조는 안 바뀝니다.

---

## ⚠️ 먼저 — `.proto` 에서 발견한 이름 오류 1건

`Protocol.proto:117` 을 읽어보니 제안서에 쓴 `C_SKILL_CANCEL` 이 아니라
**`C_SKILL_CAST`** 로 들어가 있습니다.

```protobuf
message C_SKILL_CAST     // ← 취소 패킷인데 이름이 CAST 다
{
}
```

`S_SKILL_CAST`(121행)가 이미 "누군가 캐스트를 시작했다"는 뜻이라 **이름이 정반대로 겹칩니다.**
그리고 "클라가 스킬을 쓰겠다"는 이미 `C_SKILL`(109행)이 담당합니다.

**STEP A 에는 영향이 없습니다** (이 패킷을 안 씁니다). 다만 **STEP C 에서 핸들러 이름이
`Handle_C_SKILL_CAST` 로 생성되므로** 그 전에 고치는 게 낫습니다.

| | |
|---|---|
| 고칠 곳 | `Protocol.proto:117` — `C_SKILL_CAST` → `C_SKILL_CANCEL` |
| 패킷 ID | **안 밀립니다.** 선언 위치가 그대로라 ID도 그대로입니다 (삽입이 아니라 개명) |
| 재빌드 | `GenPackets.bat` 후 서버·클라 양쪽. 아직 이 패킷을 쓰는 코드가 없어 안전합니다 |

> 지금 고치실지, STEP C 때 같이 하실지는 편한 쪽으로 하세요.
> **지금이 가장 쌉니다** — 참조하는 코드가 0줄입니다.

---

# STEP A-1 — `SkillTable.h` (신규 파일)

**파일** `Server/GameServer/SkillTable.h` — **새로 만듭니다**

```cpp
#pragma once
#include "Enum.pb.h"

// 스킬 정의 테이블 — 정적 데이터. 런타임에 바뀌지 않는다.
//   설계 근거: design/gdd/skill-system.md R2 (스킬 구성 요소) · R12 (판정 형태)
//   🔴 클라이언트도 같은 테이블을 갖는다. skill_id 만 받으면 나머지를 조회할 수 있으므로
//      cast_ms 같은 값을 패킷으로 보내지 않는다 (equipment-skill-binding.md B8).

enum class SkillShape : uint8
{
	Single = 0,		// 대상 지정 — 커서 아래 적을 락
	Line,			// 논타겟 — 커서 방향 직선
	CircleSelf,		// 조준 없음 — 시전자 중심 원
	CirclePoint,	// 논타겟 — 커서 위치 중심 원
};

enum class SkillEffectType : uint8
{
	Damage = 0,
	HardCc,
	Knockback,
	SoftCc,
	Movement,
	SelfBuff,
};

enum class SkillBuffType : uint8
{
	None = 0,
	MoveSpeed,		// effective_move_speed 의 percent_ms_bonus 항에 합류
	WallPierce,		// 판정이 벽을 무시한다 (skill-system.md R13)
};

struct SkillEffect
{
	SkillEffectType	type = SkillEffectType::Damage;

	// Damage
	float			adRatio = 0.f;			// [0, 3.0]
	float			apRatio = 0.f;			// [0, 3.0]
	Protocol::DamageType damageType = Protocol::DAMAGE_TYPE_PHYSICAL;

	// HardCc · SoftCc
	Protocol::CcType ccType = Protocol::CC_TYPE_NONE;
	uint32			ccDurationMs = 0;		// 상한은 skill-system.md S1
	float			ccMagnitude = 0.f;		// 감속 전용. 상한 0.40

	// Knockback · Movement — 🔴 speed >= dist / 0.4 를 지켜야 한다 (S2)
	float			distCm = 0.f;
	float			speedCms = 0.f;
	bool			ignoresWalls = false;	// Movement 전용 (R13)

	// SelfBuff
	SkillBuffType	buffType = SkillBuffType::None;
	float			buffMagnitude = 0.f;
	uint32			buffDurationMs = 0;
};

struct SkillDef
{
	uint32			skillId = 0;
	uint32			effectId = 0;			// 연출 애셋 참조. 관찰자에게 전송되는 유일한 식별자

	uint32			castMs = 0;				// 0 이면 즉발 — Casting 상태를 거치지 않는다
	uint32			cooldownMs = 0;
	bool			canMoveWhileCasting = false;

	SkillShape		shape = SkillShape::Single;
	float			rangeCm = 0.f;			// 형태별 의미는 R12 표
	float			widthCm = 0.f;			// Line 전용
	float			radiusCm = 0.f;			// CircleSelf · CirclePoint 전용

	vector<SkillEffect> effects;

	bool			HasHardCc() const;
	bool			HasMovement() const;
};

class SkillTable
{
public:
	static void				Init();
	static const SkillDef*	Find(uint32 skillId);	// 없으면 nullptr

private:
	static unordered_map<uint32, SkillDef> s_defs;
};
```

## 왜

- **`SkillShape`·`SkillEffectType`을 `.proto` enum이 아니라 C++ enum으로 둡니다.**
  이 값들은 **네트워크로 나가지 않습니다** — 클라가 `skill_id`로 자기 테이블에서 조회하므로
  서버·클라가 같은 정의를 각자 갖기만 하면 됩니다. `.proto`를 불필요하게 키우지 않습니다.
- **`effectId`가 `SkillDef`에 있습니다.** 관찰자에게 나가는 유일한 식별자입니다 (B8).
- **`HasHardCc()`** — `equipment-skill-binding.md` E2(무기 하드 CC 재추첨)와
  `skill-system.md` R7(무기 부위 전용) 검사에 씁니다.

---

# STEP A-2 — `SkillTable.cpp` (신규 파일)

**파일** `Server/GameServer/SkillTable.cpp` — **새로 만듭니다**

```cpp
#include "pch.h"
#include "SkillTable.h"

unordered_map<uint32, SkillDef> SkillTable::s_defs;

bool SkillDef::HasHardCc() const
{
	for (const SkillEffect& e : effects)
	{
		if (e.type != SkillEffectType::HardCc && e.type != SkillEffectType::Knockback)
			continue;

		// 하드 CC = 기절 · 속박 · 넉백 · 공중띄우기
		if (e.ccType == Protocol::CC_TYPE_STUN
			|| e.ccType == Protocol::CC_TYPE_ROOT
			|| e.ccType == Protocol::CC_TYPE_KNOCKBACK
			|| e.ccType == Protocol::CC_TYPE_LAUNCH)
			return true;

		if (e.type == SkillEffectType::Knockback)
			return true;
	}
	return false;
}

bool SkillDef::HasMovement() const
{
	for (const SkillEffect& e : effects)
	{
		if (e.type == SkillEffectType::Movement)
			return true;
	}
	return false;
}

void SkillTable::Init()
{
	s_defs.clear();

	// ────────────────────────────────────────────────────────────────
	// 🔴 임시 데이터 — 배관 검증용
	//    skill-system.md Q6("P1.5 에 넣을 스킬 2~3종")이 아직 미정이다.
	//    손맛이 정해지면 이 블록만 교체한다. 구조는 그대로다.
	// ────────────────────────────────────────────────────────────────

	// [1001] 후려치기 — 무기 · 하드 CC(기절) + 물리 데미지
	{
		SkillDef def;
		def.skillId = 1001;
		def.effectId = 1;
		def.castMs = 300;
		def.cooldownMs = 8000;
		def.canMoveWhileCasting = false;
		def.shape = SkillShape::Single;
		def.rangeCm = 400.f;

		SkillEffect dmg;
		dmg.type = SkillEffectType::Damage;
		dmg.adRatio = 2.0f;
		dmg.damageType = Protocol::DAMAGE_TYPE_PHYSICAL;
		def.effects.push_back(dmg);

		SkillEffect stun;
		stun.type = SkillEffectType::HardCc;
		stun.ccType = Protocol::CC_TYPE_STUN;
		stun.ccDurationMs = 1000;		// 상한 1250 (S1) 이하
		def.effects.push_back(stun);

		s_defs[def.skillId] = def;
	}

	// [2001] 짓쳐들기 — 신발 · 이동(대시)
	{
		SkillDef def;
		def.skillId = 2001;
		def.effectId = 2;
		def.castMs = 60;				// 🔴 이동 스킬은 castMs >= 60 (skill-system.md R10)
		def.cooldownMs = 6000;
		def.canMoveWhileCasting = false;
		def.shape = SkillShape::CircleSelf;
		def.radiusCm = 0.f;

		SkillEffect move;
		move.type = SkillEffectType::Movement;
		move.distCm = 400.f;
		move.speedCms = 1200.f;			// 400 / 1200 = 0.333s — [0.15, 0.4] 안
		move.ignoresWalls = false;
		def.effects.push_back(move);

		s_defs[def.skillId] = def;
	}
}

const SkillDef* SkillTable::Find(uint32 skillId)
{
	auto it = s_defs.find(skillId);
	if (it == s_defs.end())
		return nullptr;

	return &it->second;
}
```

## 왜 이 두 개인가

배관을 전부 통과시키는 **최소 조합**입니다.

| | 검증되는 경로 |
|---|---|
| `1001` 후려치기 | 캐스트기(`castMs > 0`) · 하드 CC · `Single` 조준 · 데미지 |
| `2001` 짓쳐들기 | 이동 스킬 · `moveException` 순서 계약 · `castMs >= 60` 제약 |

> **수치 근거를 주석으로 남겼습니다** — 기절 1000ms는 상한 1250 이하(S1),
> 대시 400/1200 = 0.333초는 `dash_duration` [0.15, 0.4] 안(S2). 나중에 값을 바꿀 때
> 무엇을 지켜야 하는지 코드에서 바로 보입니다.

---

# STEP A-3 — `Creature.h` 수정

**파일** `Server/GameServer/Creature.h`

## 현재 코드

```cpp
struct SlowSource
{
	float	magnitude = 0.f;
	uint64	endUs = 0;
};
```

## 바꿀 코드 — 위 블록 **뒤에** 추가

```cpp
// 스킬 슬롯 — 6칸 고정 (equipment-skill-binding.md B1)
//   인덱스는 Protocol::EquipSlot 값 - 1 (SLOT_NONE = 0 을 빼고 0~5 로 압축)
constexpr int32 SKILL_SLOT_COUNT = 6;

struct SkillSlot
{
	uint32					skillId = 0;			// 0 = 비어 있음
	Protocol::SlotBindState	bindState = Protocol::BIND_UNBOUND;
	Protocol::EquipSlot		shadowedBy = Protocol::SLOT_NONE;

	// 🔴 쿨다운은 슬롯에 귀속된다. 장비를 바꿔도 남는다.
	//    (skill-system.md § Edge Cases — 장비 스왑 쿨다운 리셋 악용 차단)
	uint64					cooldownEndUs = 0;
};
```

## 현재 코드 (`Creature` 클래스 안, 스탯 블록)

```cpp
public:
	int32				maxHp				= BaseStats::MAX_HP;
	int32				hp				= BaseStats::MAX_HP;
	int32				attackPower		= BaseStats::ATTACK_POWER;
```

## 바꿀 코드

```cpp
public:
	int32				maxHp				= BaseStats::MAX_HP;
	int32				hp				= BaseStats::MAX_HP;
	int32				attackPower		= BaseStats::ATTACK_POWER;
	int32				magicPower		= BaseStats::MAGIC_POWER;
```

## 현재 코드 (`namespace BaseStats` 안)

```cpp
	constexpr int32		ATTACK_POWER				= 10;
```

## 바꿀 코드

```cpp
	constexpr int32		ATTACK_POWER				= 10;
	constexpr int32		MAGIC_POWER					= 0;
```

## 현재 코드 (파일 끝, 평타 상태 블록)

```cpp
public:
	uint64				attackTargetId	= 0;
	uint64				attackHitAtUs	= 0;
	uint64				attackReadyAtUs	= 0;
};
```

## 바꿀 코드

```cpp
public:
	uint64				attackTargetId	= 0;
	uint64				attackHitAtUs	= 0;
	uint64				attackReadyAtUs	= 0;

public:
	// 슬롯 조회 — Protocol::EquipSlot 을 배열 인덱스로 변환한다
	SkillSlot*			GetSlot(Protocol::EquipSlot slot);
	const SkillSlot*	GetSlot(Protocol::EquipSlot slot) const;

	// 🔴 중복 skill_id 를 고정 우선순위로 판정한다 (equipment-skill-binding.md B5)
	//    슬롯 내용이 바뀔 때마다 반드시 호출한다.
	void				RefreshBindStates();

	SkillSlot			skillSlots[SKILL_SLOT_COUNT];
};
```

## 왜

- **`magicPower` 추가** — `skill-system.md` S3의 마법 항이 이 값을 요구합니다.
  맨몸 0이므로 P1.5에서는 마법 계수가 무의미하지만, **필드가 없으면 공식을 구현할 수 없습니다.**
  `base_stats_naked`에도 `magic_power: 0`으로 등록돼 있습니다.
- **`skillSlots`를 `Creature`에 둡니다** — `Player`가 아니라. 몬스터도 스킬을 쓸 가능성이
  있고(`skill-system.md` Q5), 기존 `CanUseSkill()`도 `Creature`에 있어 일관됩니다.
- **`cooldownEndUs`가 슬롯 안에 있는 것이 핵심입니다.** 아이템이 아니라 슬롯에 붙어야
  장비 교대로 쿨다운을 초기화하는 악용이 막힙니다.

---

# STEP A-4 — `Creature.cpp` 수정

**파일** `Server/GameServer/Creature.cpp`

## 현재 코드 (파일 끝부분)

```cpp
bool Creature::CanUseSkill(uint64 nowUs) const
{
	return CanAttack(nowUs) && (nowUs >= silenceEndUs);
}
```

## 바꿀 코드 — 위 함수 **뒤에** 추가

```cpp
SkillSlot* Creature::GetSlot(Protocol::EquipSlot slot)
{
	// SLOT_NONE(0) 은 유효한 슬롯이 아니다. 1~6 을 0~5 로 압축한다.
	const int32 index = static_cast<int32>(slot) - 1;
	if (index < 0 || index >= SKILL_SLOT_COUNT)
		return nullptr;

	return &skillSlots[index];
}

const SkillSlot* Creature::GetSlot(Protocol::EquipSlot slot) const
{
	const int32 index = static_cast<int32>(slot) - 1;
	if (index < 0 || index >= SKILL_SLOT_COUNT)
		return nullptr;

	return &skillSlots[index];
}

void Creature::RefreshBindStates()
{
	// 배열 순서가 곧 우선순위다.
	//   무기(Shift) > 무기(Q) > 투구 > 갑옷 > 신발 > 장신구
	//   앞에서 이미 나온 skill_id 를 뒤에서 또 만나면 뒤쪽이 Shadowed 가 된다.
	//   착용 순서를 쓰지 않으므로 재접속 후에도 항상 같은 결과가 나온다.
	//   (equipment-skill-binding.md B5)

	unordered_map<uint32, Protocol::EquipSlot> seen;

	for (int32 i = 0; i < SKILL_SLOT_COUNT; i++)
	{
		SkillSlot& s = skillSlots[i];
		const Protocol::EquipSlot slotEnum = static_cast<Protocol::EquipSlot>(i + 1);

		if (s.skillId == 0)
		{
			s.bindState = Protocol::BIND_UNBOUND;
			s.shadowedBy = Protocol::SLOT_NONE;
			continue;
		}

		auto found = seen.find(s.skillId);
		if (found == seen.end())
		{
			s.bindState = Protocol::BIND_BOUND;
			s.shadowedBy = Protocol::SLOT_NONE;
			seen[s.skillId] = slotEnum;
		}
		else
		{
			// 이미 우선순위 높은 슬롯이 이 스킬을 쥐고 있다.
			s.bindState = Protocol::BIND_SHADOWED;
			s.shadowedBy = found->second;
		}
	}
}
```

## 왜

**배열 순서가 곧 우선순위입니다.** 별도 우선순위 테이블을 두지 않았습니다 —
`Protocol::EquipSlot` enum이 이미 `SLOT_WEAPON_PRIMARY = 1` … `SLOT_TRINKET = 6` 순서로
정의돼 있으므로, 배열을 그 순서로 두면 **for 루프 한 번이 곧 우선순위 순회**입니다.

> **`shadowedBy`를 채우는 이유**: 로드아웃 화면이 *"같은 스킬이 `Z`에 있음"* 을 표시합니다
> (`skill-system.md` § UI Requirements). 어느 슬롯 때문에 죽었는지 알려주지 않으면
> 플레이어가 원인을 못 찾습니다.

---

# STEP A-5 — `Room.cpp` 수정 (입장 시 동기화)

**파일** `Server/GameServer/Room.cpp`

## 현재 코드 (`EnterRoom` 안, 78~81행)

```cpp
		SendBufferRef sendBuffer = ServerPacketHandler::MakeSendBuffer(enterGamePkt);
		if (auto session = player->session.lock())
			session->Send(sendBuffer);
	}
```

## 바꿀 코드

```cpp
		SendBufferRef sendBuffer = ServerPacketHandler::MakeSendBuffer(enterGamePkt);
		if (auto session = player->session.lock())
			session->Send(sendBuffer);

		// 🔴 스킬 슬롯 동기화 — 소유자에게만 유니캐스트한다.
		//    Broadcast 를 쓰지 않는 이유: skill_id 는 본인만 알아야 한다
		//    (equipment-skill-binding.md B8). 다른 플레이어에게 나가면 마주치는 순간
		//    상대 로드아웃 6칸이 전부 노출된다.
		{
			// TODO(P2): 지금은 하드코딩. 아이템 시스템이 생기면 착용 장비에서 읽는다.
			player->skillSlots[0].skillId = 1001;	// SLOT_WEAPON_PRIMARY
			player->skillSlots[4].skillId = 2001;	// SLOT_BOOTS
			player->RefreshBindStates();

			Protocol::S_EQUIP_SYNC equipPkt;
			for (int32 i = 0; i < SKILL_SLOT_COUNT; i++)
			{
				const SkillSlot& s = player->skillSlots[i];

				Protocol::SkillInfo* info = equipPkt.add_skills();
				info->set_slot(static_cast<Protocol::EquipSlot>(i + 1));
				info->set_skill_id(s.skillId);
				info->set_bind_state(s.bindState);
				info->set_shadowed_by(s.shadowedBy);
			}

			SendBufferRef equipBuffer = ServerPacketHandler::MakeSendBuffer(equipPkt);
			if (auto session = player->session.lock())
				session->Send(equipBuffer);
		}
	}
```

## 왜

- **빈 슬롯도 함께 보냅니다.** 클라가 6칸 스킬 바를 항상 그려야 하므로
  (`skill-system.md` § UI Requirements "항상 6개를 표시한다") `skillId = 0`인 칸도
  `BIND_UNBOUND`로 내려보냅니다.
- **하드코딩에 `TODO(P2)`를 명시했습니다.** 아이템 인스턴스가 생기기 전까지는
  모든 플레이어가 같은 스킬 2개를 갖습니다 — P1.5 배관 검증에는 충분합니다.

---

# STEP A-6 — `GameServer.cpp` 에 테이블 초기화

**파일** `Server/GameServer/GameServer.cpp`

`main()` 안에서 **서버가 접속을 받기 전에** 한 번 호출합니다.

```cpp
	SkillTable::Init();
```

> 위치는 기존 초기화 코드(`GRoom` 생성 근처)와 같은 구간이면 됩니다. `#include "SkillTable.h"` 추가.

---

# STEP A-7 — `GameServer.vcxproj` 에 신규 파일 등록

🔴 **이걸 빼먹으면 링크 에러가 납니다.**

```xml
<!-- ClCompile 그룹에 -->
<ClCompile Include="SkillTable.cpp" />

<!-- ClInclude 그룹에 -->
<ClInclude Include="SkillTable.h" />
```

> Visual Studio에서 **솔루션 탐색기 → GameServer 우클릭 → 추가 → 기존 항목**으로 넣으면
> `.vcxproj`와 `.filters`가 함께 갱신됩니다. XML 직접 편집보다 안전합니다.

---

# 빌드 순서

```
1  ServerCore    빌드      ← 먼저. 안 하면 구버전 .lib 링크
2  GameServer    빌드
```

DummyClient·S1은 이번에 안 건드립니다 (`.proto` 변경 없음).

---

# 검증

## 1. 컴파일

```
GameServer 빌드 성공  →  SkillTable 링크 확인 (A-7 누락 시 여기서 실패)
```

## 2. 서버 콘솔 — 접속 1회

기존 `[SPAWN]` 로그 옆에 다음을 임시로 추가해 확인하시면 확실합니다.

```cpp
// EnterRoom 의 S_EQUIP_SYNC 블록 안, 패킷 전송 직전에
wcout << L"[EQUIP] id=" << player->objectInfo->object_id()
	<< L" slots=" << equipPkt.skills_size() << endl;
```

```
기대 출력   [EQUIP] id=1 slots=6
```

**6이 나와야 합니다.** 빈 슬롯 4개를 포함해 항상 6칸입니다.

## 3. `Shadowed` 판정 — 임시 확인

두 슬롯에 **같은 `skill_id`** 를 넣어보면 우선순위가 도는지 볼 수 있습니다.

```cpp
// 임시 — 확인 후 되돌릴 것
player->skillSlots[4].skillId = 1001;	// 신발에도 후려치기
player->skillSlots[0].skillId = 1001;	// 무기(Shift)
player->RefreshBindStates();
```

```
기대   slot 1 (무기)  BIND_BOUND
       slot 5 (신발)  BIND_SHADOWED · shadowedBy = SLOT_WEAPON_PRIMARY(1)
```

**무기가 살고 신발이 죽어야 합니다.** 반대로 나오면 배열 순서가 어긋난 것입니다.

## 4. 이 STEP에서 확인 못 하는 것

`C_SKILL` 핸들러가 아직 없으므로 **스킬 발동은 안 됩니다.** 이번 목표는
**"슬롯 데이터가 서버에 존재하고 클라에 도달한다"** 까지입니다.

---

# 다음 단계

```
STEP B  Handle_C_SKILL — 검증 6종 · 캐스트 타이머 · FlushCombat 배치 편입
STEP C  Handle_C_SKILL_CANCEL
STEP D  DummyClient C_SKILL 발신  →  SK-43/44 성능 측정
STEP E  UE 클라 — 스킬 바 · 캐스트 게이지 · effect_id 연출
```

**STEP B가 이 시리즈에서 가장 큽니다** — 상태기(`Casting`) · 쿨다운 · 판정 · 배치 편입이
한꺼번에 들어갑니다. A가 통과하면 B를 두 조각으로 나눠 드리겠습니다.
