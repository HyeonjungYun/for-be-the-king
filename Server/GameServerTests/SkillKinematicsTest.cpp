#include "pch.h"
#include "SkillKinematics.h"
#include "FloorBounds.h"
#include "WallMap.h"

// 서버가 옮기는 변위 - 넉백 · 끌기 (design/quick-specs/skill-roster-2026-10-10.md 1 단계).
// 사용자 결정: 끌기는 벽에 막히고 시전자 바로 앞까지 · 넉백은 벽에 닿으면 멈춘다 (벽 기절은 Room)

namespace
{
	using namespace SkillKinematics;

	WallBox Box(float cx, float cy, float halfLength, float halfThickness, float yawDeg)
	{
		WallBox w;
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

	// 벽 없는 층
	WallMap Empty() { return Build({}); }

	// x = 500 에 선 벽 - 두께 50, Y 방향으로 4 m (로컬 X 축 = +Y)
	WallMap WallAtX500() { return Build({ Box(500.f, 0.f, 200.f, 25.f, 90.f) }); }
}

TEST(SkillKinematics, 벽이_없으면_그_거리만큼_간다)
{
	const WallMap map = Empty();
	const PushResult r = Push(map, 100.f, 100.f, 1.f, 0.f, 300.f);

	EXPECT_TRUE(r.moved);
	EXPECT_FALSE(r.hitWall);
	EXPECT_FLOAT_EQ(r.endX, 400.f);
	EXPECT_FLOAT_EQ(r.endY, 100.f);
	EXPECT_FLOAT_EQ(r.distCm, 300.f);
}

TEST(SkillKinematics, 방향은_정규화되지_않아도_된다)
{
	const WallMap map = Empty();
	const PushResult r = Push(map, 0.f, 0.f, 0.f, 10.f, 200.f);

	EXPECT_FLOAT_EQ(r.endX, 0.f);
	EXPECT_FLOAT_EQ(r.endY, 200.f);
}

TEST(SkillKinematics, 벽에_닿으면_그_앞에서_멈추고_벽에_겹치지_않는다)
{
	const WallMap map = WallAtX500();

	// 벽 면은 x = 475. 캡슐 반지름 42 → 중심은 433 근처에서 멈춘다
	const PushResult r = Push(map, 200.f, 0.f, 1.f, 0.f, 300.f);

	EXPECT_TRUE(r.moved);
	EXPECT_TRUE(r.hitWall);
	EXPECT_LT(r.distCm, 300.f);
	EXPECT_GT(r.distCm, 200.f);
	EXPECT_NEAR(r.endX, 475.f - CAPSULE_RADIUS_CM - WALL_BACKOFF_CM, 2.f);
	EXPECT_FALSE(map.Overlaps(r.endX, r.endY, CAPSULE_RADIUS_CM));
}

TEST(SkillKinematics, 벽에_붙어_있으면_움직이지_않고_벽_충돌로_센다)
{
	const WallMap map = WallAtX500();
	const PushResult r = Push(map, 475.f - CAPSULE_RADIUS_CM, 0.f, 1.f, 0.f, 300.f);

	EXPECT_FALSE(r.moved);
	EXPECT_TRUE(r.hitWall);
	EXPECT_FLOAT_EQ(r.distCm, 0.f);
}

TEST(SkillKinematics, 벽과_나란히_밀리면_막히지_않는다)
{
	const WallMap map = WallAtX500();
	const PushResult r = Push(map, 300.f, -100.f, 0.f, 1.f, 300.f);

	EXPECT_TRUE(r.moved);
	EXPECT_FALSE(r.hitWall);
	EXPECT_FLOAT_EQ(r.distCm, 300.f);
}

TEST(SkillKinematics, 층_경계_안쪽에서_잘린다)
{
	const WallMap map = Empty();
	const float x = FloorBounds::HALF_CM - 100.f;
	const PushResult r = Push(map, x, 0.f, 1.f, 0.f, 300.f);

	EXPECT_TRUE(r.moved);
	EXPECT_FALSE(r.hitWall);
	EXPECT_FLOAT_EQ(r.endX, FloorBounds::HALF_CM - CAPSULE_RADIUS_CM);
	EXPECT_FLOAT_EQ(r.distCm, 100.f - CAPSULE_RADIUS_CM);
	EXPECT_TRUE(FloorBounds::Contains(r.endX, r.endY));
}

TEST(SkillKinematics, 거리_0_이나_층_밖에서는_아무것도_하지_않는다)
{
	const WallMap map = Empty();

	EXPECT_FALSE(Push(map, 0.f, 0.f, 1.f, 0.f, 0.f).moved);
	EXPECT_FALSE(Push(map, 0.f, 0.f, 0.f, 0.f, 300.f).moved);
	EXPECT_FALSE(Push(map, FloorBounds::HALF_CM + 10.f, 0.f, -1.f, 0.f, 300.f).moved);
}

// ── 끌기 ──

TEST(SkillKinematics, 끌기는_시전자_바로_앞에서_멈춘다)
{
	const WallMap map = Empty();
	const PushResult r = PullToward(map, 600.f, 0.f, 0.f, 0.f, 100.f);

	EXPECT_TRUE(r.moved);
	EXPECT_FALSE(r.hitWall);
	EXPECT_NEAR(r.endX, 100.f, 0.01f);
	EXPECT_NEAR(r.endY, 0.f, 0.01f);
	EXPECT_NEAR(r.distCm, 500.f, 0.01f);
}

TEST(SkillKinematics, 이미_가까우면_끌리지_않는다)
{
	const WallMap map = Empty();
	const PushResult r = PullToward(map, 80.f, 0.f, 0.f, 0.f, 100.f);

	EXPECT_FALSE(r.moved);
	EXPECT_FALSE(r.hitWall);
	EXPECT_FLOAT_EQ(r.endX, 80.f);
}

TEST(SkillKinematics, 사이에_벽이_있으면_끌리지_않는다)
{
	const WallMap map = WallAtX500();

	// 대상 x = 800, 시전자 x = 200 - 벽(x = 500)이 가운데
	const PushResult r = PullToward(map, 800.f, 0.f, 200.f, 0.f, 100.f);

	EXPECT_FALSE(r.moved);
	EXPECT_TRUE(r.hitWall);
	EXPECT_FLOAT_EQ(r.endX, 800.f);
}

TEST(SkillKinematics, 축복_끌기는_벽을_지나_시전자_앞에_선다)
{
	const WallMap map = WallAtX500();

	// 대상 x = 800, 시전자 x = 200 - 바로 앞(x 300)은 벽(475~525) 이쪽이다
	const PushResult r = PullToward(map, 800.f, 0.f, 200.f, 0.f, 100.f, true);

	EXPECT_TRUE(r.moved);
	EXPECT_FALSE(r.hitWall);
	EXPECT_NEAR(r.endX, 300.f, 0.01f);
	EXPECT_FALSE(map.Overlaps(r.endX, r.endY, CAPSULE_RADIUS_CM));
}

TEST(SkillKinematics, 축복_끌기의_끝점이_벽_두께_안이면_벽_밖으로_옮긴다)
{
	const WallMap map = WallAtX500();

	// 시전자 x = 380 (벽 면 475 에서 95 cm) → 바로 앞 x 480 은 벽 안. 이쪽엔 몸 둘이 들어갈 자리가 없다 (95 - 42 < 84)
	// → 벽 저편 면(525 + 42 = 567) 밖으로. 2026-10-11 실기동에서 몸이 벽에 박혀 멈췄다
	const PushResult r = PullToward(map, 800.f, 0.f, 380.f, 0.f, 100.f, true);

	EXPECT_TRUE(r.moved);
	EXPECT_FALSE(map.Overlaps(r.endX, r.endY, CAPSULE_RADIUS_CM)) << "end x=" << r.endX;
	EXPECT_GE(r.endX, 525.f + CAPSULE_RADIUS_CM);
	EXPECT_LT(r.endX, 800.f);

	// 시전자가 벽에서 충분히 떨어져 있으면(몸 둘 + 바로 앞 자리) 이쪽으로 넘어온다
	const PushResult across = PullToward(map, 800.f, 0.f, 300.f, 0.f, 100.f, true);
	EXPECT_FALSE(map.Overlaps(across.endX, across.endY, CAPSULE_RADIUS_CM)) << "end x=" << across.endX;
	EXPECT_LE(across.endX, 475.f - CAPSULE_RADIUS_CM);
}

TEST(SkillKinematics, 날아가는_시간은_거리_나누기_속도다)
{
	EXPECT_EQ(TravelMs(300.f, 800.f), 375u);
	EXPECT_EQ(TravelMs(500.f, 1600.f), 313u);		// 312.5 → 올림
	EXPECT_EQ(TravelMs(0.f, 800.f), 0u);
	EXPECT_EQ(TravelMs(300.f, 0.f), 0u);
}

// ── 투사체 (2 단계) - 선분이 원에 닿는 t ──

TEST(SkillKinematics, 투사체_경로가_캡슐을_지나면_처음_닿는_비율을_준다)
{
	// (0,0) → (400,0), 원 중심 (200, 0) 반지름 82 (투사체 40 + 캡슐 42) → 118 에서 닿는다 = t 0.295
	const float t = SegmentCircleHit(0.f, 0.f, 400.f, 0.f, 200.f, 0.f, 82.f);
	EXPECT_NEAR(t, 118.f / 400.f, 0.001f);
}

TEST(SkillKinematics, 경로_옆을_스치면_안_닿는다)
{
	EXPECT_FLOAT_EQ(SegmentCircleHit(0.f, 0.f, 400.f, 0.f, 200.f, 100.f, 82.f), NO_HIT);
	EXPECT_LT(SegmentCircleHit(0.f, 0.f, 400.f, 0.f, 200.f, 80.f, 82.f), 1.f) << "반지름 안쪽은 닿는다";
}

TEST(SkillKinematics, 뒤에_있거나_너무_멀면_안_닿는다)
{
	EXPECT_FLOAT_EQ(SegmentCircleHit(0.f, 0.f, 400.f, 0.f, -200.f, 0.f, 82.f), NO_HIT) << "뒤";
	EXPECT_FLOAT_EQ(SegmentCircleHit(0.f, 0.f, 400.f, 0.f, 600.f, 0.f, 82.f), NO_HIT) << "이번 스텝 너머";
}

TEST(SkillKinematics, 시작점이_이미_원_안이면_0이고_안_움직이면_안_닿는다)
{
	EXPECT_FLOAT_EQ(SegmentCircleHit(0.f, 0.f, 400.f, 0.f, 30.f, 0.f, 82.f), 0.f);
	EXPECT_FLOAT_EQ(SegmentCircleHit(0.f, 0.f, 0.f, 0.f, 200.f, 0.f, 82.f), NO_HIT);
}
