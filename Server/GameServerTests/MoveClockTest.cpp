#include "pch.h"
#include "MoveClock.h"

// 이동 검증의 경과 시간 (MoveClock.h · roadmap.md § 0-11 후속). 이 값에 이동 예산이 비례한다.

namespace
{
	constexpr uint64 TICK_US = 33'000;		// 봇 · 클라 이동 송신 간격(30Hz)
	constexpr uint64 TICK_MS = 33;
	constexpr uint64 START_MS = 1'000'000;	// 클라 시계의 아무 시작값
}

TEST(MoveClock, 시각이_없으면_서버_간격을_쓴다)
{
	MoveClock clock;
	EXPECT_DOUBLE_EQ(clock.Interval(TICK_US, 0), 0.033);
	EXPECT_DOUBLE_EQ(clock.Interval(50'000, 0), 0.05);
}

TEST(MoveClock, 첫_패킷은_서버_간격을_쓰고_기준을_잡는다)
{
	MoveClock clock;
	EXPECT_DOUBLE_EQ(clock.Interval(TICK_US, START_MS), 0.033);
	EXPECT_EQ(clock.lastClientMs, START_MS);
	EXPECT_EQ(clock.leadUs, 0);
}

TEST(MoveClock, 몰려_도착한_이동도_보낸_간격으로_잰다)
{
	// soak2 의 SV-3 109건 - 보내는 쪽이 멈췄다 10개를 한꺼번에 보냈다. 서버 간격은 거의 0
	MoveClock clock;
	clock.Interval(TICK_US, START_MS);

	double total = 0.0;
	const double first = clock.Interval(10 * TICK_US, START_MS + TICK_MS);		// 늦게 온 첫 패킷
	total += first;
	for (uint64 i = 2; i <= 10; i++)
		total += clock.Interval(0, START_MS + i * TICK_MS);					// 나머지는 같은 순간에 처리

	// 예전 방식(서버 간격)이면 첫 패킷 0.33초 + 나머지 0 - 나머지 9개의 이동 거리를 예산이 못 덮어 거부됐다.
	// 이제는 패킷마다 보낸 간격 0.033초씩이다
	EXPECT_DOUBLE_EQ(first, 0.033);
	EXPECT_NEAR(total, 10 * 0.033, 1e-9);
	EXPECT_EQ(clock.leadUs, 0);			// 보낸 시간과 받은 시간의 합이 같다
}

TEST(MoveClock, 시각을_부풀려도_서버_시계보다_상한_넘게_앞서지_못한다)
{
	// 스피드핵 - 실제로는 33ms 마다 보내면서 시각은 66ms 씩 올린다
	MoveClock clock;
	clock.Interval(TICK_US, START_MS);

	double claimed = 0.0;
	double real = 0.0;
	for (uint64 i = 1; i <= 300; i++)		// 약 10초
	{
		claimed += clock.Interval(TICK_US, START_MS + i * 2 * TICK_MS);
		real += TICK_US / 1'000'000.0;
	}

	EXPECT_LE(claimed, real + MoveClock::MAX_LEAD_US / 1'000'000.0 + 1e-6);
	EXPECT_EQ(clock.leadUs, MoveClock::MAX_LEAD_US);
}

TEST(MoveClock, 오래_끊겼다_몰아_보내도_2초까지만_인정한다)
{
	// 랙 스위치 - 5초 동안 보내지 않다가(서버 간격 5초 · 시각도 5초) 이후 10초 분량을 순식간에 보낸다
	MoveClock clock;
	clock.Interval(TICK_US, START_MS);

	clock.Interval(5'000'000, START_MS + 5000);			// 5초 뒤 정상 도착 - lead 0
	EXPECT_EQ(clock.leadUs, 0);

	// 이제 서버 시간은 거의 흐르지 않는데 시각만 3.3초 분량 올라간다 - 뒤처진 몫이 없으니 앞섬 상한을 넘는 몫은 잘린다
	double claimed = 0.0;
	for (uint64 i = 1; i <= 100; i++)
		claimed += clock.Interval(1, START_MS + 5000 + i * TICK_MS);

	EXPECT_LE(claimed, MoveClock::MAX_LEAD_US / 1'000'000.0 + 1e-3);

	// 반대로 진짜 지연(보낸 시각은 고르게, 도착만 늦음)은 뒤처진 몫이 2초까지만 쌓인다
	MoveClock lagged;
	lagged.Interval(TICK_US, START_MS);
	lagged.Interval(5'000'000, START_MS + TICK_MS);		// 33ms 뒤에 보낸 것이 5초 뒤 도착
	EXPECT_EQ(lagged.leadUs, -MoveClock::MAX_LAG_US);
}

TEST(MoveClock, 늦게_도착한_첫_패킷이_기준이_돼도_정상_이동을_깎지_않는다)
{
	// 보내는 쪽이 1.5초 멈춘 사이에 입장 - 첫 패킷(기준)이 1.5초 늦게 왔다. 이후 패킷은 제때 온다.
	// 이후 패킷은 전부 "1.5초 앞섰다" 로 보이지만 상한 안이라 보낸 간격 그대로 받는다 (2026-10-05 bnew 실측)
	MoveClock clock;
	clock.Interval(TICK_US, START_MS);										// 기준 - 실제로는 1.5초 전에 보낸 것

	// 그 1.5초 동안 쥐고 있던 45개가 한꺼번에 도착한다(서버 간격 0)
	for (uint64 i = 1; i <= 45; i++)
		EXPECT_DOUBLE_EQ(clock.Interval(0, START_MS + i * TICK_MS), 0.033);

	// 이후 제때 오는 패킷도 그대로
	for (uint64 i = 46; i <= 100; i++)
		EXPECT_DOUBLE_EQ(clock.Interval(TICK_US, START_MS + i * TICK_MS), 0.033);

	EXPECT_LE(clock.leadUs, MoveClock::MAX_LEAD_US);
}

TEST(MoveClock, 시계가_거꾸로_가면_기준을_다시_잡는다)
{
	MoveClock clock;
	clock.Interval(TICK_US, START_MS + 5000);
	EXPECT_DOUBLE_EQ(clock.Interval(TICK_US, START_MS), 0.033);	// 서버 간격
	EXPECT_EQ(clock.lastClientMs, START_MS);
	EXPECT_EQ(clock.leadUs, 0);
}
