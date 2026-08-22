# 서버 작업 제안 — DummyClient 봇 전투

**작성**: 2026-08-18 · **소유**: 🔴 사용자 · **Phase**: P1 (전투)
**푸는 것**: **게이트 G1**(전투 패킷 대역폭 증가분) + **P1 완료 기준 2건**

> 이 문서의 코드는 **2026-08-18 시점의 `BotSession` · `TickBots` · `Service` 를 읽고** 작성했습니다.

---

## 왜 이 작업 하나로 둘이 풀리나

P1 완료 기준을 다시 보면 **사람 손이 꼭 필요한 항목이 없다.**

| P1 완료 기준 | 실제로 필요한 것 | 상태 |
| ---- | ---- | ---- |
| 3인이 서로 죽이는 루프 10분 | **안정성 검증.** 클라 3개면 된다 | 🤖 봇 |
| 3인 이상 동시 공격 시 TTK 감소 | **실측.** 봇 3기가 1기를 때리면 된다 | 🤖 봇 |
| 4인 체인 CC ≤ 5초 | 디버그 커맨드 | ✅ 완료 |
| 감속 4개 = 최대값 1개 | 디버그 커맨드 | ✅ 완료 |
| 감속 스냅백 오탐 0 | 실측 | ✅ 완료 |
| 장비 교체 시 공격력 변경 | 장비가 없다 | ⬜ P2 |

**"3인"은 사람 3명이 아니라 클라이언트 3개다.** 봇이 전투를 하면 전부 자동으로 돈다.

그리고 같은 봇으로 **G1 대역폭**(전투 패킷이 붙은 30인 수치)을 잰다.
현재 기준선은 이동만 있을 때의 값이다 — `sent 3.20 Mbps · p95 ≤100us · 메모리 91.7MB`.

---

## 설계

```
1  가장 가까운 살아있는 다른 봇을 고른다
2  사거리(150cm) 밖이면 추격
3  안에 들어오면 1초마다 C_ATTACK
4  죽으면(S_DIED 수신) 접속을 끊고, main 이 새 세션을 채운다
```

### 봇이 다른 봇의 위치를 어떻게 아는가

**`GBots` 를 직접 읽는다.** 모든 봇이 한 프로세스에 있으므로 서로의 `x/y` 에 그냥 접근할 수 있다.

> ⚠️ **진짜 클라이언트는 이렇게 못 한다.** `S_MOVE` 를 받아 남의 위치를 추적해야 한다.
> 하지만 `DummyClient` 는 게임 클라이언트가 아니라 **부하 생성기**이므로 이 지름길이 맞다.
> 목적은 "서버에 전투 부하를 거는 것"이지 "클라이언트를 흉내내는 것"이 아니다.

### 왜 재접속이 필요한가

`combat-system.md` 에 따라 **리스폰이 없다.** `Dead` 는 종결 상태다.
죽은 봇은 서버에서 이동도 공격도 거부되므로 **그 자리에 시체로 남는다.**

```
재접속이 없으면   50초쯤 뒤 전원 사망 → 부하가 0 이 된다 → G1 측정 불가
재접속이 있으면   죽고 살아나기를 반복 → "10분 루프" 가 성립한다
```

`ClientService::Start()` 는 세션을 **한 번만** 만들고 재접속 기능이 없다.
`Service::CreateSession()` 이 public 이므로 main 루프에서 직접 채운다.

---

# STEP 1 — `BotSession.h`

**현재 코드**
```cpp
public:
	atomic<bool> inGame = false;
	atomic<uint64> objectId = 0;

	float x = 0.f;
	float y = 0.f;
	float z = 100.f;
	float destX = 0.f;
	float destY = 0.f;
};
```

**바꿀 코드**
```cpp
public:
	atomic<bool> inGame = false;
	atomic<uint64> objectId = 0;

	// 서버가 사망을 알려주면 내려간다. 수신 스레드가 쓰고 TickBots 가 읽으므로 atomic.
	atomic<bool> alive = true;

	float x = 0.f;
	float y = 0.f;
	float z = 100.f;
	float destX = 0.f;
	float destY = 0.f;

	// TickBots 단일 스레드에서만 접근하므로 atomic 이 필요 없다.
	uint64 nextAttackAtUs = 0;
};
```

---

# STEP 2 — `BotSession.cpp` 의 `OnDisconnected`

**현재 코드**
```cpp
void BotSession::OnDisconnected()
{
	inGame.store(false);
	cout << "[BOT] disconnected id=" << objectId.load() << endl;
}
```

**바꿀 코드**
```cpp
void BotSession::OnDisconnected()
{
	inGame.store(false);
	alive.store(false);

	// GBots 에서 빼야 main 이 빈 자리를 알아채고 새 세션을 채운다.
	{
		lock_guard<mutex> guard(GBotsLock);

		BotSessionRef self = static_pointer_cast<BotSession>(GetSessionRef());
		GBots.erase(std::remove(GBots.begin(), GBots.end(), self), GBots.end());
	}

	cout << "[BOT] disconnected id=" << objectId.load() << endl;
}
```

> `<algorithm>` 이 필요하다. `CorePch.h` 에 없으면 파일 상단에 추가할 것.

---

# STEP 3 — `ClientPacketHandler.cpp` 의 `Handle_S_DIED`

**현재 코드**
```cpp
bool Handle_S_DIED(PacketSessionRef& session, Protocol::S_DIED& pkt)
{
	return true;
}
```

**바꿀 코드**
```cpp
bool Handle_S_DIED(PacketSessionRef& session, Protocol::S_DIED& pkt)
{
	auto bot = static_pointer_cast<BotSession>(session);

	for (const Protocol::DiedInfo& info : pkt.deaths())
	{
		if (info.victim_id() != bot->objectId.load())
			continue;

		bot->alive.store(false);

		cout << "[BOT DIED] id=" << bot->objectId.load()
			<< " by=" << info.instigator_id() << endl;

		// 리스폰이 없으므로(combat-system.md: Dead 는 종결 상태) 재접속이 유일한 복귀 수단이다.
		bot->Disconnect(L"Died");
	}

	return true;
}
```

---

# STEP 4 — `DummyClient.cpp` 상수

**현재 코드**
```cpp
namespace
{
	constexpr float BOT_MOVE_SPEED = 340.f; 
	constexpr float WANDER_RADIUS = 1000.f; 
	constexpr float ARRIVE_EPSILON = 20.f;  
	constexpr int   MOVE_INTERVAL_MS = 33;
}
```

**바꿀 코드**
```cpp
namespace
{
	constexpr float BOT_MOVE_SPEED = 340.f; 
	constexpr float WANDER_RADIUS = 1000.f; 
	constexpr float ARRIVE_EPSILON = 20.f;  
	constexpr int   MOVE_INTERVAL_MS = 33;

	// entities.yaml: attack_range_naked. 서버 판정과 같은 값이어야 헛스윙이 안 난다.
	constexpr float ATTACK_RANGE = 150.f;
	constexpr float ATTACK_RANGE_SQ = ATTACK_RANGE * ATTACK_RANGE;

	// 사거리보다 안쪽에서 멈춘다. 딱 150 에서 멈추면 서버 쪽 계산 오차로 자꾸 빗나간다.
	constexpr float BOT_STOP_DISTANCE = 120.f;

	// 공속 1.0 = 1초에 1회. 더 자주 보내도 서버가 버리므로 낭비다.
	constexpr uint64 ATTACK_INTERVAL_US = 1000000;

	// 재접속 검사 주기. 매 틱 확인하면 Connect 가 비동기라 응답 전에 중복 생성된다.
	constexpr uint64 REFILL_INTERVAL_US = 1000000;
}

std::atomic<uint64> GAttackSentThisWindow{ 0 };
```

---

# STEP 5 — `DummyClient.cpp` 의 `TickBots` 전체 교체

```cpp
static void TickBots(float deltaTime)
{
	vector<BotSessionRef> snapshot;
	{
		lock_guard<mutex> guard(GBotsLock);
		snapshot = GBots;
	}

	const uint64 nowUs = Utils::NowMicroseconds();

	for (BotSessionRef& bot : snapshot)
	{
		if (bot->inGame.load() == false || bot->alive.load() == false)
			continue;

		// ── 대상 선정 — 가장 가까운 살아있는 다른 봇
		//    같은 프로세스라 남의 좌표를 그냥 읽는다. 부하 생성기이므로 허용되는 지름길이다.
		BotSessionRef target = nullptr;
		float bestDistSq = FLT_MAX;

		for (BotSessionRef& other : snapshot)
		{
			if (other == bot)
				continue;
			if (other->inGame.load() == false || other->alive.load() == false)
				continue;

			const float ox = other->x - bot->x;
			const float oy = other->y - bot->y;
			const float distSq = ox * ox + oy * oy;

			if (distSq < bestDistSq)
			{
				bestDistSq = distSq;
				target = other;
			}
		}

		// ── 이동 — 대상이 있으면 추격, 없으면 기존처럼 배회
		if (target != nullptr)
		{
			bot->destX = target->x;
			bot->destY = target->y;
		}

		const float dx = bot->destX - bot->x;
		const float dy = bot->destY - bot->y;
		const float dist = ::sqrtf(dx * dx + dy * dy);

		if (target == nullptr)
		{
			if (dist < ARRIVE_EPSILON)
			{
				bot->destX = bot->x + Utils::GetRandom(-WANDER_RADIUS, WANDER_RADIUS);
				bot->destY = bot->y + Utils::GetRandom(-WANDER_RADIUS, WANDER_RADIUS);
			}
			else
			{
				const float step = BOT_MOVE_SPEED * deltaTime;
				const float ratio = (step < dist) ? (step / dist) : 1.f;

				bot->x += dx * ratio;
				bot->y += dy * ratio;
			}
		}
		else if (dist > BOT_STOP_DISTANCE)
		{
			const float step = BOT_MOVE_SPEED * deltaTime;
			const float ratio = (step < dist) ? (step / dist) : 1.f;

			bot->x += dx * ratio;
			bot->y += dy * ratio;
		}

		// ── 이동 패킷 — 제자리에 있어도 계속 보낸다.
		//    서버는 매번 posInfo 를 갱신하고 dirty 로 표시하므로 브로드캐스트 부하가 유지된다.
		{
			Protocol::C_MOVE pkt;

			Protocol::PosInfo* info = pkt.mutable_info();
			info->set_object_id(bot->objectId.load());
			info->set_x(bot->x);
			info->set_y(bot->y);
			info->set_z(bot->z);
			info->set_yaw(0.f);
			info->set_state(Protocol::MOVE_STATE_RUN);

			bot->Send(ClientPacketHandler::MakeSendBuffer(pkt));
			++GSentThisWindow;
		}

		// ── 공격
		if (target != nullptr && bestDistSq <= ATTACK_RANGE_SQ && nowUs >= bot->nextAttackAtUs)
		{
			Protocol::C_ATTACK attackPkt;
			attackPkt.set_target_id(target->objectId.load());

			bot->Send(ClientPacketHandler::MakeSendBuffer(attackPkt));

			bot->nextAttackAtUs = nowUs + ATTACK_INTERVAL_US;
			++GAttackSentThisWindow;
		}
	}
}
```

---

# STEP 6 — `DummyClient.cpp` 의 `main` 루프

**현재 코드**
```cpp
	uint64 lastUs = Utils::NowMicroseconds();
	uint64 lastReportUs = lastUs;

	while (true)
	{
		this_thread::sleep_for(chrono::milliseconds(MOVE_INTERVAL_MS));

		const uint64 nowUs = Utils::NowMicroseconds();
		const float deltaTime = static_cast<float>(nowUs - lastUs) / 1000000.f;
		lastUs = nowUs;

		TickBots(deltaTime);

		if (nowUs - lastReportUs >= 10000000)
		{
			const double sec = static_cast<double>(nowUs - lastReportUs) / 1000000.0;
			const uint64 sent = GSentThisWindow.exchange(0);
			...
```

**바꿀 코드** — 재접속 블록 추가 + 리포트에 공격 수 추가
```cpp
	uint64 lastUs = Utils::NowMicroseconds();
	uint64 lastReportUs = lastUs;
	uint64 lastRefillUs = lastUs;

	while (true)
	{
		this_thread::sleep_for(chrono::milliseconds(MOVE_INTERVAL_MS));

		const uint64 nowUs = Utils::NowMicroseconds();
		const float deltaTime = static_cast<float>(nowUs - lastUs) / 1000000.f;
		lastUs = nowUs;

		TickBots(deltaTime);

		// 죽어서 빠진 자리를 다시 채운다. 리스폰이 없으므로 이게 '10분 루프' 를 성립시킨다.
		if (nowUs - lastRefillUs >= REFILL_INTERVAL_US)
		{
			lastRefillUs = nowUs;

			size_t liveCount = 0;
			{
				lock_guard<mutex> guard(GBotsLock);
				liveCount = GBots.size();
			}

			for (size_t i = liveCount; i < static_cast<size_t>(botCount); i++)
			{
				SessionRef session = service->CreateSession();
				session->Connect();
			}
		}

		if (nowUs - lastReportUs >= 10000000)
		{
			const double sec = static_cast<double>(nowUs - lastReportUs) / 1000000.0;
			const uint64 sent = GSentThisWindow.exchange(0);
			const uint64 attacks = GAttackSentThisWindow.exchange(0);
			...
```

리포트 출력에 한 줄 추가
```cpp
			cout << "[BOT] send rate = "
				<< (botCount > 0 ? sent / sec / botCount : 0.0)
				<< " Hz/bot  (target 30)"
				<< "   attacks = " << (attacks / sec) << "/s" << endl;
```

**빌드 순서**: `DummyClient` 만 다시 빌드. 서버·S1 무관.

---

## 검증

### ① P1 — 3인 루프 10분

```
DummyClient.exe -bots 3
```

| 확인 | 기대 |
| ---- | ---- |
| `[BOT DIED]` | 약 50초마다 발생 (HP 500 ÷ 10 × 1초) |
| `[BOT] disconnected` → 재접속 | 1초 안에 자리가 채워짐 |
| 10분 방치 | **크래시 0 · 서버 살아있음 · 봇 수 3 유지** |
| 서버 `[MOVE REJECT]` | **0건** — 추격 이동이 검증에 안 걸려야 한다 |

### ② P1 — 3인 동시 공격 시 TTK 감소

3기가 서로 가장 가까운 대상을 고르므로 자연히 뭉친다. 서버 로그의 `[DIED]` 간격을 본다.

```
1:1   HP 500 ÷ (10 × 1회/초)     = 50초
3:1   HP 500 ÷ (30 × 1회/초)     ≈ 16.7초
```

**유의미한 감소가 관찰되면 통과.** 정확히 16.7초일 필요는 없다 — 3기가 항상 같은 대상을 치지는 않기 때문이다.

### ③ G1 — 전투 패킷 대역폭 증가분

```
DummyClient.exe -bots 30
```

| 항목 | 이동만 (기준선) | 전투 포함 | 판정 기준 |
| ---- | ---- | ---- | ---- |
| SV-1 p95 | ≤100 us | ? | < 16,600 us |
| SV-5 sent | 3.20 Mbps | ? | ≤ 12 Mbps |
| SV-5 per-client | 107 kbps | ? | ≤ 400 kbps |
| SV-6 메모리 | 91.7 MB | ? | ≤ 130 MB |
| 봇 공격 수 | 0/s | **약 30/s** | — |

> **증가분이 작을 것으로 예상된다.** 30봇이 초당 30회 공격하면 `S_ATTACK` · `S_DAMAGE` 가
> 각각 초당 30건인데, 33ms 배치로 묶이므로 **틱당 1건씩**이다.
> 이동이 틱당 30건인 것에 비하면 미미하다.
>
> 🔴 **그래도 반드시 재서 기록한다.** 로드맵이 G1 에서 명시적으로 요구하는 항목이고,
> "예상보다 컸다" 는 결과가 나오면 그게 P1.5(스킬) 전에 알아야 할 정보다.

---

## 알려진 한계

| | |
| ---- | ---- |
| 봇이 한 점으로 뭉친다 | 서로 가장 가까운 대상을 쫓으므로 결국 모인다. **부하 측정에는 최악 케이스라 오히려 유리**하지만 실제 교전 분포와는 다르다 |
| 봇이 CC 를 쓰지 않는다 | P1 에는 스킬이 없어 CC 공급원이 디버그 커맨드뿐이다. **CC 패킷 부하는 P1.5 에서 다시 잰다** |
| 봇이 회피하지 않는다 | 맞기만 한다. TTK 가 이론값에 가깝게 나오므로 검증에는 오히려 낫다 |
| 재접속 시 새 `object_id` | 서버가 새 id 를 발급한다. 로그를 추적할 때 같은 봇이 이어지지 않는다 |
