#include "pch.h"
#include "FloorBounds.h"
#include "SpawnPlacement.h"
#include <algorithm>
#include <limits>
#include <random>
#include <vector>

// 층 경계 · 진입 지점 빈자리 (design/gdd/level-map.md AC-LM-02 · AC-LM-10).

TEST(FloorBounds, 경계_자체는_안이고_조금이라도_넘으면_밖이다)
{
	// 섬 맵(2026-10-11)부터 바깥 안전 사각형 ±30,000 - 섬(동서 589 m)을 감싼다. 진짜 안은 해안선 (WallMapTest)
	constexpr float H = FloorBounds::HALF_CM;
	static_assert(H == 30000.f, "level-map.md Core Rule 1 / ADR-0013 - the outer square is 600 m");

	EXPECT_TRUE(FloorBounds::Contains(0.f, 0.f));
	EXPECT_TRUE(FloorBounds::Contains(H, -H));
	EXPECT_TRUE(FloorBounds::Contains(-H, H));

	EXPECT_FALSE(FloorBounds::Contains(30000.01f, 0.f));
	EXPECT_FALSE(FloorBounds::Contains(0.f, -30000.01f));
	EXPECT_FALSE(FloorBounds::Contains(40000.f, 40000.f));
}

TEST(FloorBounds, NaN_과_무한대는_밖이다)
{
	// 속도 검증의 `distance > budget` 은 NaN 이면 거짓이라 통과했다 - 경계 검사가 막아야 한다
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();

	EXPECT_FALSE(FloorBounds::Contains(nan, 0.f));
	EXPECT_FALSE(FloorBounds::Contains(0.f, nan));
	EXPECT_FALSE(FloorBounds::Contains(inf, 0.f));
	EXPECT_FALSE(FloorBounds::Contains(0.f, -inf));
}

TEST(SpawnPlacement, 비어_있으면_첫_고리의_첫_칸에_놓는다)
{
	float x = 0.f, y = 0.f;
	SpawnPlacement::Find(1000.f, 2000.f, [](float, float) { return false; }, [] { return 0.5f; }, x, y);

	// 지점 중심이 아니라 150 cm 떨어진 첫 칸 - 중심은 비워 둔다
	EXPECT_NEAR(x, 1150.f, 0.01f);
	EXPECT_NEAR(y, 2000.f, 0.01f);
}

TEST(SpawnPlacement, 찬_칸을_건너뛰어_다음_빈_칸에_놓는다)
{
	int32 calls = 0;
	float x = 0.f, y = 0.f;
	// 앞의 10칸이 찼다 - 첫 고리 6칸 + 둘째 고리 4칸
	SpawnPlacement::Find(0.f, 0.f, [&](float, float) { return ++calls <= 10; }, [] { return 0.f; }, x, y);

	EXPECT_EQ(calls, 11);
	EXPECT_NEAR(std::sqrt(x * x + y * y), 300.f, 0.1f);		// 둘째 고리 = 반경 300 cm
}

TEST(SpawnPlacement, 고리_126칸은_서로_150cm_이상_떨어져_있다)
{
	// 같은 지점에 몰린 입장자끼리 겹치지 않는다 (AC-EP-04)
	std::vector<std::pair<float, float>> placed;
	for (int32 n = 0; n < 126; n++)
	{
		float x = 0.f, y = 0.f;
		SpawnPlacement::Find(0.f, 0.f,
			[&](float cx, float cy)
			{
				for (auto& p : placed)
					if ((p.first - cx) * (p.first - cx) + (p.second - cy) * (p.second - cy) < 149.f * 149.f)
						return true;
				return false;
			},
			[] { return 0.f; }, x, y);
		placed.emplace_back(x, y);
	}

	float minGap = 1e9f;
	for (size_t i = 0; i < placed.size(); i++)
		for (size_t j = i + 1; j < placed.size(); j++)
		{
			const float dx = placed[i].first - placed[j].first;
			const float dy = placed[i].second - placed[j].second;
			minGap = (std::min)(minGap, std::sqrt(dx * dx + dy * dy));	// 괄호 - windows.h 의 min 매크로
		}

	EXPECT_GE(minGap, 149.f);	// 바깥 고리 칸 간격 = 2π·900/36 ≈ 157 cm, 안쪽 고리 = 150 cm
}

TEST(SpawnPlacement, 다_차면_반경_900cm_원_안에_놓는다)
{
	// 예전 폴백(±900 cm 정사각형)은 모서리가 1,273 cm 까지 나가 무벽 원을 벗어났다
	std::mt19937 rng(1234);		// 시드 고정 - 결정적이다
	std::uniform_real_distribution<float> dist(0.f, 1.f);

	float maxR = 0.f;
	for (int32 trial = 0; trial < 1000; trial++)
	{
		float x = 0.f, y = 0.f;
		SpawnPlacement::Find(500.f, -500.f, [](float, float) { return true; }, [&] { return dist(rng); }, x, y);

		const float dx = x - 500.f;
		const float dy = y + 500.f;
		maxR = (std::max)(maxR, std::sqrt(dx * dx + dy * dy));
	}

	EXPECT_LE(maxR, SpawnPlacement::MAX_RADIUS_CM + 0.01f);
	EXPECT_GT(maxR, 800.f);		// 원 전체를 쓴다 - 한가운데에 몰아 넣지 않는다
}

TEST(SpawnPlacement, 반경은_레벨_무벽_원과_같다)
{
	// design/gdd/level-map.md Tuning Knobs entry_clear_radius 9 m · registry spawn_spread_radius 900 cm
	EXPECT_FLOAT_EQ(SpawnPlacement::MAX_RADIUS_CM, 900.f);
}
