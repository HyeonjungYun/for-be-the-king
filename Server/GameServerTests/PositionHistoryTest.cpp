#include "pch.h"
#include "PositionHistory.h"

// 지난 몇 초의 자리 (2026-10-11 3차 로스터 - 회귀 1167 "3초 전에 서 있던 자리로" · 발자국 추적 1187)

namespace
{
	constexpr uint64 MS = 1000;
	constexpr uint64 T0 = 1'000'000'000;		// 아무 시각 - 0 근처의 뺄셈을 피한다

	// 10 Hz 로 x 축을 따라 걷는다 - k 번째 표본은 (k × 34, 0) = 340 cm/s
	PositionHistory Walked(uint32 samples)
	{
		PositionHistory h;
		for (uint32 k = 0; k < samples; k++)
			h.Record(T0 + k * 100 * MS, k * 34.f, 0.f);
		return h;
	}
}

TEST(PositionHistory, 비어_있으면_자리가_없다)
{
	PositionHistory h;
	float x = 0.f;
	float y = 0.f;
	EXPECT_FALSE(h.At(T0, x, y));
	EXPECT_EQ(h.Count(), 0u);
}

TEST(PositionHistory, 그_시각_이전의_가장_늦은_표본이_그때의_자리다)
{
	const PositionHistory h = Walked(50);		// 5 s
	float x = 0.f;
	float y = 0.f;

	// 마지막 표본은 T0 + 4.9 s. 3 s 전 = 1.9 s → 19 번째 표본 (646, 0)
	ASSERT_TRUE(h.At(T0 + 4900 * MS - 3000 * MS, x, y));
	EXPECT_FLOAT_EQ(x, 19 * 34.f);

	// 표본 사이 시각은 앞 표본 (1.95 s → 1.9 s)
	ASSERT_TRUE(h.At(T0 + 1950 * MS, x, y));
	EXPECT_FLOAT_EQ(x, 19 * 34.f);
}

TEST(PositionHistory, 기록보다_이른_시각이면_가장_오래된_자리다)
{
	// 막 들어와 1 s 만 걸었다 - 3 s 전은 기록에 없다 → 들어온 자리
	const PositionHistory h = Walked(10);
	float x = -1.f;
	float y = -1.f;
	ASSERT_TRUE(h.At(T0 - 2000 * MS, x, y));
	EXPECT_FLOAT_EQ(x, 0.f);
}

TEST(PositionHistory, 용량을_넘으면_오래된_것부터_덮는다)
{
	const PositionHistory h = Walked(PositionHistory::CAPACITY + 20);
	EXPECT_EQ(h.Count(), PositionHistory::CAPACITY);

	// 가장 오래 남은 것은 20 번째 표본 - 그보다 이른 시각은 그 자리
	float x = 0.f;
	float y = 0.f;
	ASSERT_TRUE(h.At(T0, x, y));
	EXPECT_FLOAT_EQ(x, 20 * 34.f);
}

TEST(PositionHistory, 발자국은_그_시각_이후를_간격마다_오래된_것부터)
{
	const PositionHistory h = Walked(60);		// 6 s
	vector<float> xs;

	// 마지막 2 s (4.0 ~ 5.9 s) 를 0.5 s 간격으로 → 4.0 · 4.5 · 5.0 · 5.5
	h.ForEachSince(T0 + 4000 * MS, 500 * MS, [&](const PositionHistory::Sample& s) { xs.push_back(s.x); });

	ASSERT_EQ(xs.size(), 4u);
	EXPECT_FLOAT_EQ(xs[0], 40 * 34.f);
	EXPECT_FLOAT_EQ(xs[1], 45 * 34.f);
	EXPECT_FLOAT_EQ(xs[3], 55 * 34.f);
}

TEST(PositionHistory, 덮어쓴_뒤에도_발자국은_시간_순이다)
{
	const PositionHistory h = Walked(PositionHistory::CAPACITY + 5);
	uint64 last = 0;
	int32 n = 0;
	h.ForEachSince(0, 0, [&](const PositionHistory::Sample& s)
		{
			EXPECT_GT(s.atUs, last);
			last = s.atUs;
			n++;
		});
	EXPECT_EQ(n, static_cast<int32>(PositionHistory::CAPACITY));
}
