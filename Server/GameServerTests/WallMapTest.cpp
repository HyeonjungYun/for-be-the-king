#include "pch.h"
#include "WallMap.h"
#include "WallTable.h"
#include "EntryPointTable.h"
#include "ItemTable.h"
#include "CsvTable.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>
#include <vector>

// 서버 2D 벽 (docs/architecture/adr-0006-server-2d-walls.md § Validation Criteria · level-map.md AC-LM-03 · 04 · 05).

namespace
{
	WallBox Box(float cx, float cy, float halfLength, float halfThickness, float yawDeg, WallKind kind = WallKind::Wall)
	{
		WallBox w;
		w.kind = kind;
		w.cx = cx;
		w.cy = cy;
		w.halfLength = halfLength;
		w.halfThickness = halfThickness;
		w.yawDeg = yawDeg;
		return w;
	}

	WallMap Build(vector<WallBox> walls)
	{
		WallMap map;
		string error;
		EXPECT_TRUE(map.Build(move(walls), OUT error)) << error;
		return map;
	}

	// 4 m × 0.5 m 벽 하나 - 원점 중심, 길이 방향 = X
	WallMap OneWall()
	{
		return Build({ Box(0.f, 0.f, 200.f, 25.f, 0.f) });
	}

	// 시드 고정 - 매번 같은 벽 2,000개. 층 안 무작위 자리 · 크기 · 방향 (4분의 1 은 0° · 90°)
	vector<WallBox> RandomWalls(int32 count, uint32 seed)
	{
		mt19937 rng(seed);
		uniform_real_distribution<float> pos(-13000.f, 13000.f);
		uniform_real_distribution<float> len(25.f, 300.f);
		uniform_real_distribution<float> thick(25.f, 60.f);
		uniform_real_distribution<float> yaw(0.f, 180.f);
		uniform_int_distribution<int32> quarter(0, 3);

		vector<WallBox> walls;
		for (int32 i = 0; i < count; i++)
		{
			float y = yaw(rng);
			if (quarter(rng) == 0)
				y = (quarter(rng) < 2) ? 0.f : 90.f;

			walls.push_back(Box(pos(rng), pos(rng), len(rng), thick(rng), y));
		}

		return walls;
	}

	// 1층 비슷한 밀도 - 56 m 집 3채에 4 m 격자 벽 · 격자점 기둥 + 엄폐 69 · 타워 3 (floor-1.md § 8 의 771개 근처)
	vector<WallBox> HouseLikeWalls(uint32 seed)
	{
		mt19937 rng(seed);
		uniform_real_distribution<float> coin(0.f, 1.f);

		const float houseX0 = -3600.f;
		const float houseY0[] = { -11200.f, -2800.f, 5600.f };

		vector<WallBox> walls;
		for (const float y0 : houseY0)
		{
			bool pillar[15][15] = {};

			for (int32 i = 0; i <= 14; i++)
			{
				for (int32 j = 0; j < 14; j++)
				{
					// 격자선 i 를 따라 j 번째 4 m 칸 - 가로 · 세로 각각
					if (coin(rng) < 0.25f)
					{
						walls.push_back(Box(houseX0 + i * 400.f, y0 + j * 400.f + 200.f, 200.f, 25.f, 90.f));
						pillar[i][j] = pillar[i][j + 1] = true;
					}

					if (coin(rng) < 0.25f)
					{
						walls.push_back(Box(houseX0 + j * 400.f + 200.f, y0 + i * 400.f, 200.f, 25.f, 0.f));
						pillar[j][i] = pillar[j + 1][i] = true;
					}
				}
			}

			for (int32 i = 0; i <= 14; i++)
			{
				for (int32 j = 0; j <= 14; j++)
				{
					if (pillar[i][j])
						walls.push_back(Box(houseX0 + i * 400.f, y0 + j * 400.f, 25.f, 25.f, 0.f, WallKind::Pillar));
				}
			}
		}

		uniform_real_distribution<float> open(-12000.f, 12000.f);
		for (int32 i = 0; i < 69; i++)
			walls.push_back(Box(open(rng), open(rng), 300.f, 50.f, (i % 2) ? 0.f : 90.f, WallKind::Cover));

		walls.push_back(Box(6200.f, -7200.f, 150.f, 150.f, 0.f, WallKind::Tower));
		walls.push_back(Box(6200.f, 7200.f, 150.f, 150.f, 0.f, WallKind::Tower));
		walls.push_back(Box(-7800.f, 0.f, 150.f, 150.f, 0.f, WallKind::Tower));

		return walls;
	}

	// 격자 없이 모든 상자를 본다 - ADR-0006 Alt 2. 격자 결과가 이것과 같아야 한다
	float BruteSweep(const WallMap& map, float ax, float ay, float bx, float by, float r)
	{
		float best = 1.f;
		for (const WallBox& w : map.Walls())
			best = (std::min)(best, WallGeom::Sweep(w, ax, ay, bx, by, r));
		return best;
	}

	bool BruteOverlaps(const WallMap& map, float x, float y, float r)
	{
		for (const WallBox& w : map.Walls())
		{
			if (WallGeom::SignedDistance(w, x, y) < r)
				return true;
		}
		return false;
	}

	float BrutePenetration(const WallMap& map, float x, float y)
	{
		float depth = 0.f;
		for (const WallBox& w : map.Walls())
			depth = (std::max)(depth, -WallGeom::SignedDistance(w, x, y));
		return depth;
	}

	// 1층 황금 파일 - 레벨 빌더 내보내기(Server/Data/export/floor0)를 그대로 복사했다. 데이터 버전 b716c8a5e4af.
	// 🔴 레벨을 바꿔 다시 내보내면 Server/GameServerTests/Data/floor1_*.csv 도 같이 바꾼다
	string FixturePath(const char* name)
	{
		return ItemTable::DefaultDataDir() + "\\..\\GameServerTests\\Data\\" + name;
	}

	// 1층 = floor_id 0. 실제 진입 지점 9곳으로 부팅 검사(9 m 무벽 원)까지 거친다
	bool LoadFloor1(OUT string& error)
	{
		string walls, versions, entries;
		if (Csv::ReadFile(FixturePath("floor1_walls.csv"), OUT walls) == false
			|| Csv::ReadFile(FixturePath("floor1_level_versions.csv"), OUT versions) == false
			|| Csv::ReadFile(FixturePath("floor1_entry_points.csv"), OUT entries) == false)
		{
			error = "fixture missing under " + FixturePath("");
			return false;
		}

		if (EntryPointTable::Parse(entries, 1, OUT error) == false)
			return false;

		return WallTable::Parse(&walls, &versions, 1, EntryPointTable::All(), OUT error);
	}
}

/*-------- 해석해 --------*/

TEST(WallMap, 점이_벽_안이면_겹치고_면_위는_겹치지_않는다)
{
	const WallMap map = OneWall();

	EXPECT_TRUE(map.Overlaps(0.f, 0.f, 0.f));
	EXPECT_TRUE(map.Overlaps(0.f, 24.9f, 0.f));
	EXPECT_FALSE(map.Overlaps(0.f, 25.f, 0.f));		// 면 위 - 안이 아니다
	EXPECT_FALSE(map.Overlaps(200.f, 0.f, 0.f));		// 끝 면 위

	// 반지름 42 (클라 캡슐) - 면에서 42 cm 미만이면 겹친다
	EXPECT_TRUE(map.Overlaps(0.f, 66.9f, 42.f));
	EXPECT_FALSE(map.Overlaps(0.f, 67.f, 42.f));

	EXPECT_FLOAT_EQ(map.PenetrationDepth(0.f, 0.f), 25.f);
	EXPECT_FLOAT_EQ(map.PenetrationDepth(0.f, 10.f), 15.f);
	EXPECT_FLOAT_EQ(map.PenetrationDepth(190.f, 0.f), 10.f);	// 끝 면이 더 가깝다
	EXPECT_FLOAT_EQ(map.PenetrationDepth(0.f, 100.f), 0.f);
}

TEST(WallMap, 벽을_가로지르는_선분은_들어가는_지점에서_막힌다)
{
	const WallMap map = OneWall();

	WallKind kind = WallKind::Tower;
	EXPECT_NEAR(map.SweepFirstHit(0.f, -100.f, 0.f, 100.f, 0.f, OUT &kind), 0.375f, 1e-5f);	// y = -25 에서 = 75 / 200
	EXPECT_EQ(kind, WallKind::Wall);
	EXPECT_TRUE(map.SweepBlocked(0.f, -100.f, 0.f, 100.f, 0.f));

	EXPECT_FLOAT_EQ(map.SweepFirstHit(300.f, -100.f, 300.f, 100.f, 0.f), 1.f);	// 벽 끝 너머로 지나간다

	// 대각선 - x 는 t > 0.5 에서, y 는 t ∈ (0.25, 0.75) 에서 안. 둘 다 안인 첫 순간 = 0.5
	EXPECT_NEAR(map.SweepFirstHit(-250.f, -50.f, -150.f, 50.f, 0.f), 0.5f, 1e-5f);
}

TEST(WallMap, 면을_따라_미끄러지거나_끝점이_면에_닿으면_막지_않는다)
{
	// AC-LM-04 "벽을 따라 미끄러지는 이동은 수락"
	const WallMap map = OneWall();

	EXPECT_FALSE(map.SweepBlocked(-300.f, 25.f, 300.f, 25.f, 0.f));
	EXPECT_FALSE(map.SweepBlocked(0.f, -100.f, 0.f, -25.f, 0.f));		// 끝점이 면에 닿기만 한다

	// 0.1 cm 안쪽을 지나가면 막힌다 - x = -200 에서 = 100 / 600
	EXPECT_NEAR(map.SweepFirstHit(-300.f, 24.9f, 300.f, 24.9f, 0.f), 100.f / 600.f, 1e-5f);
}

TEST(WallMap, 반지름이_있으면_부풀린_면과_모서리_원에서_멈춘다)
{
	// 넉백 · 대시 r = 42 (AC-LM-03)
	const WallMap wall = OneWall();
	EXPECT_NEAR(wall.SweepFirstHit(0.f, -200.f, 0.f, 0.f, 42.f), (200.f - 67.f) / 200.f, 1e-5f);	// y = -67 에서

	// 0.5 m 기둥 모서리로 대각선 접근 - 모서리 (-25, -25) 에서 42 cm 떨어진 순간
	const WallMap pillar = Build({ Box(0.f, 0.f, 25.f, 25.f, 0.f, WallKind::Pillar) });
	const float expected = (100.f - 25.f - 42.f / std::sqrt(2.f)) / 100.f;		// 0.453015
	EXPECT_NEAR(pillar.SweepFirstHit(-100.f, -100.f, 0.f, 0.f, 42.f), expected, 1e-4f);
}

TEST(WallMap, 회전한_벽도_근사_없이_판정한다)
{
	const float c = 1000.f;
	const float s = std::sqrt(0.5f);
	const WallMap map = Build({ Box(c, c, 200.f, 25.f, 45.f) });

	// 로컬 축 u = (s, s) 길이 방향 · v = (-s, s) 두께 방향
	auto world = [&](float l, float t) { return make_pair(c + l * s - t * s, c + l * s + t * s); };

	// 두께 방향으로 가로지른다 - 로컬 t = -25 에서
	{
		const auto a = world(0.f, -100.f);
		const auto b = world(0.f, 100.f);
		EXPECT_NEAR(map.SweepFirstHit(a.first, a.second, b.first, b.second, 0.f), 0.375f, 1e-4f);
	}

	// 길이 방향으로 나란히 - 면에서 5 cm 밖은 통과, 5 cm 안은 끝 면(로컬 l = -200)에서 막힘
	{
		const auto a = world(-300.f, 30.f);
		const auto b = world(300.f, 30.f);
		EXPECT_FALSE(map.SweepBlocked(a.first, a.second, b.first, b.second, 0.f));

		const auto a2 = world(-300.f, 20.f);
		const auto b2 = world(300.f, 20.f);
		EXPECT_NEAR(map.SweepFirstHit(a2.first, a2.second, b2.first, b2.second, 0.f), 100.f / 600.f, 1e-4f);
	}

	// 모서리 바깥 점 - 로컬 (230, 55) 는 모서리 (200, 25) 에서 √(30² + 30²) = 42.43 cm.
	// 캡슐 근사(중심선 ± 200 · 반경 25 + 42)라면 끝점 (200, 0) 에서 62.6 cm < 67 이라 겹친다고 잘못 판정한다 (ADR-0006 Alt 1)
	{
		const auto p = world(230.f, 55.f);
		EXPECT_FALSE(map.Overlaps(p.first, p.second, 42.f));
		EXPECT_TRUE(map.Overlaps(p.first, p.second, 43.f));
	}
}

TEST(WallMap, yaw_90은_길이와_두께를_바꾼_yaw_0과_같다)
{
	const WallMap turned = Build({ Box(0.f, 0.f, 200.f, 25.f, 90.f) });
	const WallMap swapped = Build({ Box(0.f, 0.f, 25.f, 200.f, 0.f) });

	for (float x = -260.f; x <= 260.f; x += 20.f)
	{
		for (float y = -260.f; y <= 260.f; y += 20.f)
		{
			EXPECT_EQ(turned.Overlaps(x, y, 0.f), swapped.Overlaps(x, y, 0.f)) << x << "," << y;
			EXPECT_EQ(turned.Overlaps(x, y, 42.f), swapped.Overlaps(x, y, 42.f)) << x << "," << y;
			EXPECT_NEAR(turned.SweepFirstHit(x, y, -x * 0.5f, 300.f - y, 42.f), swapped.SweepFirstHit(x, y, -x * 0.5f, 300.f - y, 42.f), 1e-6f);
		}
	}
}

TEST(WallMap, 벽_안에서_시작하면_더_깊이_들어갈_때만_막는다)
{
	// AC-LM-04 "이미 벽 안이면 침투 깊이가 같거나 줄 때만 수락" · level-map.md § Edge Cases
	const WallMap map = Build({ Box(0.f, 0.f, 200.f, 25.f, 0.f), Box(0.f, -100.f, 200.f, 25.f, 0.f) });

	EXPECT_FALSE(map.SweepBlocked(0.f, 10.f, 0.f, 20.f, 0.f));		// 깊이 15 → 5 - 나간다
	EXPECT_FALSE(map.SweepBlocked(0.f, 10.f, 50.f, 10.f, 0.f));		// 15 → 15 - 같다
	EXPECT_FLOAT_EQ(map.SweepFirstHit(0.f, 10.f, 0.f, 0.f, 0.f), 0.f);	// 15 → 25 - 파고든다

	// 반대쪽 면으로 빠져나가는 것도 나가는 것이다 (ADR-0006 § ②)
	EXPECT_FALSE(map.SweepBlocked(0.f, 10.f, 0.f, -40.f, 0.f));

	// 빠져나와 다른 벽으로 들어가면 그 벽에서 막힌다 - y = -75 에서 = 85 / 110
	EXPECT_NEAR(map.SweepFirstHit(0.f, 10.f, 0.f, -100.f, 0.f), 85.f / 110.f, 1e-5f);
}

TEST(WallMap, 벽에_붙은_채_벽_쪽으로_밀리면_그_자리에서_막힌다)
{
	// 넉백 r = 42 - 면에서 정확히 42 cm (닿아 있음)
	const WallMap map = OneWall();

	EXPECT_FLOAT_EQ(map.SweepFirstHit(0.f, -67.f, 0.f, -17.f, 42.f), 0.f);		// 벽 쪽 - 즉시 벽 충돌
	EXPECT_FLOAT_EQ(map.SweepFirstHit(0.f, -67.f, 0.f, -167.f, 42.f), 1.f);		// 반대쪽 - 자유
	EXPECT_FLOAT_EQ(map.SweepFirstHit(-100.f, -67.f, 100.f, -67.f, 42.f), 1.f);	// 나란히 - 모서리 원에 스치기만 한다

	// 오차로 0.05 cm 겹친 채 시작해도 같다
	EXPECT_FLOAT_EQ(map.SweepFirstHit(0.f, -66.95f, 0.f, -17.f, 42.f), 0.f);
	EXPECT_FLOAT_EQ(map.SweepFirstHit(0.f, -66.95f, 0.f, -167.f, 42.f), 1.f);
}

TEST(WallMap, 벽이_없으면_어디로든_간다)
{
	const WallMap unbuilt;
	EXPECT_FLOAT_EQ(unbuilt.SweepFirstHit(-14000.f, -14000.f, 14000.f, 14000.f, 42.f), 1.f);
	EXPECT_FALSE(unbuilt.Overlaps(0.f, 0.f, 900.f));

	const WallMap empty = Build({});
	EXPECT_EQ(empty.Count(), 0);
	EXPECT_FALSE(empty.SweepBlocked(0.f, 0.f, 100.f, 0.f, 0.f));
}

TEST(WallMap, NaN_좌표는_막힌_것으로_본다)
{
	// 판정할 수 없다 - 이동 검증은 FloorBounds 가 먼저 거르지만, 다른 호출자가 거르지 않아도 통과시키지 않는다
	const WallMap map = OneWall();
	const float nan = numeric_limits<float>::quiet_NaN();
	const float inf = numeric_limits<float>::infinity();

	EXPECT_FLOAT_EQ(map.SweepFirstHit(nan, 0.f, 100.f, 0.f, 0.f), 0.f);
	EXPECT_FLOAT_EQ(map.SweepFirstHit(0.f, 0.f, inf, 0.f, 0.f), 0.f);
	EXPECT_TRUE(map.Overlaps(nan, 0.f, 0.f));
}

/*-------- 격자 = 전수 --------*/

TEST(WallMap, 격자_결과는_전수_검사와_같다)
{
	// 시드 고정 무작위 질의 10,000건. 층 밖 시작점 · 층을 가로지르는 긴 선분 · 반지름 0 / 42 / 150 을 섞는다
	const WallMap map = Build(RandomWalls(WallMap::MAX_WALLS, 20261007));
	ASSERT_EQ(map.Count(), WallMap::MAX_WALLS);

	mt19937 rng(7);
	uniform_real_distribution<float> start(-14500.f, 14500.f);
	uniform_real_distribution<float> angle(0.f, 6.2831853f);
	uniform_real_distribution<float> coin(0.f, 1.f);
	const float radii[] = { 0.f, 42.f, 150.f };

	int32 sweepMismatch = 0;
	int32 overlapMismatch = 0;
	int32 depthMismatch = 0;
	int32 hits = 0;

	for (int32 i = 0; i < 10000; i++)
	{
		const float ax = start(rng);
		const float ay = start(rng);
		const float roll = coin(rng);
		const float length = (roll < 0.70f) ? coin(rng) * 300.f : (roll < 0.95f) ? coin(rng) * 2000.f : coin(rng) * 30000.f;
		const float a = angle(rng);
		const float bx = ax + length * std::cos(a);
		const float by = ay + length * std::sin(a);
		const float r = radii[i % 3];

		const float t = map.SweepFirstHit(ax, ay, bx, by, r);
		if (t != BruteSweep(map, ax, ay, bx, by, r))
			sweepMismatch++;
		if (t < 1.f)
			hits++;

		if (map.Overlaps(ax, ay, r) != BruteOverlaps(map, ax, ay, r))
			overlapMismatch++;

		if (map.PenetrationDepth(bx, by) != BrutePenetration(map, bx, by))
			depthMismatch++;
	}

	EXPECT_EQ(sweepMismatch, 0);
	EXPECT_EQ(overlapMismatch, 0);
	EXPECT_EQ(depthMismatch, 0);
	EXPECT_GT(hits, 300);		// 의미 있는 비교였다 - 거의 다 빗나가면 같아도 소용없다
}

TEST(WallMap, 1층_황금_벽에서도_격자_결과는_전수_검사와_같다)
{
	// 실제 레벨 771개 - 집 3채에 몰려 있다. 질의도 집이 있는 띠에 몰아 빽빽한 곳을 본다
	string error;
	ASSERT_TRUE(LoadFloor1(OUT error)) << error;
	const WallMap& map = WallTable::Get(0);

	mt19937 rng(11);
	uniform_real_distribution<float> houseX(-4000.f, 2400.f);
	uniform_real_distribution<float> houseY(-11600.f, 11600.f);
	uniform_real_distribution<float> anyXY(-14000.f, 14000.f);
	uniform_real_distribution<float> angle(0.f, 6.2831853f);
	uniform_real_distribution<float> coin(0.f, 1.f);
	const float radii[] = { 0.f, 42.f };

	int32 mismatch = 0;
	int32 hits = 0;

	for (int32 i = 0; i < 10000; i++)
	{
		const bool inHouses = coin(rng) < 0.8f;
		const float ax = inHouses ? houseX(rng) : anyXY(rng);
		const float ay = inHouses ? houseY(rng) : anyXY(rng);
		const float a = angle(rng);
		const float length = coin(rng) * ((coin(rng) < 0.9f) ? 300.f : 3000.f);
		const float bx = ax + length * std::cos(a);
		const float by = ay + length * std::sin(a);
		const float r = radii[i % 2];

		const float t = map.SweepFirstHit(ax, ay, bx, by, r);
		if (t != BruteSweep(map, ax, ay, bx, by, r) || map.Overlaps(ax, ay, r) != BruteOverlaps(map, ax, ay, r))
			mismatch++;
		if (t < 1.f)
			hits++;
	}

	EXPECT_EQ(mismatch, 0);
	EXPECT_GT(hits, 1000);
}

/*-------- 만들기 검증 --------*/

TEST(WallMap, 상한을_넘거나_층_밖으로_나가거나_크기가_0이면_만들지_않는다)
{
	string error;
	WallMap map;

	EXPECT_TRUE(map.Build(RandomWalls(WallMap::MAX_WALLS, 1), OUT error)) << error;

	EXPECT_FALSE(map.Build(RandomWalls(WallMap::MAX_WALLS + 1, 1), OUT error));
	EXPECT_NE(error.find("limit"), string::npos) << error;
	EXPECT_EQ(map.Count(), 0);

	// 중심은 안이지만 끝이 층 밖 (13,990 + 20 > 14,000)
	EXPECT_FALSE(map.Build({ Box(13990.f, 0.f, 20.f, 5.f, 0.f) }, OUT error));
	EXPECT_NE(error.find("outside"), string::npos) << error;

	// 회전하면 외접 사각형이 커진다 - 45° 의 4 m 벽은 중심에서 약 159 cm 까지 뻗는다
	EXPECT_TRUE(map.Build({ Box(13800.f, 0.f, 200.f, 25.f, 0.f) }, OUT error)) << error;
	EXPECT_FALSE(map.Build({ Box(13900.f, 0.f, 200.f, 25.f, 45.f) }, OUT error));

	EXPECT_FALSE(map.Build({ Box(0.f, 0.f, 0.f, 25.f, 0.f) }, OUT error));
	EXPECT_NE(error.find("> 0"), string::npos) << error;

	EXPECT_FALSE(map.Build({ Box(numeric_limits<float>::quiet_NaN(), 0.f, 200.f, 25.f, 0.f) }, OUT error));
	EXPECT_NE(error.find("non-finite"), string::npos) << error;
	EXPECT_EQ(map.Count(), 0);
}

/*-------- WallTable --------*/

namespace
{
	const char* WALLS_CSV =
		"floor_id,kind,cx,cy,half_length,half_thickness,yaw\n"
		"0,wall,2000,0,200,25,90.00\n"
		"0,pillar,2000,200,25,25,0.00\n"
		"0,tower,-7800,0,150,150,0.00\n";

	const char* VERSIONS_CSV =
		"floor_id,data_version\n"
		"0,3a2fb2565b83\n";

	vector<EntryPoint> Entries(float x, float y)
	{
		EntryPoint e;
		e.entryId = 1;
		e.floorId = 0;
		e.x = x;
		e.y = y;
		return { e };
	}

	bool ParseWalls(const string& walls, OUT string& error)
	{
		const string versions = VERSIONS_CSV;
		return WallTable::Parse(&walls, &versions, 2, Entries(0.f, 0.f), OUT error);
	}
}

TEST(WallTable, 층별로_읽고_레벨이_없는_층은_빈_맵이다)
{
	const string walls = WALLS_CSV;
	const string versions = VERSIONS_CSV;
	string error;

	ASSERT_TRUE(WallTable::Parse(&walls, &versions, 2, Entries(0.f, 0.f), OUT error)) << error;

	EXPECT_EQ(WallTable::Get(0).Count(), 3);
	EXPECT_EQ(WallTable::Get(1).Count(), 0);
	EXPECT_EQ(WallTable::Get(7).Count(), 0);			// 범위 밖 층도 빈 맵
	EXPECT_EQ(WallTable::DataVersion(0), "3a2fb2565b83");
	EXPECT_EQ(WallTable::DataVersion(1), "");
	EXPECT_EQ(WallTable::Get(0).Walls()[2].kind, WallKind::Tower);

	// yaw 90 - 길이가 Y 방향. x = 2000 ± 25 를 가로지르는 이동이 막힌다
	WallKind kind = WallKind::Tower;
	EXPECT_TRUE(WallTable::Get(0).SweepBlocked(1900.f, 0.f, 2100.f, 0.f, 0.f, OUT &kind));
	EXPECT_EQ(kind, WallKind::Wall);
	EXPECT_FALSE(WallTable::Get(1).SweepBlocked(1900.f, 0.f, 2100.f, 0.f, 0.f));
}

TEST(WallTable, 파일이_없으면_전층_벽_0개로_부팅한다)
{
	// 지금 Server/Data 상태 - 레벨이 아직 서버에 올라가지 않았다 (ADR-0006 § Migration 1)
	string error;
	ASSERT_TRUE(WallTable::Parse(nullptr, nullptr, 4, Entries(0.f, 0.f), OUT error)) << error;

	for (uint32 floor = 0; floor < 4; floor++)
	{
		EXPECT_EQ(WallTable::Get(floor).Count(), 0);
		EXPECT_EQ(WallTable::DataVersion(floor), "");
	}
}

TEST(WallTable, 잘못된_행은_줄_번호와_함께_부팅을_거부한다)
{
	const string header = "floor_id,kind,cx,cy,half_length,half_thickness,yaw\n";
	string error;

	EXPECT_FALSE(ParseWalls(header + "0,door,0,5000,200,25,0.00\n", OUT error));
	EXPECT_NE(error.find("walls.csv:2"), string::npos) << error;
	EXPECT_NE(error.find("kind"), string::npos) << error;

	EXPECT_FALSE(ParseWalls(header + "0,wall,0,5000,200,25,180.00\n", OUT error));
	EXPECT_NE(error.find("yaw"), string::npos) << error;

	EXPECT_FALSE(ParseWalls(header + "0,wall,0,5000,0,25,0.00\n", OUT error));
	EXPECT_NE(error.find("> 0"), string::npos) << error;

	EXPECT_FALSE(ParseWalls(header + "0,wall,nan,5000,200,25,0.00\n", OUT error));
	EXPECT_NE(error.find("finite"), string::npos) << error;

	EXPECT_FALSE(ParseWalls(header + "9,wall,0,5000,200,25,0.00\n", OUT error));
	EXPECT_NE(error.find("floor_id"), string::npos) << error;

	EXPECT_FALSE(ParseWalls("floor_id,kind,cx,cy,half_length,yaw\n0,wall,0,5000,200,0.00\n", OUT error));
	EXPECT_NE(error.find("half_thickness"), string::npos) << error;

	// 실패하면 이전에 읽은 벽도 남기지 않는다
	EXPECT_EQ(WallTable::Get(0).Count(), 0);
}

TEST(WallTable, 레벨이_있는_층에_벽이_없으면_부팅을_거부한다)
{
	// level_versions 에 1층이 있는데 벽은 0층뿐 - 파일이 잘려 벽이 조용히 꺼지는 경우
	const string walls = WALLS_CSV;
	const string versions = "floor_id,data_version\n0,3a2fb2565b83\n1,0123456789ab\n";
	string error;

	EXPECT_FALSE(WallTable::Parse(&walls, &versions, 2, Entries(0.f, 0.f), OUT error));
	EXPECT_NE(error.find("floor 1"), string::npos) << error;
}

TEST(WallTable, 진입_지점_9m_안에_벽이_있으면_부팅을_거부한다)
{
	// level-map.md Core Rule 15 - 벽은 x = 1975 .. 2025, y = -200 .. 200
	const string walls = WALLS_CSV;
	const string versions = VERSIONS_CSV;
	string error;

	EXPECT_TRUE(WallTable::Parse(&walls, &versions, 2, Entries(1000.f, 0.f), OUT error)) << error;	// 975 cm

	EXPECT_FALSE(WallTable::Parse(&walls, &versions, 2, Entries(1100.f, 0.f), OUT error));			// 875 cm
	EXPECT_NE(error.find("entry point 1"), string::npos) << error;
}

TEST(WallTable, 1층_황금_벽이_부팅_검사를_통과한다)
{
	// 실제 레벨 - 진입 지점 9곳 모두 9 m 안에 벽이 없어야 서버가 뜬다 (Core Rule 15 · ADR-0006 § Migration 2)
	string error;
	ASSERT_TRUE(LoadFloor1(OUT error)) << error;

	const WallMap& map = WallTable::Get(0);
	EXPECT_EQ(map.Count(), 771);		// 벽 309 · 문 기둥 58 · 격자점 기둥 328 · 독립 기둥 4 · 엄폐 69 · 타워 3 (floor-1.md § 8)
	EXPECT_EQ(WallTable::DataVersion(0), "b716c8a5e4af");

	int32 perKind[6] = {};
	for (const WallBox& w : map.Walls())
		perKind[static_cast<int32>(w.kind)]++;

	EXPECT_EQ(perKind[static_cast<int32>(WallKind::Wall)], 309);
	EXPECT_EQ(perKind[static_cast<int32>(WallKind::DoorPost)], 58);
	EXPECT_EQ(perKind[static_cast<int32>(WallKind::Pillar)], 328);
	EXPECT_EQ(perKind[static_cast<int32>(WallKind::FreePillar)], 4);
	EXPECT_EQ(perKind[static_cast<int32>(WallKind::Cover)], 69);
	EXPECT_EQ(perKind[static_cast<int32>(WallKind::Tower)], 3);

	// 타워 Tc (-78, 0) m 본체를 가로지르는 이동은 막힌다 - 원기둥이 아니라 3 x 3 m 상자다
	WallKind kind = WallKind::Wall;
	EXPECT_TRUE(map.SweepBlocked(-7800.f, -500.f, -7800.f, 500.f, 0.f, OUT &kind));
	EXPECT_EQ(kind, WallKind::Tower);
}

TEST(WallTable, 실제_데이터_파일이_부팅_검사를_통과한다)
{
	// Server/Data - 서버가 실제로 읽는 파일. 레벨이 올라가기 전에는 walls.csv 가 없다
	string error;
	ASSERT_TRUE(EntryPointTable::Load(ItemTable::DefaultDataDir(), 4, OUT error)) << error;
	EXPECT_TRUE(WallTable::Load(ItemTable::DefaultDataDir(), 4, EntryPointTable::All(), OUT error)) << error;
}

/*-------- 성능 (게이트 아님 - 직접 실행) --------*/

// GameServerTests.exe --gtest_also_run_disabled_tests --gtest_filter=WallMapBench.*
// AC-LM-50 마이크로 벤치: 이동 1회 벽 검사 평균 ≤ 20 µs · 최악 ≤ 100 µs. Release 로 잴 것.
TEST(WallMapBench, DISABLED_이동_1회_벽_검사_시간)
{
	struct Case
	{
		const char*		name;
		vector<WallBox>	walls;
		float			x0, x1, y0, y1;		// 이동 시작점을 뽑는 범위 - 벽이 가장 빽빽한 곳
	};

	vector<Case> cases;

	string error;
	if (LoadFloor1(OUT error))
		cases.push_back({ "floor-1 (golden)", WallTable::Get(0).Walls(), -3600.f, 2000.f, -11200.f, 11200.f });
	else
		ADD_FAILURE() << error;

	cases.push_back({ "floor-1-like", HouseLikeWalls(11), -3600.f, 2000.f, -11200.f, 11200.f });
	cases.push_back({ "synthetic-2000", RandomWalls(WallMap::MAX_WALLS, 20261007), -13000.f, 13000.f, -13000.f, 13000.f });

	for (Case& c : cases)
	{
		const int32 wallCount = static_cast<int32>(c.walls.size());
		const WallMap map = Build(move(c.walls));

		mt19937 rng(3);
		uniform_real_distribution<float> px(c.x0, c.x1);
		uniform_real_distribution<float> py(c.y0, c.y1);
		uniform_real_distribution<float> angle(0.f, 6.2831853f);
		uniform_real_distribution<float> length(0.f, 290.f);		// 속도 검사가 묶는 한 번 이동 (510 × 1.15 × 0.5)

		constexpr int32 N = 200000;
		vector<double> us;
		us.reserve(N);
		int32 blocked = 0;

		for (int32 i = 0; i < N; i++)
		{
			const float ax = px(rng);
			const float ay = py(rng);
			const float a = angle(rng);
			const float l = length(rng);

			const float bx = ax + l * std::cos(a);
			const float by = ay + l * std::sin(a);

			// 같은 질의를 3번 재 가장 짧은 것 - 한 번만 재면 OS 선점 · 인터럽트가 max 에 섞여 실행마다 45 - 580 µs 로
			// 흔들렸다 (2026-10-07). 같은 입력의 계산량은 같으므로 최솟값이 이 질의의 실제 비용이다
			bool hit = false;
			double best = 1e9;
			for (int32 rep = 0; rep < 3; rep++)
			{
				const auto t0 = chrono::steady_clock::now();
				hit = map.SweepBlocked(ax, ay, bx, by, 0.f);
				const auto t1 = chrono::steady_clock::now();
				best = (std::min)(best, chrono::duration<double, micro>(t1 - t0).count());
			}

			us.push_back(best);
			blocked += hit ? 1 : 0;
		}

		double sum = 0.0;
		for (const double v : us)
			sum += v;

		vector<double> sorted = us;
		sort(sorted.begin(), sorted.end());
		const double avg = sum / N;
		const double p99 = sorted[static_cast<size_t>(N * 0.99)];
		const double p999 = sorted[static_cast<size_t>(N * 0.999)];
		const double maxUs = sorted.back();

		printf("[WALL BENCH] %s walls=%d moves=%d blocked=%d avg=%.3fus p99=%.3fus p99.9=%.3fus max=%.3fus\n",
			c.name, wallCount, N, blocked, avg, p99, p999, maxUs);

		EXPECT_LE(avg, 20.0) << c.name;
		EXPECT_LE(maxUs, 100.0) << c.name;
	}
}
