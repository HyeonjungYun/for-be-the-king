#include "pch.h"
#include "NavGrid.h"
#include "WallMap.h"
#include "WallTable.h"
#include "EntryPointTable.h"
#include "ItemTable.h"
#include "MonsterBrain.h"
#include "MonsterTable.h"
#include "SkillTable.h"
#include <chrono>
#include <cmath>
#include <random>

// design/gdd/monster-ai.md § K 길찾기 · § L 몬스터 스킬 · 원거리 (2026-10-17). 이름 끝의 AC 번호가 그 행이다.
// NavGrid · MonsterMover · MonsterSkillUse 는 Room 을 모르는 순수 로직 - 합성 벽 · 주입 시계로 돌린다.

namespace
{
	constexpr uint64 MS = 1000;
	constexpr uint64 SEC = 1'000'000;

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

	// x = 0 에 남북으로 선 긴 벽 (y -1500 .. 1500, 두께 1 m) - 서쪽에서 동쪽으로 가려면 끝을 돌아야 한다
	WallMap LongWall()
	{
		return Build({ Box(0.f, 0.f, 1500.f, 50.f, 90.f) });
	}

	float Dist(float ax, float ay, float bx, float by)
	{
		return ::sqrtf((ax - bx) * (ax - bx) + (ay - by) * (ay - by));
	}

	NavSweepFn SweepOf(const WallMap& walls)
	{
		return [&walls](float ax, float ay, float bx, float by, float r) { return walls.SweepBlocked(ax, ay, bx, by, r); };
	}

	// 길의 모든 선분(출발점 → 첫 점 → ...)이 몸 반지름으로 벽에 닿지 않는다
	void ExpectPathClear(const WallMap& walls, float fromX, float fromY, const NavGrid::PathResult& r, float radius)
	{
		float x = fromX;
		float y = fromY;
		for (const auto& p : r.points)
		{
			EXPECT_FALSE(walls.SweepBlocked(x, y, p.first, p.second, radius))
				<< "segment (" << x << "," << y << ") -> (" << p.first << "," << p.second << ") r=" << radius;
			x = p.first;
			y = p.second;
		}
	}

	MonsterTypeDef Archer()
	{
		MonsterTypeDef t;
		t.typeId = 8;
		t.nameKey = "monster.husk_archer";
		t.tier = MonsterTier::House;
		t.maxHp = 200;
		t.attackPower = 14;
		t.attackRangeCm = 150.f;
		t.moveSpeedCms = 260.f;
		t.radiusCm = 45.f;
		t.aggroRadiusCm = 700.f;
		t.leashRadiusCm = 1400.f;
		t.searchS = 3.f;
		t.giveUpS = 8.f;
		t.kiteMinCm = 400.f;
		t.keepRangeCm = 650.f;
		return t;
	}

	// 잠금이 풀린 채 목표 7 을 쫓는 두뇌
	MonsterBrainState Chasing(uint64 now)
	{
		MonsterBrainState s;
		MonsterBrain::Reset(s, now - 2 * SEC);
		MonsterBrain::Alert(s, 7, now - 100 * MS);
		return s;
	}

	BrainInput In(uint64 now, const vector<BrainPlayer>& players, bool clear = true)
	{
		BrainInput in;
		in.nowUs = now;
		in.players = &players;
		in.lineClear = [clear](float, float, float, float) { return clear; };
		return in;
	}

	SkillDef ShapeSkill(SkillShape shape, float rangeCm, float radiusCm)
	{
		SkillDef d;
		d.skillId = 9999;
		d.shape = shape;
		d.rangeCm = rangeCm;
		d.radiusCm = radiusCm;
		return d;
	}
}

// ── § K 길찾기 ───────────────────────────────────────────────

TEST(NavGrid, 여유는_가장_가까운_벽까지_거리다_AC_MA_90)
{
	const WallMap walls = LongWall();
	NavGrid nav;
	ASSERT_TRUE(nav.Build(walls));

	// 칸 중심 기준 (1 m 칸) - 벽 면(x = 50)에서 칸 중심 x = 250 은 200 cm
	EXPECT_NEAR(nav.ClearanceAt(250.f, 0.f), 200.f, 1.f);
	EXPECT_NEAR(nav.ClearanceAt(-250.f, 0.f), 200.f, 1.f);
	EXPECT_EQ(nav.ClearanceAt(0.f, 0.f), 0.f);								// 벽 안
	EXPECT_EQ(nav.ClearanceAt(5000.f, 5000.f), NavGrid::CLEARANCE_CAP_CM);		// 먼 곳은 상한

	// 몸 45 는 여유 55 가 필요하다 - x = 150 (여유 100) 은 되고 x = 50 (벽 면) 은 안 된다
	EXPECT_TRUE(nav.Walkable(150.f, 0.f, 45.f));
	EXPECT_FALSE(nav.Walkable(50.f, 0.f, 45.f));

	// 벽 없는 층은 격자를 만들지 않는다 (막을 것이 없다 - 직선 그대로)
	NavGrid empty;
	EXPECT_FALSE(empty.Build(WallMap()));
	EXPECT_FALSE(empty.IsBuilt());
}

TEST(NavGrid, 긴_벽_끝을_돌아가는_길_모든_선분이_트였다_AC_MA_91)
{
	const WallMap walls = LongWall();
	NavGrid nav;
	ASSERT_TRUE(nav.Build(walls));

	const NavGrid::PathResult r = nav.FindPath(-1000.f, 0.f, 1000.f, 0.f, 45.f, SweepOf(walls));
	ASSERT_TRUE(r.found) << (r.failReason ? r.failReason : "");
	ASSERT_FALSE(r.points.empty());
	EXPECT_FLOAT_EQ(r.points.back().first, 1000.f);			// 마지막 = 실제 도착점
	EXPECT_FLOAT_EQ(r.points.back().second, 0.f);
	ExpectPathClear(walls, -1000.f, 0.f, r, 45.f);

	// 벽 끝(|y| > 1500)을 돌았다 - 당겨 펴서 꺾는 점이 몇 개 안 된다
	bool aroundEnd = false;
	for (const auto& p : r.points)
		aroundEnd |= ::fabsf(p.second) > 1500.f;
	EXPECT_TRUE(aroundEnd);
	EXPECT_LE(r.points.size(), 5u);
	EXPECT_GT(r.expansions, 0);
}

TEST(NavGrid, 트인_직선은_도착점_하나_AC_MA_91)
{
	const WallMap walls = LongWall();
	NavGrid nav;
	ASSERT_TRUE(nav.Build(walls));

	const NavGrid::PathResult r = nav.FindPath(2000.f, -800.f, 2000.f, 800.f, 45.f, SweepOf(walls));
	ASSERT_TRUE(r.found);
	ASSERT_EQ(r.points.size(), 1u);
	EXPECT_FLOAT_EQ(r.points[0].first, 2000.f);
	EXPECT_FLOAT_EQ(r.points[0].second, 800.f);
}

TEST(NavGrid, 좁은_틈은_작은_몸만_지난다_AC_MA_92)
{
	// 남북 벽 두 토막 사이 3 m 틈 (y -150 .. 150 - 집 문틀과 같은 폭). 1 m 칸 중심은 틈 중심에서 50 떨어져 여유 100:
	// 몸 45 (필요 55) 는 틈으로 지나고, 110 (필요 120) 은 못 지나 벽 끝(2150)을 돈다
	const WallMap walls = Build({ Box(0.f, 1150.f, 1000.f, 50.f, 90.f), Box(0.f, -1150.f, 1000.f, 50.f, 90.f) });
	NavGrid nav;
	ASSERT_TRUE(nav.Build(walls));

	const NavGrid::PathResult small = nav.FindPath(-600.f, 0.f, 600.f, 0.f, 45.f, SweepOf(walls));
	ASSERT_TRUE(small.found);
	ExpectPathClear(walls, -600.f, 0.f, small, 45.f);
	for (const auto& p : small.points)
		EXPECT_LT(::fabsf(p.second), 2150.f);				// 틈으로 - 벽 끝까지 가지 않는다

	const NavGrid::PathResult big = nav.FindPath(-600.f, 0.f, 600.f, 0.f, 110.f, SweepOf(walls));
	ASSERT_TRUE(big.found);
	ExpectPathClear(walls, -600.f, 0.f, big, 110.f);
	bool aroundEnd = false;
	for (const auto& p : big.points)
		aroundEnd |= ::fabsf(p.second) > 2150.f;
	EXPECT_TRUE(aroundEnd);

	// 🔴 격자의 한계 - 2 m 틈은 몸 45 가 실제로는 지나가지만(지름 90) 칸 중심의 여유가 50 이라 격자는 막혔다고 본다.
	// 그때는 길 없이 예전처럼 직선 + 미끄러짐 (§ K Edge Cases)
	const WallMap narrow = Build({ Box(0.f, 1100.f, 1000.f, 50.f, 90.f), Box(0.f, -1100.f, 1000.f, 50.f, 90.f) });
	NavGrid narrowNav;
	ASSERT_TRUE(narrowNav.Build(narrow));
	EXPECT_FALSE(narrowNav.Walkable(0.f, 50.f, 45.f));
}

TEST(NavGrid, 갇힌_목표_창_밖은_실패한다_AC_MA_93)
{
	// 목표를 사방 벽으로 가둔다 (안쪽 4 m 네모)
	const WallMap walls = Build({
		Box(0.f, 250.f, 300.f, 50.f, 0.f), Box(0.f, -250.f, 300.f, 50.f, 0.f),
		Box(250.f, 0.f, 300.f, 50.f, 90.f), Box(-250.f, 0.f, 300.f, 50.f, 90.f) });
	NavGrid nav;
	ASSERT_TRUE(nav.Build(walls));

	const NavGrid::PathResult boxed = nav.FindPath(-3000.f, 0.f, 0.f, 0.f, 45.f, SweepOf(walls));
	EXPECT_FALSE(boxed.found);
	EXPECT_LE(boxed.expansions, NavGrid::MAX_EXPANSIONS + 1);

	// 창(160 m)에 둘이 같이 안 든다 - 바로 실패, 확장 0
	const NavGrid::PathResult far_ = nav.FindPath(-9000.f, 0.f, 9000.f, 0.f, 45.f, SweepOf(walls));
	EXPECT_FALSE(far_.found);
	ASSERT_NE(far_.failReason, nullptr);
	EXPECT_STREQ(far_.failReason, "window");
	EXPECT_EQ(far_.expansions, 0);
}

TEST(NavGrid, 벽에_붙어_출발해도_가까운_칸에서_시작한다_AC_MA_92)
{
	const WallMap walls = LongWall();
	NavGrid nav;
	ASSERT_TRUE(nav.Build(walls));

	// 벽 면(x = -50)에서 몸 45 가 붙어 선 자리 x = -96 - 그 칸은 여유가 모자라 옆 칸에서 시작한다
	const NavGrid::PathResult r = nav.FindPath(-96.f, 0.f, 1000.f, 0.f, 45.f, SweepOf(walls));
	ASSERT_TRUE(r.found) << (r.failReason ? r.failReason : "");
	ExpectPathClear(walls, -96.f, 0.f, r, 45.f);
}

TEST(MonsterMover, 길을_따라_긴_벽_뒤의_목표에_닿는다_AC_MA_94)
{
	const WallMap walls = LongWall();
	NavGrid nav;
	ASSERT_TRUE(nav.Build(walls));

	auto run = [&](bool useNav, OUT float& endX, OUT float& endY, OUT int32& searches, OUT uint64& maxStuckUs)
	{
		MonsterBrainState s;
		float x = -1000.f;
		float y = 0.f;
		uint64 now = 10 * SEC;
		uint32 budget = 4;
		searches = 0;
		maxStuckUs = 0;

		for (int32 i = 0; i < 30 * 20; i++)		// 30 Hz × 20 초
		{
			now += 33 * MS;
			budget = 4;

			StepInput in;
			in.nowUs = now;
			in.dtUs = 33 * MS;
			in.x = x;
			in.y = y;
			in.goalX = 1000.f;
			in.goalY = 0.f;
			in.stopDistCm = 120.f;
			in.speedCms = 300.f;
			in.radiusCm = 40.f;
			in.blocked = SweepOf(walls);
			if (useNav)
			{
				in.nav = &nav;
				in.pathBudget = &budget;
			}

			const StepOutput o = MonsterMover::Step(s, in);
			searches += o.pathSearched ? 1 : 0;
			maxStuckUs = (std::max)(maxStuckUs, s.stuckAccumUs);
			if (o.moved)
			{
				EXPECT_FALSE(walls.SweepBlocked(x, y, o.x, o.y, 40.f)) << "step " << i;
				x = o.x;
				y = o.y;
			}
			if (o.arrived)
				break;
		}
		endX = x;
		endY = y;
	};

	float x = 0.f, y = 0.f;
	int32 searches = 0;
	uint64 maxStuckUs = 0;
	run(true, OUT x, OUT y, OUT searches, OUT maxStuckUs);
	EXPECT_LE(Dist(x, y, 1000.f, 0.f), 121.f) << "end (" << x << "," << y << ")";
	EXPECT_GE(searches, 1);
	EXPECT_LE(searches, 8);						// 목표가 그대로면 PATH_MAX_AGE(2 s)마다 한 번 정도
	EXPECT_LT(maxStuckUs, MonsterConfig::STUCK_US);	// 꺾는 점 쪽으로 늘 나아간다 - 두뇌가 막힘(T3)으로 포기하지 않는다

	// 예전(길찾기 없음) - 벽 면을 따라 목표와 수직으로 미끄러져 진척이 0 이다. 두뇌가 2 초 만에 T3 로 포기하던 자리
	run(false, OUT x, OUT y, OUT searches, OUT maxStuckUs);
	EXPECT_GE(maxStuckUs, MonsterConfig::STUCK_US);
	EXPECT_EQ(searches, 0);
}

TEST(MonsterMover, 예산이_0이면_길을_구하지_않는다_AC_MA_94)
{
	const WallMap walls = LongWall();
	NavGrid nav;
	ASSERT_TRUE(nav.Build(walls));

	MonsterBrainState s;
	uint32 budget = 0;
	StepInput in;
	in.nowUs = 10 * SEC;
	in.dtUs = 33 * MS;
	in.x = -1000.f;
	in.goalX = 1000.f;
	in.speedCms = 300.f;
	in.radiusCm = 40.f;
	in.blocked = SweepOf(walls);
	in.nav = &nav;
	in.pathBudget = &budget;

	const StepOutput o = MonsterMover::Step(s, in);
	EXPECT_FALSE(o.pathSearched);
	EXPECT_TRUE(s.path.empty());
	EXPECT_EQ(budget, 0u);

	// 다음 걸음에 예산이 생기면 구한다 - 1 을 쓴다
	budget = 1;
	in.nowUs += 33 * MS;
	const StepOutput o2 = MonsterMover::Step(s, in);
	EXPECT_TRUE(o2.pathSearched);
	EXPECT_TRUE(o2.pathFound);
	EXPECT_EQ(budget, 0u);
}

TEST(NavGrid, 실제_1층_격자와_길_AC_MA_95)
{
	string error;
	ASSERT_TRUE(EntryPointTable::Load(ItemTable::DefaultDataDir(), 4, OUT error)) << error;
	ASSERT_TRUE(WallTable::Load(ItemTable::DefaultDataDir(), 4, EntryPointTable::All(), OUT error)) << error;
	const WallMap& walls = WallTable::Get(0);
	ASSERT_GT(walls.Count(), 0);

	const auto t0 = chrono::steady_clock::now();
	NavGrid nav;
	ASSERT_TRUE(nav.Build(walls));
	const auto buildMs = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - t0).count();
	printf("[NAV] floor 0 build %lld ms walkable(r45)=%u\n", static_cast<long long>(buildMs), nav.WalkableCells(45.f));
	EXPECT_GT(nav.WalkableCells(45.f), 10000u);

	// 진입 지점마다 곁 30 m 안 무작위 걸을 수 있는 점까지 - 찾은 길은 모두 몸 반지름으로 트였다 (시드 고정)
	mt19937 rng(17);
	uniform_real_distribution<float> off(-3000.f, 3000.f);
	int32 tried = 0, found = 0;
	uint64 worstUs = 0;
	for (const EntryPoint& e : EntryPointTable::All())
	{
		if (e.floorId != 0)
			continue;
		for (int32 k = 0; k < 4; k++)
		{
			const float tx = e.x + off(rng);
			const float ty = e.y + off(rng);
			if (nav.Walkable(tx, ty, 45.f) == false || nav.Walkable(e.x, e.y, 45.f) == false)
				continue;
			tried++;
			const auto s0 = chrono::steady_clock::now();
			const NavGrid::PathResult r = nav.FindPath(e.x, e.y, tx, ty, 45.f, SweepOf(walls));
			worstUs = (std::max)(worstUs, static_cast<uint64>(chrono::duration_cast<chrono::microseconds>(chrono::steady_clock::now() - s0).count()));
			if (r.found)
			{
				found++;
				ExpectPathClear(walls, e.x, e.y, r, 45.f);
			}
		}
	}
	printf("[NAV] floor 0 paths %d/%d worst %llu us\n", found, tried, static_cast<unsigned long long>(worstUs));
	EXPECT_GT(tried, 20);
	EXPECT_GE(found * 10, tried * 9);			// 90 % 이상 - 못 찾는 것은 집 안 막힌 방 · 바다 건너 같은 진짜 단절
}

// ── § L 몬스터 스킬 · 원거리 ──────────────────────────────────

TEST(MonsterSkillUse, 모양마다_쓰는_조건_AC_MA_100)
{
	const vector<BrainPlayer> none;
	auto decide = [](const SkillDef& d, float px, bool clear)
	{
		const vector<BrainPlayer> players{ { 7, px, 0.f } };
		auto lineClear = [clear](float, float, float, float) { return clear; };
		return MonsterSkillUse::Decide(d, 0.f, 0.f, &players[0], players, lineClear);
	};

	// 보스 광역 - 반경 400 - 50 = 350 안의 누구라도. 조준은 발밑
	const SkillDef circle = ShapeSkill(SkillShape::CircleSelf, 400.f, 400.f);
	EXPECT_TRUE(decide(circle, 350.f, true).cast);
	EXPECT_FALSE(decide(circle, 351.f, true).cast);
	EXPECT_FLOAT_EQ(decide(circle, 100.f, true).aimX, 0.f);

	// 돌진 - 250 .. 700 · 시선. 조준 = 목표 자리
	const SkillDef line = ShapeSkill(SkillShape::Line, 700.f, 0.f);
	EXPECT_FALSE(decide(line, 249.f, true).cast);
	EXPECT_TRUE(decide(line, 250.f, true).cast);
	EXPECT_TRUE(decide(line, 700.f, true).cast);
	EXPECT_FALSE(decide(line, 701.f, true).cast);
	EXPECT_FALSE(decide(line, 500.f, false).cast);			// 벽 뒤
	EXPECT_EQ(decide(line, 500.f, true).targetId, 7u);
	EXPECT_FLOAT_EQ(decide(line, 500.f, true).aimX, 500.f);

	// 화살 - 0 .. 사거리 · 시선
	const SkillDef arrow = ShapeSkill(SkillShape::Projectile, 850.f, 0.f);
	EXPECT_TRUE(decide(arrow, 100.f, true).cast);
	EXPECT_TRUE(decide(arrow, 850.f, true).cast);
	EXPECT_FALSE(decide(arrow, 851.f, true).cast);
	EXPECT_FALSE(decide(arrow, 400.f, false).cast);

	// 다른 모양은 쓰지 않는다
	EXPECT_FALSE(decide(ShapeSkill(SkillShape::Cone, 500.f, 300.f), 100.f, true).cast);
	EXPECT_FALSE(MonsterSkillUse::Decide(line, 0.f, 0.f, nullptr, none, nullptr).cast);
}

TEST(MonsterBrain, 원거리는_가까우면_물러나고_keep_에서_선다_AC_MA_101)
{
	const MonsterTypeDef archer = Archer();
	const uint64 now = 100 * SEC;

	// 목표 300 cm (< kite 400) - 반대쪽으로 물러난다. 평타 · 스킬 없음
	{
		MonsterBrainState s = Chasing(now);
		const vector<BrainPlayer> players{ { 7, 300.f, 0.f } };
		const BrainOutput out = MonsterBrain::Think(s, archer, In(now, players));
		EXPECT_TRUE(out.retreating);
		EXPECT_TRUE(out.hasGoal);
		EXPECT_LT(out.goalX, -250.f);						// 서쪽으로 (목표는 동쪽)
		EXPECT_FLOAT_EQ(out.stopDistCm, 0.f);
		EXPECT_EQ(out.attackTargetId, 0u);
		EXPECT_EQ(s.state, MonsterState::Chase);
	}

	// 물러나기는 1.2 초 창 - 창 안에서는 계속, 창이 끝나면 2.5 초는 붙어 있어도 물러나지 않는다 (맞서 쏜다). 쉬는 시간이 끝나면 다시
	{
		MonsterBrainState s = Chasing(now);
		const vector<BrainPlayer> players{ { 7, 300.f, 0.f } };
		EXPECT_TRUE(MonsterBrain::Think(s, archer, In(now, players)).retreating);
		EXPECT_EQ(s.retreatEndUs, now + MonsterConfig::RETREAT_MAX_US);
		EXPECT_TRUE(MonsterBrain::Think(s, archer, In(now + MonsterConfig::RETREAT_MAX_US - 1, players)).retreating);

		const uint64 after = now + MonsterConfig::RETREAT_MAX_US;
		const BrainOutput resting = MonsterBrain::Think(s, archer, In(after, players));
		EXPECT_FALSE(resting.retreating);
		EXPECT_FLOAT_EQ(resting.stopDistCm, 650.f);
		EXPECT_FALSE(MonsterBrain::Think(s, archer, In(after + MonsterConfig::RETREAT_COOLDOWN_US - 1, players)).retreating);
		EXPECT_TRUE(MonsterBrain::Think(s, archer, In(after + MonsterConfig::RETREAT_COOLDOWN_US, players)).retreating);
	}

	// 목표 500 cm (kite .. keep) - 다가가지 않고 그 자리에 선다 (stop = keep 650 이라 걸음이 바로 "닿았다")
	{
		MonsterBrainState s = Chasing(now);
		const vector<BrainPlayer> players{ { 7, 500.f, 0.f } };
		const BrainOutput out = MonsterBrain::Think(s, archer, In(now, players));
		EXPECT_FALSE(out.retreating);
		EXPECT_FLOAT_EQ(out.stopDistCm, 650.f);
		EXPECT_EQ(s.lastEngagedUs, now);				// 멀리서 쏘는 것도 교전 - T2 포기가 오지 않는다
	}

	// 목표 1000 cm - keep 까지 다가간다 (교전 시각은 갱신하지 않는다: 650 × 1.3 = 845 밖)
	{
		MonsterBrainState s = Chasing(now);
		const vector<BrainPlayer> players{ { 7, 1000.f, 0.f } };
		const BrainOutput out = MonsterBrain::Think(s, archer, In(now, players));
		EXPECT_FLOAT_EQ(out.goalX, 1000.f);
		EXPECT_FLOAT_EQ(out.stopDistCm, 650.f);
		EXPECT_LT(s.lastEngagedUs, now);
	}

	// 사방이 막혔다(구석) - 물러나지 못하고 버틴다: 붙은 목표에게 평타
	{
		MonsterBrainState s = Chasing(now);
		const vector<BrainPlayer> players{ { 7, 120.f, 0.f } };
		BrainInput in = In(now, players);
		in.bodyClear = [](float, float, float, float) { return false; };
		const BrainOutput out = MonsterBrain::Think(s, archer, in);
		EXPECT_FALSE(out.retreating);
		EXPECT_EQ(out.attackTargetId, 7u);
	}

	// 끈 안쪽만 - 집에서 1300 cm 서쪽에 선 궁수에게 집 쪽(동쪽)에서 다가오면 물러날 자리가 모두 끈(1400 - 100) 밖이다 → 버틴다
	{
		MonsterBrainState s = Chasing(now);
		const vector<BrainPlayer> players{ { 7, -1000.f, 0.f } };
		BrainInput in = In(now, players);
		in.x = -1300.f;
		const BrainOutput out = MonsterBrain::Think(s, archer, in);
		EXPECT_FALSE(out.retreating);
		EXPECT_EQ(s.state, MonsterState::Chase);
	}

	// 옆에서 다가오면 비껴 물러난다 - 물러날 자리는 끈 안쪽
	{
		MonsterBrainState s = Chasing(now);
		const vector<BrainPlayer> players{ { 7, -1200.f, 300.f } };
		BrainInput in = In(now, players);
		in.x = -1200.f;
		const BrainOutput out = MonsterBrain::Think(s, archer, in);
		ASSERT_TRUE(out.retreating);
		EXPECT_LE(Dist(out.goalX, out.goalY, 0.f, 0.f), 1300.f + 1.f);
		EXPECT_GT(Dist(out.goalX, out.goalY, -1200.f, 300.f), 300.f + 50.f);
	}

	// 근접 몬스터(keep 0)는 예전 그대로 - 물러나지 않고 사거리 × 0.8 까지 다가간다
	{
		MonsterTypeDef melee = archer;
		melee.kiteMinCm = 0.f;
		melee.keepRangeCm = 0.f;
		MonsterBrainState s = Chasing(now);
		const vector<BrainPlayer> players{ { 7, 300.f, 0.f } };
		const BrainOutput out = MonsterBrain::Think(s, melee, In(now, players));
		EXPECT_FALSE(out.retreating);
		EXPECT_FLOAT_EQ(out.stopDistCm, 150.f * MonsterConfig::STOP_DIST_RATIO);
	}
}

TEST(MonsterTable, 원거리_열_검사_AC_MA_102)
{
	SkillTable::Init();
	const string header = "monster_type_id,name_key,tier,max_hp,attack_power,attack_speed,attack_range_cm,armor,magic_resist,resistance,"
		"move_speed_cms,radius_cm,aggro_radius_cm,leash_radius_cm,search_s,give_up_s,assist_radius_cm,displace_immune,skill_id,drop_tier,drop_chance_pct,drop_rolls,"
		"aggro_kind,kite_min_cm,keep_range_cm\n";
	auto row = [](const string& kite, const string& keep)
	{
		return "8,monster.husk_archer,house,200,14,0.8,150,10,10,0,260,45,700,1400,3,8,0,0,0,none,0,0,guard," + kite + "," + keep + "\n";
	};
	auto parse = [&](const string& r, OUT string& error)
	{
		const string types = header + r;
		return MonsterTable::Parse(&types, nullptr, 4, OUT error);
	};

	string error;
	EXPECT_TRUE(parse(row("400", "650"), OUT error)) << error;
	ASSERT_NE(MonsterTable::FindType(8), nullptr);
	EXPECT_FLOAT_EQ(MonsterTable::FindType(8)->kiteMinCm, 400.f);
	EXPECT_FLOAT_EQ(MonsterTable::FindType(8)->keepRangeCm, 650.f);

	EXPECT_TRUE(parse(row("400", "500"), OUT error)) << error;			// 딱 100 차이
	EXPECT_FALSE(parse(row("400", "499"), OUT error));					// 선 자리에서 떤다
	EXPECT_NE(error.find("keep_range_cm"), string::npos) << error;
	EXPECT_FALSE(parse(row("x", "650"), OUT error));
	EXPECT_FALSE(parse(row("400", "1201"), OUT error));
	EXPECT_TRUE(parse(row("", ""), OUT error)) << error;				// 비면 근접
	EXPECT_FLOAT_EQ(MonsterTable::FindType(8)->keepRangeCm, 0.f);
}
