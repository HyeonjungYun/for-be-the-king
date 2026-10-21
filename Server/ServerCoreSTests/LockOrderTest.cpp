#include "pch.h"
#include <thread>
#include "LockOrder.h"

// 락 순서 검사 (LockOrder.h, 2026-10-21). 위반은 기본 처리기(크래시 · 로그) 대신 받아서 센다

namespace
{
	struct Violation
	{
		int32 held = 0;
		int32 wanted = 0;
	};

	vector<Violation> g_violations;

	void Record(int32 held, int32 wanted, const char*)
	{
		g_violations.push_back(Violation{ held, wanted });
	}

	struct ScopedHandler
	{
		ScopedHandler() { g_violations.clear(); LockOrder::SetViolationHandler(&Record); }
		~ScopedHandler() { LockOrder::SetViolationHandler(nullptr); }
	};

	// USE_LOCK 을 가진 "등록부" 흉내 - WRITE_LOCK_ORDERED 가 실제 매크로 그대로 쓰이는지
	struct Registry
	{
		USE_LOCK;
		int32 value = 0;

		void Touch() { WRITE_LOCK_ORDERED(20); value++; }
	};

	struct Coordinator
	{
		USE_LOCK;
		Registry* registry = nullptr;

		void TouchRegistryInside() { WRITE_LOCK_ORDERED(10); registry->Touch(); }		// 10 → 20 : 허용
	};
}

TEST(LockOrder, 낮은_수준에서_높은_수준으로는_위반이_아니다)
{
	ScopedHandler handler;

	Registry registry;
	Coordinator coordinator;
	coordinator.registry = &registry;

	coordinator.TouchRegistryInside();

	EXPECT_TRUE(g_violations.empty());
	EXPECT_EQ(LockOrder::Depth(), 0);
	EXPECT_EQ(registry.value, 1);
}

TEST(LockOrder, 높은_수준을_쥔_채_낮은_수준을_잡으면_위반이다)
{
	ScopedHandler handler;

	{
		LockOrderGuard registry(20, "test");
		LockOrderGuard party(10, "test");		// 20 → 10 : 역순
	}

	ASSERT_EQ(g_violations.size(), 1u);
	EXPECT_EQ(g_violations[0].held, 20);
	EXPECT_EQ(g_violations[0].wanted, 10);
	EXPECT_EQ(LockOrder::Depth(), 0);
}

TEST(LockOrder, 같은_수준_재진입도_위반이다)
{
	ScopedHandler handler;

	{
		LockOrderGuard a(20, "test");
		LockOrderGuard b(20, "test");
	}

	ASSERT_EQ(g_violations.size(), 1u);
	EXPECT_EQ(g_violations[0].held, 20);
	EXPECT_EQ(g_violations[0].wanted, 20);
}

TEST(LockOrder, 스레드마다_따로_센다)
{
	ScopedHandler handler;

	LockOrderGuard held(20, "main");

	// 다른 스레드는 아무것도 쥐지 않았으므로 10 을 잡아도 위반이 아니다
	std::thread other([]()
		{
			LockOrderGuard party(10, "other");
			EXPECT_EQ(LockOrder::Depth(), 1);
		});
	other.join();

	EXPECT_TRUE(g_violations.empty());
	EXPECT_EQ(LockOrder::TopLevel(), 20);
}
