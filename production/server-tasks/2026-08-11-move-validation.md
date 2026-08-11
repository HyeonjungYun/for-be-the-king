# 서버 작업 제안 — 이동 검증 (공식 7)

**작성**: 2026-08-11 · **소유**: 🔴 사용자 · **P0 항목**: 서버 이동 검증
**설계 근거**: `design/gdd/movement-camera.md` § Formulas 7 (7a · 7b · 7c)

> 이 문서의 코드는 **2026-08-11 시점의 실제 파일을 읽고** 작성했습니다.
> 이미 고치셨다면 해당 항목은 건너뛰세요.

---

## 요약 — 구멍이 두 개입니다

`Room::HandleMove`는 클라가 보낸 위치를 **아무 검증 없이 그대로 믿습니다.**

```cpp
// Room.cpp:130 — 현재
player->posInfo->CopyFrom(pkt.info());
```

| # | 구멍 | 심각도 | 이 문서에서 |
|---|---|---|---|
| **1** | **object_id 위조** — 남의 캐릭터를 움직일 수 있다 | 🔴 **더 심각** | 변경 4 |
| **2** | 속도 무검증 — 순간이동·스피드핵 | 🔴 높음 | 변경 2·3 |

**구멍 1이 더 위험합니다.** `Handle_C_MOVE`는 세션에서 진짜 player를 꺼내놓고도, `Room::HandleMove`에 **패킷 안의 `object_id`를 그대로** 넘깁니다. 클라 A가 B의 id를 실어 보내면 **B가 끌려다닙니다.** 고치는 데는 한 줄이면 됩니다.

## 🔴 그리고 클라를 같이 고쳐야 검증이 작동합니다

```cpp
// S1GameInstance.cpp:177 — 현재
if (Player->IsMyPlayer())
    return;               // ← 자기 자신의 S_MOVE 를 버린다
```

**클라가 자기 위치 보정을 통째로 무시합니다.** 서버가 스냅백을 보내도 도달하지 않습니다. 변경 5를 함께 적용하지 않으면 서버만 고쳐봐야 **검증 실패 → 조용히 디싱크** 로 끝납니다.

---

## 적용 순서

**서버 4개를 전부 적용한 뒤 한 번 빌드**합니다.

```
1  GameServer/Utils.h                  NowMicroseconds() 추가
2  GameServer/Player.h                 검증 상태 + 속도 계산 선언
   GameServer/Player.cpp               속도 계산 구현
3  GameServer/Room.cpp                 HandleMove 검증 본체
4  GameServer/ServerPacketHandler.cpp  object_id 위조 차단
─────────────────────────────────────────────────────────
5  S1/Source/S1/S1GameInstance.cpp     스냅백 수신  🔴 사용자
6  S1/Source/S1/Game/S1Player.h        30Hz 전환    (에이전트 영역 — 지시하시면 제가 적용)
```

> ✅ **ServerCore 변경 없음.** 4개 전부 GameServer 프로젝트 안이라 **ServerCore 선행 빌드가 필요 없습니다.**
> (`GameServer.vcxproj`에 ServerCore 참조가 없어 구버전 lib가 링크되는 문제는 이번엔 해당 없음)

---

# 변경 1 — `Server/GameServer/Utils.h`

### 현재 코드 (21~28행)

```cpp
		else
		{
			std::uniform_real_distribution<T> distribution(min, max);
			return distribution(generator);
		}
	}
};

```

### 바꿀 코드

```cpp
		else
		{
			std::uniform_real_distribution<T> distribution(min, max);
			return distribution(generator);
		}
	}

	// 서버 단조 시계. 이동 검증의 Δ시간 측정에 쓴다.
	//
	// GetTickCount64() 를 쓰지 않는 이유: 기본 해상도가 약 15.6ms 라서
	// 33ms 간격을 15.6 / 31.2 / 46.8 로 읽는다. 오차 ±47% 는 검증 여유율
	// 1.15 를 훌쩍 넘어 정상 플레이어가 스냅백당한다.
	// steady_clock 은 단조 증가라 시스템 시간 변경(NTP 동기화 등)에도 안전하다.
	static uint64 NowMicroseconds()
	{
		return std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
	}
};

```

**왜**: 공식 7a가 요구하는 `Δ시간`은 **서버 수신 타임스탬프 실측차**입니다. 1/30 고정 가정은 금지입니다 — 지연 변동이 그대로 오차가 되기 때문입니다.

**검증**: 컴파일만 통과하면 됩니다. `<chrono>`는 `CorePch.h:17`에 이미 있습니다.

---

# 변경 2 — `Server/GameServer/Player.h` · `Player.cpp`

## 2-1. `Player.h` — 전체 교체

### 현재 코드 (전체)

```cpp
#pragma once
#include "Creature.h"

class GameSession;
class Room;

class Player : public Creature
{
public:
	Player();
	virtual ~Player();

public:
	weak_ptr<GameSession> session;
};
```

### 바꿀 코드 (전체)

```cpp
#pragma once
#include "Creature.h"

class GameSession;
class Room;

class Player : public Creature
{
public:
	Player();
	virtual ~Player();

public:
	// design/gdd/movement-camera.md 공식 1 — 장비·감속을 반영한 최종 이동속도
	float GetEffectiveMoveSpeed() const;

	// 공식 7b — 승인된 대시·넉백 중이면 그 상한을, 아니면 통상 상한을 돌려준다
	float GetSpeedCeiling(uint64 nowUs) const;

public:
	weak_ptr<GameSession> session;

public:
	// ─── 이동 검증 상태 (공식 7) ───────────────────────────────
	// 모두 Room 의 JobQueue 안에서만 접근한다. 별도 락 불필요.

	// 마지막으로 검증을 통과한 이동 패킷의 서버 수신 시각. 0 = 아직 없음
	uint64 lastMoveUs = 0;

	// 소프트 CC 감속. 0.0 ~ 0.40 (entities.yaml: soft_cc_slow_cap)
	// TODO(전투 시스템): CC 적용/해제 시 여기를 갱신한다. 클라 보고값을 믿지 않는다.
	float activeSlow = 0.f;

	// 공식 7b — 대시·넉백처럼 서버가 파라미터를 알고 승인한 이동의 예외 창
	// TODO(스킬 시스템): 대시 캐스트를 승인할 때 두 값을 채운다.
	float  moveExceptionSpeed = 0.f;   // 이번 승인의 실제 최대 속도
	uint64 moveExceptionEndUs = 0;     // 만료 시각(us). 0 = 비활성
};
```

## 2-2. `Player.cpp` — 전체 교체

### 현재 코드 (전체)

```cpp
#include "pch.h"
#include "Player.h"

Player::Player()
{
	_isPlayer = true;
}

Player::~Player()
{
}
```

### 바꿀 코드 (전체)

```cpp
#include "pch.h"
#include "Player.h"
#include <algorithm>

namespace
{
	// design/registry/entities.yaml — 값을 바꾸려면 레지스트리를 먼저 고칠 것
	constexpr float BASE_MOVE_SPEED = 340.f;   // base_move_speed
	constexpr float MS_MIN_RATIO    = 0.5f;    // ms_min_ratio  → 하한 170
	constexpr float MS_MAX_RATIO    = 1.5f;    // ms_max_ratio  → 상한 510
}

Player::Player()
{
	_isPlayer = true;
}

Player::~Player()
{
}

float Player::GetEffectiveMoveSpeed() const
{
	// TODO(장비 시스템): 장착 아이템에서 합산한다.
	//   flatBonus     0 ~ 85     (flat_ms_bonus)
	//   percentBonus  0 ~ 0.75   (아이템당 0 ~ 0.15, 가산 스택)
	//
	// 가산 후 1회 곱이다. 곱연산을 순차로 하면 아이템 5개 × +20% 가
	// 1.2^5 ≈ 2.49배로 폭주해 무한 카이팅이 된다.
	const float flatBonus = 0.f;
	const float percentBonus = 0.f;

	// activeSlow 는 반드시 별도 곱셈항이다.
	// (1 + percent - slow) 로 섞으면 속도 장비를 낀 플레이어가 감속에 면역이 된다:
	//   맨몸    340 × 0.6                 = 204
	//   속도템  340 × (1 + 0.5 - 0.4)     = 374   ← 감속 전 base 보다 빠르다
	const float speed =
		(BASE_MOVE_SPEED + flatBonus) * (1.f + percentBonus) * (1.f - activeSlow);

	return std::clamp(speed, BASE_MOVE_SPEED * MS_MIN_RATIO, BASE_MOVE_SPEED * MS_MAX_RATIO);
}

float Player::GetSpeedCeiling(uint64 nowUs) const
{
	// 공식 7b — 승인된 대시·넉백 중에는 감속을 무시하고 예외 상한을 쓴다.
	// 감속 상태에서 대시가 느려져야 하는지는 대시를 '승인하는 시점'에
	// 전투 시스템이 정할 문제다. 검증 레이어에서 이중으로 깎지 않는다.
	if (moveExceptionEndUs != 0 && nowUs < moveExceptionEndUs)
		return moveExceptionSpeed;

	return GetEffectiveMoveSpeed();
}
```

**왜**: 공식 7a의 상한은 **전역 상수 340이 아니라 플레이어별 실효 속도**여야 합니다. 상수를 쓰면 이속 장비를 낀 플레이어(최대 510)가 **정상 이동만으로 강제 스냅백**당합니다. 지금은 장비 시스템이 없어 결과가 340이지만, **구조를 미리 맞춰두면 장비 도입 시 `flatBonus` 두 줄만 바뀝니다.**

**검증**: 컴파일. 값 확인은 변경 3의 로그로 함께 합니다.

---

# 변경 3 — `Server/GameServer/Room.cpp`

## 3-1. 파일 상단 (1~5행)

### 현재 코드

```cpp
#include "pch.h"
#include "Room.h"
#include "Player.h"

RoomRef GRoom = make_shared<Room>();
```

### 바꿀 코드

```cpp
#include "pch.h"
#include "Room.h"
#include "Player.h"
#include <cmath>

namespace
{
	// design/gdd/movement-camera.md 공식 7a
	constexpr double VALIDATION_MARGIN = 1.15;    // validation_margin
	constexpr double MIN_DELTA_SEC     = 0.0166;  // min_delta_t = 0.5 × 33ms
}

RoomRef GRoom = make_shared<Room>();
```

## 3-2. `HandleMove` (122~143행) — 전체 교체

### 현재 코드

```cpp
void Room::HandleMove(Protocol::C_MOVE pkt)
{
	const uint64 objectId = pkt.info().object_id();
	if (_objects.find(objectId) == _objects.end())
		return;

	// ����
	PlayerRef player = dynamic_pointer_cast<Player>(_objects[objectId]);
	player->posInfo->CopyFrom(pkt.info());

	// �̵�
	{
		Protocol::S_MOVE movePkt;
		{
			Protocol::PosInfo* info = movePkt.mutable_info();
			info->CopyFrom(pkt.info());
		}

		SendBufferRef sendBuffer = ServerPacketHandler::MakeSendBuffer(movePkt);
		Broadcast(sendBuffer, objectId);
	}
}
```

### 바꿀 코드

```cpp
void Room::HandleMove(Protocol::C_MOVE pkt)
{
	const uint64 objectId = pkt.info().object_id();

	auto findIt = _objects.find(objectId);
	if (findIt == _objects.end())
		return;

	PlayerRef player = dynamic_pointer_cast<Player>(findIt->second);
	if (player == nullptr)
		return;

	// ─── 공식 7 — 서버 이동 검증 ──────────────────────────────
	const uint64 nowUs = Utils::NowMicroseconds();

	if (player->lastMoveUs == 0)
	{
		// 입장 후 첫 패킷. 비교할 이전 시각이 없으므로 기준만 세우고 통과시킨다.
		player->lastMoveUs = nowUs;
	}
	else
	{
		const double deltaSec =
			static_cast<double>(nowUs - player->lastMoveUs) / 1000000.0;

		// 7a — min_delta_t 미만은 통째로 버린다. 0 나눗셈과 패킷 플러딩을 함께 막는다.
		//
		// 지터로 정상 패킷 두 개가 붙어 와도 문제없다. 뒤엣것을 버리면 그 다음 패킷의
		// Δ시간과 Δ거리가 함께 두 배가 되어 속도 계산이 그대로 성립한다. 자기 보정된다.
		if (deltaSec < MIN_DELTA_SEC)
			return;

		// XY 평면 거리만 본다. 점프가 없는 지상 전용 게임이고, 경사면에서는
		// 3D 거리가 XY 거리보다 항상 크다. XY 기준은 과소평가 쪽이라 오탐이 없다.
		const double dx = static_cast<double>(pkt.info().x()) - player->posInfo->x();
		const double dy = static_cast<double>(pkt.info().y()) - player->posInfo->y();
		const double distance = std::sqrt(dx * dx + dy * dy);
		const double speed = distance / deltaSec;

		const double ceiling = player->GetSpeedCeiling(nowUs) * VALIDATION_MARGIN;

		// 실패해도 시각은 갱신한다. 갱신하지 않으면 Δ시간이 계속 늘어나
		// 상한이 점점 관대해진다.
		player->lastMoveUs = nowUs;

		if (speed > ceiling)
		{
			// 검증 실패 — posInfo 를 갱신하지 않고 마지막 유효 위치를 되돌려 보낸다.
			// 브로드캐스트도 하지 않는다. 다른 플레이어는 잘못된 위치를 본 적이 없다.
			Protocol::S_MOVE snapbackPkt;
			snapbackPkt.mutable_info()->CopyFrom(*player->posInfo);

			SendBufferRef sendBuffer = ServerPacketHandler::MakeSendBuffer(snapbackPkt);
			if (auto session = player->session.lock())
				session->Send(sendBuffer);

			// 튜닝·디버깅용. 안정화되면 지우거나 로그 레벨로 내릴 것.
			cout << "[MOVE REJECT] id=" << objectId
				 << " speed=" << speed
				 << " ceiling=" << ceiling
				 << " dt=" << deltaSec << endl;

			return;
		}
	}

	// ─── 검증 통과 ────────────────────────────────────────────
	player->posInfo->CopyFrom(pkt.info());

	{
		Protocol::S_MOVE movePkt;
		movePkt.mutable_info()->CopyFrom(pkt.info());

		SendBufferRef sendBuffer = ServerPacketHandler::MakeSendBuffer(movePkt);
		Broadcast(sendBuffer, objectId);
	}
}
```

**왜**

- 원본은 `dynamic_pointer_cast` 결과를 **null 체크 없이 역참조**합니다. 같은 `_objects`에 몬스터가 들어오면 크래시입니다. 지금은 몬스터가 없지만 P3에서 들어옵니다.
- `_objects.find()` 후 `_objects[objectId]`로 **두 번 조회**하던 것을 `findIt->second`로 합쳤습니다.
- 검증 실패 시 **브로드캐스트하지 않는 것이 핵심**입니다. 다른 플레이어에게 잘못된 위치가 이미 전파된 뒤 되돌리면 순간이동이 눈에 보입니다.

**검증**

| # | 절차 | 기대 |
|---|---|---|
| 1 | 클라 2개로 정상 이동 | `[MOVE REJECT]` 로그가 **한 줄도** 안 뜬다 |
| 2 | `S1Player.cpp:35` `MaxWalkSpeed = 340.f` → `2000.f` 로 바꿔 빌드 후 이동 | `[MOVE REJECT]` 가 계속 뜨고 **캐릭터가 제자리로 튕긴다** (변경 5 적용 후) |
| 3 | 다른 클라에서 2번 캐릭터 관찰 | **움직이지 않는다** — 브로드캐스트가 막혔으므로 |
| 4 | 2번을 340으로 되돌림 | 다시 정상 |

> 테스트 2를 위해 `MaxWalkSpeed`를 임시로 올리는 건 제가 대신 해드릴 수 있습니다 (`S1Player.cpp`는 에이전트 영역).

---

# 변경 4 — `Server/GameServer/ServerPacketHandler.cpp`

### 현재 코드 (71~88행)

```cpp
bool Handle_C_MOVE(PacketSessionRef& session, Protocol::C_MOVE& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);

	PlayerRef player = gameSession->player.load();
	if (player == nullptr)
		return false;

	RoomRef room = player->room.load().lock();
	if (room == nullptr)
		return false;

	// TODO : ������ ��Ŷ�� ��¥ �÷��̾� ���ο��Լ� �� ������ Validation üũ

	GRoom->DoAsync(&Room::HandleMove, pkt);

	return true;
}
```

### 바꿀 코드

```cpp
bool Handle_C_MOVE(PacketSessionRef& session, Protocol::C_MOVE& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);

	PlayerRef player = gameSession->player.load();
	if (player == nullptr)
		return false;

	RoomRef room = player->room.load().lock();
	if (room == nullptr)
		return false;

	// object_id 는 클라가 채워 보내는 값이라 위조가 가능하다.
	// 세션이 실제로 소유한 플레이어의 id 로 덮어써서 남의 캐릭터를 움직이는 것을 막는다.
	pkt.mutable_info()->set_object_id(player->objectInfo->object_id());

	// GRoom 전역이 아니라 이 플레이어가 실제로 속한 방에 넣는다.
	// 지금은 방이 하나뿐이라 결과가 같지만, 층이 4개가 되면 갈린다.
	room->DoAsync(&Room::HandleMove, pkt);

	return true;
}
```

**왜**: 이게 **속도 검증보다 심각한 구멍**이었습니다. `Room::HandleMove`가 쓰는 `objectId`는 전부 패킷에서 옵니다. 클라 A가 B의 `object_id`로 `C_MOVE`를 보내면 **서버가 B를 그 자리로 옮기고 전원에게 방송**합니다. 남을 절벽 밖으로 끌어낼 수 있습니다. `Handle_C_MOVE`는 이미 세션에서 진짜 player를 꺼내놓고도 쓰지 않고 있었습니다.

`GRoom` → `room` 교체는 별건입니다. **층 4개(`floor_count`)가 들어오면 전역 방 참조는 전부 버그**가 됩니다. 지금 고쳐두는 편이 쌉니다.

**검증**: `DummyClient`에서 `C_MOVE`의 `object_id`를 다른 플레이어 것으로 채워 보냅니다. **대상이 움직이지 않으면 통과**입니다. (수정 전에는 움직입니다 — 먼저 재현해두면 확실합니다)

---

# 변경 5 — `S1/Source/S1/S1GameInstance.cpp` 🔴 사용자

### 현재 코드 (171~184행)

```cpp
	const uint64 ObjectId = MovePkt.info().object_id();
	AS1Player** FindActor = Players.Find(ObjectId);
	if (FindActor == nullptr)
		return;

	AS1Player* Player = (*FindActor);
	if (Player->IsMyPlayer())
		return;

	const Protocol::PosInfo& Info = MovePkt.info();
	
	//Player->SetPlayerInfo(Info);
	Player->SetDestInfo(Info);
```

### 바꿀 코드

```cpp
	const uint64 ObjectId = MovePkt.info().object_id();
	AS1Player** FindActor = Players.Find(ObjectId);
	if (FindActor == nullptr)
		return;

	AS1Player* Player = (*FindActor);
	const Protocol::PosInfo& Info = MovePkt.info();

	// 내 캐릭터에게 오는 S_MOVE 는 서버 이동 검증의 스냅백이다.
	// 평상시 서버는 내 위치를 되돌려 보내지 않으므로, 이게 왔다는 것 자체가
	// "네 위치를 인정하지 않는다"는 뜻이다.
	// 원격 프록시용 보간(SetDestInfo)이 아니라 즉시 강제해야 한다.
	if (Player->IsMyPlayer())
	{
		Player->SetPlayerInfo(Info);
		return;
	}

	Player->SetDestInfo(Info);
```

**왜**: 지금은 `IsMyPlayer()`면 **그냥 return** 합니다. 서버가 스냅백을 보내도 클라가 버리므로 **검증이 무의미**해집니다. 더 나쁜 건, 서버는 위치 A를 갖고 클라는 위치 B를 그리는 **영구 디싱크**가 된다는 점입니다 — 이후 모든 패킷이 검증에 실패하고, 그 플레이어는 남들 눈에 얼어붙습니다.

`SetPlayerInfo`가 맞는 함수입니다. 안에서 `SetActorLocation(Location)`으로 **즉시 이동**합니다(`S1Player.cpp:167`). `SetDestInfo`는 보간 경로라 스냅백에 부적합합니다.

**검증**: 변경 3의 테스트 2와 동일. `MaxWalkSpeed`를 2000으로 올리고 이동하면 **캐릭터가 계속 뒤로 튕겨야** 합니다.

**빌드 순서**: 서버와 무관합니다. 언리얼 쪽만 빌드하면 됩니다.

---

# 변경 6 — `S1/Source/S1/Game/S1Player.h` (에이전트 영역)

### 현재 코드 (48행 부근)

```cpp
	const float MOVE_PACKET_SEND_DELAY = 0.2f;
```

### 바꿀 코드

```cpp
	// 30Hz. design/gdd/movement-camera.md § Core Rule 10 · entities.yaml move_packet_send_rate
	// 서버 검증 여유율 1.15 와 원격 보간 66ms 가 이 값을 전제로 계산돼 있다.
	const float MOVE_PACKET_SEND_DELAY = 1.f / 30.f;
```

**왜**: 문서는 30Hz로 확정돼 있는데 **코드는 5Hz**입니다. 검증의 `min_delta_t = 16.6ms`는 33ms 간격의 절반이라는 뜻이므로, 5Hz(200ms)에서는 아무 의미가 없습니다. 원격 프록시 보간(`InterpDuration`)도 `MOVE_PACKET_SEND_DELAY`를 그대로 쓰므로 함께 바뀝니다.

**부하 영향** (`technical-preferences.md` § 서버 예산): 초당 `Send()` 호출이 층당 **26,100회**. 단일 스레드로 충분하다고 계산돼 있습니다. 클라 1인 수신 **278 kbps** · 서버 업로드(30명) **8.4 Mbps**.

> ⚠️ 이 값은 위치 패킷만 센 것입니다. 전투 패킷이 들어오면 다시 재야 합니다 (SV-5).

**적용**: `S1Player.h`는 에이전트 영역이라 **말씀만 하시면 제가 바로 적용**합니다. 다만 서버 부하가 6배가 되므로 서버 변경과 **같은 타이밍에 올리는 편**이 원인 추적에 유리합니다.

---

## 이 변경이 막지 못하는 것 — 정직하게

| 공격 | 막히나 | 왜 |
|---|---|---|
| 스피드핵 (지속적 과속) | ✅ | 공식 7a |
| 순간이동 (한 방에 멀리) | ✅ | 공식 7a |
| 남의 캐릭터 조종 | ✅ | 변경 4 |
| **벽 통과** | ❌ | 거리만 보고 **경로를 안 본다.** 충돌 검증은 별도 기능 |
| **느린 순간이동** | ❌ | 5초 기다렸다 1000cm 이동 = 200cm/s → 통과. 다만 **걸어가는 것과 같아** 이득이 없다 |
| 위치 미세 조작 (에임핵 보정) | ❌ | 여유율 15% 안쪽은 통과. 의도된 것 |

**벽 통과가 남는 유일한 실질 구멍입니다.** 서버가 레벨 지오메트리를 모르기 때문이고, 그걸 알게 하려면 서버에 충돌 데이터를 넣어야 합니다 — 별도 작업이며 P0 범위가 아닙니다. **레벨 작업(P3) 이후에 다시 판단**하는 게 맞습니다.

---

## 남은 P0 (이 문서 범위 밖)

| 항목 | 소유 |
|---|---|
| GoogleTest 도입 + 동시성 스트레스 3종 | 🔴 사용자 |
| 계측 훅 3종 (SV-1 JobQueue Flush P95) | 🔴 사용자 |
| DummyClient 봇 | 🔴 사용자 |
| 유령 플레이어 | 🔴 사용자 |
| `S1Player.cpp:171` assert 오류 | 에이전트 (지시 시 적용) |
