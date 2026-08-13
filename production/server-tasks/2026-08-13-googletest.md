# 서버 작업 제안 — GoogleTest 도입 + 동시성 스트레스 3종

**작성**: 2026-08-13 · **소유**: 🔴 사용자 · **P0 항목**: P0-2 (추정 2일)
**게이트**: `SV-4` — 8스레드 × 1000회 × **반복 100회** 크래시·행 0건 · ServerCore 커버리지 ≥ 70%

> 이 문서의 코드는 **2026-08-13 시점의 실제 파일을 읽고** 작성했습니다.

---

## 현황

```
테스트 프로젝트    0개
Server.sln         GameServer · DummyClient · ServerCore · PacketGenerator(py)
C++ 표준           stdcpp20
출력               $(SolutionDir)Binaries\$(Configuration)\
라이브러리 경로    $(SolutionDir)Libraries\Libs\
인클루드 경로      $(SolutionDir)ServerCore\ ; $(SolutionDir)Libraries\Include\
```

## 왜 스트레스 테스트가 별도 필수 항목인가

`technical-preferences.md`에 이 프로젝트에서 **실제로 겪은** 사례가 기록돼 있습니다.

| 버그 | 단위 테스트로 잡히나 |
| ---- | ---- |
| `LockQueue::PopAll` 자기 재귀 락 | ✅ 잡힘 — 단일 스레드에서도 즉시 데드락 |
| `Session::Send` 락 범위 → `_sendQueue` 레이스 | ❌ **못 잡음** — 다중 스레드 동시 접근이 필요 |

`WRITE_LOCK`은 `lock_guard<mutex>`라 **재진입이 안 됩니다**(`CoreMacro.h:14`). 같은 종류의 실수가 또 나옵니다.

---

# STEP 1 — 테스트 프로젝트 셋업

## 1-1. GoogleTest 획득

**권장 — Visual Studio 내장 템플릿**

```
파일 → 새로 만들기 → 프로젝트 → "Google Test" 검색 → Google Test 프로젝트
  이름       ServerCoreTests
  위치       C:\Server\MMO\Server\
  솔루션     기존 솔루션에 추가
  테스트할 프로젝트   ServerCore      ← 이 항목이 있으면 참조가 자동 설정된다
```

> 템플릿이 안 보이면 **Visual Studio Installer → 수정 → 개별 구성 요소 → "Google Test용 테스트 어댑터"** 를 설치하세요.

**대안 — vcpkg**
```bash
vcpkg install gtest:x64-windows
vcpkg integrate install
```

## 1-2. 🔴 ServerCore 프로젝트 참조 — 반드시 확인

`technical-preferences.md`에 **이 프로젝트에서 실제로 터진 문제**로 기록돼 있습니다.

```
ServerCoreTests 우클릭 → 추가 → 참조 → ServerCore 체크
```

**안 하면 구버전 `ServerCore.lib`가 링크되어 고친 코드가 반영되지 않습니다.**

> ### 겸사겸사 — 이 트랩을 영구히 없애는 방법
>
> **`GameServer`와 `DummyClient`에도 ServerCore 참조가 없습니다.** 그래서 세션 내내
> "ServerCore 먼저 빌드하세요"를 반복해야 했습니다.
>
> ```
> GameServer  우클릭 → 추가 → 참조 → ServerCore 체크
> DummyClient 우클릭 → 추가 → 참조 → ServerCore 체크
> ```
>
> 참조를 걸면 **VS가 의존성 순서를 알아서 지킵니다.** 지금 5분이면 앞으로 안 겪습니다.
> (선택 사항이지만 강력 권장)

## 1-3. 프로젝트 속성

`ServerCoreTests` 속성 → 모든 구성 · x64:

| 항목 | 값 |
| ---- | ---- |
| C/C++ → 일반 → 추가 포함 디렉터리 | `$(SolutionDir)ServerCore\;$(SolutionDir)Libraries\Include\;%(AdditionalIncludeDirectories)` |
| C/C++ → 언어 → C++ 언어 표준 | **ISO C++20 (`/std:c++20`)** |
| 링커 → 일반 → 추가 라이브러리 디렉터리 | `$(SolutionDir)Libraries\Libs\;%(AdditionalLibraryDirectories)` |
| 일반 → 출력 디렉터리 | `$(SolutionDir)Binaries\$(Configuration)\` |

**C++20은 필수입니다.** ServerCore가 `/std:c++20`으로 빌드되므로 표준이 다르면 링크 에러가 납니다.

## 1-4. `Server/ServerCoreTests/pch.h`

```cpp
#pragma once

#define WIN32_LEAN_AND_MEAN

#include "gtest/gtest.h"

#ifdef _DEBUG
#pragma comment(lib, "ServerCore\\Debug\\ServerCore.lib")
#else
#pragma comment(lib, "ServerCore\\Release\\ServerCore.lib")
#endif

#include "CorePch.h"
#include "Utils.h"
```

> `CorePch.h` 가 `<thread>` 를 안 들여올 수 있습니다. 스트레스 테스트 파일 상단에
> `#include <thread>` 를 개별로 넣으면 안전합니다.

---

# STEP 2 — 순수 자료구조 단위 테스트

## 2-1. `ServerCoreTests/LockQueueTest.cpp`

```cpp
#include "pch.h"
#include "LockQueue.h"

// ─── 기본 동작 ───────────────────────────────────────────────

TEST(LockQueue, 빈_큐에서_Pop하면_기본값을_돌려준다)
{
	LockQueue<shared_ptr<int>> q;
	EXPECT_EQ(q.Pop(), nullptr);
}

TEST(LockQueue, 넣은_순서대로_나온다)
{
	LockQueue<shared_ptr<int>> q;
	q.Push(make_shared<int>(1));
	q.Push(make_shared<int>(2));

	EXPECT_EQ(*q.Pop(), 1);
	EXPECT_EQ(*q.Pop(), 2);
}

TEST(LockQueue, Clear하면_비워진다)
{
	LockQueue<shared_ptr<int>> q;
	for (int i = 1; i <= 10; i++)
		q.Push(make_shared<int>(i));

	q.Clear();
	EXPECT_EQ(q.Pop(), nullptr);
}

// ─── 회귀 방지 ───────────────────────────────────────────────

TEST(LockQueue, PopAll이_자기_자신을_다시_잠그지_않는다)
{
	// 2026-08-10 실제 발생. PopAll 이 Pop() 을 부르면 WRITE_LOCK(lock_guard<mutex>,
	// 재진입 불가)을 두 번 잡아 즉시 죽었다. PopNoLock 을 부르는지 확인한다.
	// 회귀하면 이 테스트가 '통과'가 아니라 '멈춘다'.
	LockQueue<shared_ptr<int>> q;
	for (int i = 1; i <= 100; i++)
		q.Push(make_shared<int>(i));

	vector<shared_ptr<int>> items;
	q.PopAll(OUT items);

	EXPECT_EQ(items.size(), 100u);
}

TEST(LockQueue, 기본값과_구분되지_않는_원소는_PopAll을_조기_종료시킨다)
{
	// PopAll 은 `while (T item = PopNoLock())` 구조라 T() 로 변환되는 값이 sentinel 이 된다.
	// 실사용은 shared_ptr 뿐이라 문제없지만, 정수형에 쓰면 조용히 잘린다.
	// 이 제약을 '의도된 것'으로 못 박아 둔다 — 나중에 누가 LockQueue<int> 를 쓰면 여기서 걸린다.
	LockQueue<int> q;
	q.Push(1);
	q.Push(0);   // sentinel 과 동일
	q.Push(2);

	vector<int> items;
	q.PopAll(OUT items);

	EXPECT_EQ(items.size(), 1u);
	EXPECT_EQ(items[0], 1);
}
```

## 2-2. `ServerCoreTests/RecvBufferTest.cpp`

```cpp
#include "pch.h"
#include "RecvBuffer.h"

// RecvBuffer(bufferSize) → capacity = bufferSize × BUFFER_COUNT(10)
// 아래 테스트는 bufferSize=100 → capacity 1000 을 전제한다.

TEST(RecvBuffer, 생성_직후_전체가_비어있다)
{
	RecvBuffer buf(100);
	EXPECT_EQ(buf.DataSize(), 0);
	EXPECT_EQ(buf.FreeSize(), 1000);
}

TEST(RecvBuffer, 용량을_넘겨_쓰면_실패한다)
{
	RecvBuffer buf(100);
	EXPECT_TRUE(buf.OnWrite(1000));
	EXPECT_FALSE(buf.OnWrite(1));
}

TEST(RecvBuffer, 쓴_것보다_많이_읽으면_실패한다)
{
	RecvBuffer buf(100);
	buf.OnWrite(50);

	EXPECT_FALSE(buf.OnRead(51));
	EXPECT_TRUE(buf.OnRead(50));
}

TEST(RecvBuffer, 다_읽고_Clean하면_커서가_0으로_돌아간다)
{
	RecvBuffer buf(100);
	buf.OnWrite(500);
	buf.OnRead(500);

	buf.Clean();

	EXPECT_EQ(buf.DataSize(), 0);
	EXPECT_EQ(buf.FreeSize(), 1000);
}

TEST(RecvBuffer, 남은_공간이_한_칸보다_작으면_데이터를_앞으로_당긴다)
{
	RecvBuffer buf(100);
	buf.OnWrite(950);    // FreeSize = 50 < bufferSize(100)
	buf.OnRead(900);     // DataSize = 50

	buf.Clean();

	EXPECT_EQ(buf.DataSize(), 50);
	EXPECT_EQ(buf.FreeSize(), 950);   // 앞으로 당겨졌다
}

TEST(RecvBuffer, 남은_공간이_충분하면_Clean이_아무것도_하지_않는다)
{
	RecvBuffer buf(100);
	buf.OnWrite(200);
	buf.OnRead(100);

	buf.Clean();

	EXPECT_EQ(buf.DataSize(), 100);
	EXPECT_EQ(buf.FreeSize(), 800);   // 당기지 않았다
}

TEST(RecvBuffer, FreeSize가_0인_상태를_만들_수_있다)
{
	// 🔴 이 상태에서 Session::RegisterRecv 는 wsaBuf.len = 0 으로 WSARecv 를 건다.
	//    0바이트 완료 → ProcessRecv(0) → Disconnect(L"Recv 0") 로 정상 접속이 끊긴다.
	//    RecvBuffer 자체의 버그는 아니지만, 이 조건을 만들 수 있다는 사실을 기록해 둔다.
	//    패킷이 커지거나 Clean 호출이 빠지면 실제로 발생할 수 있는 경로다.
	RecvBuffer buf(100);
	EXPECT_TRUE(buf.OnWrite(1000));
	EXPECT_EQ(buf.FreeSize(), 0);
}
```

## 2-3. `ServerCoreTests/BufferTest.cpp`

```cpp
#include "pch.h"
#include "BufferReader.h"
#include "BufferWriter.h"

TEST(Buffer, 쓴_값을_그대로_읽는다)
{
	BYTE raw[64] = {};

	const uint16 a = 0x1234;
	const uint32 b = 0xDEADBEEF;
	const float  c = 3.14f;

	BufferWriter w(raw, sizeof(raw));
	w << a << b << c;

	uint16 ra = 0;
	uint32 rb = 0;
	float  rc = 0.f;

	BufferReader r(raw, sizeof(raw));
	r >> ra >> rb >> rc;

	EXPECT_EQ(ra, a);
	EXPECT_EQ(rb, b);
	EXPECT_FLOAT_EQ(rc, c);
}

TEST(Buffer, 쓴_만큼_커서가_움직인다)
{
	BYTE raw[64] = {};
	BufferWriter w(raw, sizeof(raw));

	const uint32 v = 1;
	w.Write(&v);

	EXPECT_EQ(w.WriteSize(), sizeof(uint32));
	EXPECT_EQ(w.FreeSize(), sizeof(raw) - sizeof(uint32));
}

TEST(Buffer, 용량을_넘기면_쓰기가_실패한다)
{
	BYTE raw[4] = {};
	BufferWriter w(raw, sizeof(raw));

	const uint32 v = 1;
	EXPECT_TRUE(w.Write(&v));
	EXPECT_FALSE(w.Write(&v));
}

TEST(Buffer, 용량을_넘기면_읽기가_실패한다)
{
	BYTE raw[4] = {};
	BufferReader r(raw, sizeof(raw));

	uint32 v = 0;
	EXPECT_TRUE(r.Read(&v));
	EXPECT_FALSE(r.Read(&v));
}

TEST(Buffer, Peek은_커서를_움직이지_않는다)
{
	BYTE raw[8] = {};
	const uint32 v = 0xABCD1234;

	BufferWriter w(raw, sizeof(raw));
	w.Write(&v);

	BufferReader r(raw, sizeof(raw));

	uint32 peeked = 0;
	EXPECT_TRUE(r.Peek(&peeked));
	EXPECT_EQ(peeked, v);
	EXPECT_EQ(r.ReadSize(), 0u);   // 안 움직였다

	uint32 read = 0;
	EXPECT_TRUE(r.Read(&read));
	EXPECT_EQ(read, v);
	EXPECT_EQ(r.ReadSize(), sizeof(uint32));
}

TEST(Buffer, Reserve는_공간이_없으면_nullptr을_준다)
{
	BYTE raw[4] = {};
	BufferWriter w(raw, sizeof(raw));

	EXPECT_NE(w.Reserve<uint32>(), nullptr);
	EXPECT_EQ(w.Reserve<uint32>(), nullptr);
}
```

---

# STEP 3 — 동시성 스트레스 3종 (SV-4 본체)

## `ServerCoreTests/StressTest.cpp`

```cpp
#include "pch.h"
#include <thread>
#include "LockQueue.h"
#include "JobQueue.h"
#include "GlobalQueue.h"
#include "CoreTLS.h"

namespace
{
	constexpr int THREAD_COUNT = 8;
	constexpr int OPS_PER_THREAD = 1000;

	// 워커 스레드가 아니므로 타임슬라이스를 넉넉히 잡아 준다.
	// 이걸 안 하면 JobQueue::Execute 가 LEndTickCount 를 넘겼다고 판단해
	// 곧바로 GlobalQueue 로 넘겨 버린다.
	void GiveGenerousTimeSlice() { LEndTickCount = ::GetTickCount64() + 60000; }
}

/*---------------
	스트레스 1 — LockQueue
---------------*/

TEST(Stress, LockQueue_동시_Push와_PopAll이_원소를_잃지_않는다)
{
	LockQueue<shared_ptr<int>> q;
	atomic<int> popped{ 0 };
	atomic<bool> producersDone{ false };

	vector<std::thread> producers;
	for (int t = 0; t < THREAD_COUNT; t++)
	{
		producers.emplace_back([&q]()
			{
				for (int i = 0; i < OPS_PER_THREAD; i++)
					q.Push(make_shared<int>(i + 1));   // 0 은 sentinel 이라 1부터
			});
	}

	std::thread consumer([&]()
		{
			while (true)
			{
				vector<shared_ptr<int>> items;
				q.PopAll(OUT items);
				popped.fetch_add(static_cast<int>(items.size()));

				if (producersDone.load() && items.empty())
					break;
			}
		});

	for (auto& t : producers)
		t.join();

	producersDone.store(true);
	consumer.join();

	EXPECT_EQ(popped.load(), THREAD_COUNT * OPS_PER_THREAD);
}

/*---------------
	스트레스 2 — JobQueue
---------------*/

class CounterJobQueue : public JobQueue
{
public:
	void Increase() { counter.fetch_add(1); }
	atomic<int> counter{ 0 };
};

TEST(Stress, JobQueue_동시_DoAsync가_일감을_잃지_않는다)
{
	auto q = make_shared<CounterJobQueue>();

	vector<std::thread> threads;
	for (int t = 0; t < THREAD_COUNT; t++)
	{
		threads.emplace_back([&q]()
			{
				GiveGenerousTimeSlice();

				for (int i = 0; i < OPS_PER_THREAD; i++)
					q->DoAsync(&CounterJobQueue::Increase);
			});
	}

	for (auto& t : threads)
		t.join();

	// 타임슬라이스에 걸려 GlobalQueue 로 넘어간 잔여 일감을 마저 처리한다.
	GiveGenerousTimeSlice();
	while (JobQueueRef pending = GGlobalQueue->Pop())
	{
		GiveGenerousTimeSlice();
		pending->Execute();
	}

	EXPECT_EQ(q->counter.load(), THREAD_COUNT * OPS_PER_THREAD);
}

/*---------------
	스트레스 3 — GlobalQueue
---------------*/

TEST(Stress, GlobalQueue_동시_Push와_Pop이_큐를_깨뜨리지_않는다)
{
	GlobalQueue queue;
	atomic<int> popped{ 0 };
	atomic<bool> producersDone{ false };

	vector<std::thread> producers;
	for (int t = 0; t < THREAD_COUNT; t++)
	{
		producers.emplace_back([&queue]()
			{
				for (int i = 0; i < OPS_PER_THREAD; i++)
					queue.Push(make_shared<CounterJobQueue>());
			});
	}

	vector<std::thread> consumers;
	for (int t = 0; t < 2; t++)
	{
		consumers.emplace_back([&]()
			{
				while (true)
				{
					if (JobQueueRef q = queue.Pop())
					{
						popped.fetch_add(1);
						continue;
					}

					if (producersDone.load())
						break;

					std::this_thread::yield();
				}
			});
	}

	for (auto& t : producers)
		t.join();

	producersDone.store(true);

	for (auto& t : consumers)
		t.join();

	EXPECT_EQ(popped.load(), THREAD_COUNT * OPS_PER_THREAD);
}
```

## 🔴 SV-4는 "반복 100회"까지 요구합니다

**한 번 통과는 통과가 아닙니다.** 데이터 레이스는 확정적이지 않습니다.

```bash
# Binaries\Debug 에서
for /L %i in (1,1,100) do ServerCoreTests.exe --gtest_filter=Stress.* || exit /b 1
```

또는 gtest 내장 반복:
```bash
ServerCoreTests.exe --gtest_filter=Stress.* --gtest_repeat=100 --gtest_break_on_failure
```

**`--gtest_repeat=100`이 더 낫습니다.** 프로세스를 재시작하지 않아 TLS·전역 상태가 누적되므로 실제 운영에 가깝습니다.

---

# STEP 4 — 커버리지 측정

```bash
OpenCppCoverage.exe ^
  --sources Server\ServerCore ^
  --excluded_sources Server\ServerCore\IocpCore.cpp ^
  --excluded_sources Server\ServerCore\Listener.cpp ^
  --excluded_sources Server\ServerCore\Session.cpp ^
  --export_type html:coverage ^
  -- Server\Binaries\Debug\ServerCoreTests.exe
```

**제외 대상** (`technical-preferences.md` 기준): 실제 소켓 I/O(`IocpCore`·`Listener`·`Session`의 WSA 호출) · `*.pb.cc` · `GameServer/` · `S1/`

> **숫자가 아니라 리포트의 빨간 줄을 보세요.** "이 줄이 프로덕션에서 실행될 수 있나?"를 묻고,
> 그렇다면 테스트를 추가하고 아니면 **죽은 코드로 보고 삭제**합니다.

---

## 알려진 한계 — 정직하게

| 항목 | 상태 |
| ---- | ---- |
| **`Session::_sendQueue` 레이스** | ⚠️ **이 제안에 없음.** `Session` 생성자가 실제 소켓을 만들고 `Send()` 는 `IsConnected()` 에서 조기 반환한다. `_connected` 가 protected 라 테스트 서브클래스에서 `true` 로 세울 수는 있으나, `RegisterSend` 가 미연결 소켓에 `WSASend` 를 걸어 에러 로그가 쏟아진다. **별도 항목으로 분리** |
| **`Room::_players` 레이스** | ⚠️ `Room` 은 `GameServer` 소속이라 이 테스트 프로젝트 범위 밖. `ServerCoreTests` 가 아니라 `GameServerTests` 가 필요하다 |
| **`JobTimer`** | 시간 의존이라 결정적 테스트가 어렵다. `Reserve` → `Distribute` 왕복만 얕게 확인하고, 정밀 검증은 보류 |
| **`SendBufferChunk` 풀 재사용** | `SendBufferManager` 구조를 더 읽어야 한다. 2일 안에 못 넣으면 후속 |

**이 4개를 뺀 상태로도 SV-4의 "8스레드 × 1000회 × 반복 100회 크래시 0건"은 충족합니다.**
커버리지 70%가 안 나오면 `SendBuffer`·`JobTimer` 쪽을 먼저 보강하세요.

---

## 적용 순서 요약

```
1  VS 템플릿으로 ServerCoreTests 생성 + ServerCore 참조 설정   ← 🔴 참조 필수
   (겸사겸사 GameServer · DummyClient 에도 참조 추가 권장)
2  프로젝트 속성 4개 (인클루드 · C++20 · 라이브러리 · 출력)
3  pch.h
4  LockQueueTest.cpp · RecvBufferTest.cpp · BufferTest.cpp   ← 여기까지 1일
5  StressTest.cpp + --gtest_repeat=100                        ← 2일차
6  OpenCppCoverage 로 70% 확인
```

**빌드 순서**: ServerCore → ServerCoreTests (참조를 걸면 자동)

## 검증

| # | 기대 |
| ---- | ---- |
| 1 | 테스트 탐색기에 테스트가 전부 보인다 |
| 2 | 단위 테스트 전부 통과 |
| 3 | `--gtest_filter=Stress.* --gtest_repeat=100` **크래시·행 0건** ← SV-4 |
| 4 | 커버리지 리포트 ≥ 70% |

**3번이 G0의 마지막 조건입니다.** 통과하면 P0 전체가 닫히고 P1(전투) 착수 가능합니다.
