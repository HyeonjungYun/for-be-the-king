#include "pch.h"
#include "PatrolRouteGen.h"
#include "MonsterTable.h"
#include "HouseTable.h"
#include "TowerTable.h"
#include "WallTable.h"
#include "EntryPointTable.h"
#include "ItemTable.h"
#include "ItemRoller.h"
#include "SkillTable.h"
#include <fstream>

// 무리 순찰 길 생성 (2026-10-12 · design/quick-specs/pack-routes-2026-10-12.md § AC). 이름 끝의 AC 번호가 그 행이다.
// 실제 섬 데이터(Server/Data)와 합성 층 둘 다로 돈다. 생성은 시드로 결정적이다.

namespace
{
	// 실제 데이터 - 부팅 순서 그대로 (GameServer.cpp)
	bool LoadLive(OUT string& error)
	{
		const string dir = ItemTable::DefaultDataDir();
		return SkillTable::Load(dir, OUT error)
			&& ItemTable::Load(dir, OUT error)
			&& FloorItemPool::Load(dir, OUT error)
			&& EntryPointTable::Load(dir, 4, OUT error)
			&& WallTable::Load(dir, 4, EntryPointTable::All(), OUT error)
			&& TowerTable::Load(dir, 4, OUT error)
			&& MonsterTable::Load(dir, 4, OUT error)
			&& HouseTable::Load(dir, 4, OUT error)
			&& PackRulesTable::Load(dir, 4, OUT error);
	}

	RouteGenWorld LiveWorld()
	{
		RouteGenWorld w;
		w.walls = &WallTable::Get(0);
		w.houses = HouseTable::OnFloor(0);
		for (const TowerDef& t : TowerTable::All())
			if (t.floorId == 0) w.towers.emplace_back(t.x, t.y);
		for (const EntryPoint& e : EntryPointTable::All())
			if (e.floorId == 0) w.entries.emplace_back(e.x, e.y);
		w.bodyRadiusCm = 55.f + MonsterConfig::ROUTE_CLEARANCE_MARGIN_CM;		// 우두머리 멧돼지 55
		return w;
	}

	PackRules LiveRules()
	{
		const PackRules* r = PackRulesTable::Find(0);
		return r ? *r : PackRules();
	}

	// 합성 층 - 해안선 없음(600 m 정사각형) · 벽은 주어진 것만 · 타워 · 진입 없음
	struct Synthetic
	{
		WallMap		walls;
		RouteGenWorld	world;

		explicit Synthetic(vector<WallBox> boxes = {}, vector<HouseDef> houses = {})
		{
			string error;
			EXPECT_TRUE(walls.Build(move(boxes), OUT error)) << error;
			world.walls = &walls;
			world.houses = move(houses);
			world.bodyRadiusCm = 75.f;
		}
	};

	vector<MonsterTypeDef> PackTypes()
	{
		auto make = [](uint32 id, MonsterTier tier, MonsterAggroKind kind, float radius, float speed)
			{
				MonsterTypeDef t;
				t.typeId = id;
				t.tier = tier;
				t.aggroKind = kind;
				t.radiusCm = radius;
				t.moveSpeedCms = speed;
				return t;
			};
		return {
			make(4, MonsterTier::Field, MonsterAggroKind::Detect, 40.f, 300.f),
			make(5, MonsterTier::Leader, MonsterAggroKind::Detect, 50.f, 300.f),
			make(6, MonsterTier::Field, MonsterAggroKind::Reactive, 45.f, 280.f),
			make(7, MonsterTier::Leader, MonsterAggroKind::Reactive, 55.f, 280.f),
		};
	}

	PackRules SyntheticRules(uint32 packs = 10, uint32 reactivePct = 20)
	{
		PackRules r;
		r.packCount = packs;
		r.reactivePct = reactivePct;
		r.cellCm = 800.f;			// 600 m 정사각형 - 칸을 키워 빠르게
		r.candidates = 6;
		return r;
	}

	float Dist(float ax, float ay, float bx, float by) { return ::sqrtf((ax - bx) * (ax - bx) + (ay - by) * (ay - by)); }

	uint32 CountKind(const RouteSet& set, MonsterAggroKind kind)
	{
		uint32 n = 0;
		for (const GeneratedPack& p : set.packs)
			n += (p.def.kind == kind) ? 1 : 0;
		return n;
	}

	// 고리가 해안 띠에 닿는 점들 가운데 서로 가장 먼 두 점의 거리 - "끝에서 끝" 은 멀리 떨어진 두 해안 접점이다
	float FarthestCoastTouch(const WallMap& walls, const PatrolRoute& route, float bandCm)
	{
		vector<pair<float, float>> touches;
		for (const auto& p : route.points)
			if (walls.CoastGap(p.first, p.second) <= bandCm)
				touches.push_back(p);
		float best = 0.f;
		for (size_t i = 0; i < touches.size(); i++)
			for (size_t j = i + 1; j < touches.size(); j++)
				best = (std::max)(best, Dist(touches[i].first, touches[i].second, touches[j].first, touches[j].second));
		return best;
	}
}

/*-------- 실제 섬 --------*/

TEST(RouteGen, 실제_섬에서_10개_길이_규칙을_전부_지킨다_AC_PR_01_02_03_04_05)
{
	string error;
	ASSERT_TRUE(LoadLive(OUT error)) << error;
	const RouteGenWorld world = LiveWorld();
	const PackRules rules = LiveRules();
	ASSERT_EQ(rules.packCount, 10u) << "pack_rules.csv floor 0 must ask for 10 packs";

	RouteSet set;
	ASSERT_TRUE(PatrolRouteGen::Generate(world, rules, MonsterTable::AllTypes(), 1, OUT set, OUT error)) << error;
	ASSERT_EQ(set.packs.size(), 10u);

	const auto centroid = PatrolRouteGen::Centroid(*world.walls);
	const float coastBand = rules.coastMinCm + 1000.f;		// "맵의 끝" = 해안 안쪽 한계에서 10 m 안 (해안 띠 3 칸 + 칸 여유 + 줄 오프셋)
	uint32 crossings = 0;
	for (size_t i = 0; i < set.packs.size(); i++)
	{
		const GeneratedPack& p = set.packs[i];
		SCOPED_TRACE("route " + to_string(p.route.routeId));

		// AC-PR-01 닫힌 고리 · 벽 · 해안 · 집 · 타워 · 진입 지점 전부 (선분마다 정확 검사)
		EXPECT_TRUE(PatrolRouteGen::ValidateRoute(world, rules, p.route, OUT error)) << error;
		EXPECT_GE(p.route.points.size(), 3u);
		EXPECT_GE(PatrolRouteGen::LoopLength(p.route), 1000.f * 4);		// 줄 선 자리(4 × 150) 보다 넉넉히 길다

		// AC-PR-03 겹치지 않음 - 다른 모든 길과 sep_min 이상
		for (size_t j = i + 1; j < set.packs.size(); j++)
			EXPECT_GE(PatrolRouteGen::RouteDistance(p.route, set.packs[j].route), rules.sepMinCm - 1.f) << "vs route " << set.packs[j].route.routeId;

		// AC-PR-02 끝에서 끝 - 횡단은 두 끝이 모두 해안이고 50 m 넘게 떨어져 있다
		// AC-PR-05 같은 쪽 길은 해안에서 나와 띠 안에서 섬 중심에 가장 가까운 칸(+ 1.5 칸 + 줄 오프셋)까지 간다
		EXPECT_FLOAT_EQ(p.centreDistCm, PatrolRouteGen::PointToRoute(centroid.first, centroid.second, p.route));
		if (p.crossing)
		{
			crossings++;
			EXPECT_EQ(p.coastEnds, 2u);
			EXPECT_GE(FarthestCoastTouch(*world.walls, p.route, coastBand), 5000.f);
		}
		else if (p.coastEnds >= 1)
			EXPECT_LE(p.centreDistCm, p.bandCentreMinCm + rules.cellCm * 1.5f + rules.laneGapCm);		// 같은 쪽 - 중간까지
		else
			EXPECT_GE(p.lengthCm, 8000.f);		// 안쪽 주머니 (해안 없음) - 그래도 80 m 넘는 고리

		// 무리 정의 - 길과 1:1, 속도 상한, 종류가 방식과 맞다
		EXPECT_EQ(p.def.routeId, p.route.routeId);
		EXPECT_EQ(p.def.route, &p.route);
		EXPECT_LT(p.def.startSeq, p.route.points.size());
		const MonsterTypeDef* leader = MonsterTable::FindType(p.def.leaderTypeId);
		const MonsterTypeDef* member = MonsterTable::FindType(p.def.memberTypeId);
		ASSERT_NE(leader, nullptr);
		ASSERT_NE(member, nullptr);
		EXPECT_EQ(leader->tier, MonsterTier::Leader);
		EXPECT_EQ(member->tier, MonsterTier::Field);
		EXPECT_EQ(leader->aggroKind, p.def.kind);
		EXPECT_EQ(member->aggroKind, p.def.kind);
		EXPECT_LE(p.def.patrolSpeedCms, MonsterConfig::PATROL_SPEED_MAX_RATIO * (std::min)(leader->moveSpeedCms, member->moveSpeedCms) + 0.01f);
		EXPECT_EQ(p.def.memberCount, rules.memberCount);
	}

	// AC-PR-04 집 근처 금지 - 집 볼륨 둘레 어디서도 house_min 안에 길이 없다
	for (const HouseDef& h : world.houses)
	{
		float closest = FLT_MAX;
		for (float x = h.x0; x <= h.x1; x += 100.f)
			for (float y : { h.y0, h.y1 })
				for (const GeneratedPack& p : set.packs)
					closest = (std::min)(closest, PatrolRouteGen::PointToRoute(x, y, p.route));
		for (float y = h.y0; y <= h.y1; y += 100.f)
			for (float x : { h.x0, h.x1 })
				for (const GeneratedPack& p : set.packs)
					closest = (std::min)(closest, PatrolRouteGen::PointToRoute(x, y, p.route));
		EXPECT_GE(closest, rules.houseMinCm - 1.f) << h.houseId;
	}

	// 반응형 : 탐지형 = 2 : 8
	EXPECT_EQ(CountKind(set, MonsterAggroKind::Reactive), 2u);
	EXPECT_EQ(CountKind(set, MonsterAggroKind::Detect), 8u);
	EXPECT_EQ(set.stats.crossings, crossings);
	EXPECT_GE(set.stats.minSepCm, rules.sepMinCm - 1.f);
	EXPECT_GE(set.stats.coveragePct, 50.f);		// 고르게 - 걸을 수 있는 땅의 절반 이상이 어느 길의 30 m 안
	EXPECT_GE(crossings, 4u);					// 최대한 가로지르기 - 집이 막는 띠 말고는 횡단 (시드 20 개 관측 4 ~ 8)
	EXPECT_EQ(set.stats.crossings + set.stats.sameSide + set.stats.pockets, 10u);
	EXPECT_GE(set.stats.components, 2u);		// 서쪽 lobe 는 집 · 진입 · 타워 원에 봉인된 별개 성분이다 - 버리지 않는다
}

// AC-PR-15 본체 밖 성분(서쪽 lobe · Tb · 진입 5 · 6 · 9, y < -160 m)에도 길이 있다 - "곳곳에 고르게" (검증자 2026-10-12 수정 1: 예전엔 1,200 길 중 0)
TEST(RouteGen, 본체에서_떨어진_서쪽_lobe_에도_길이_하나_있다_AC_PR_15)
{
	string error;
	ASSERT_TRUE(LoadLive(OUT error)) << error;
	const RouteGenWorld world = LiveWorld();
	const PackRules rules = LiveRules();
	for (uint64 seed = 1; seed <= 8; seed++)
	{
		SCOPED_TRACE("seed " + to_string(seed));
		RouteSet set;
		ASSERT_TRUE(PatrolRouteGen::Generate(world, rules, MonsterTable::AllTypes(), seed, OUT set, OUT error)) << error;
		EXPECT_EQ(set.stats.sideRoutes, 1u);		// 가장 큰 본체 밖 성분(서쪽 lobe) 하나
		uint32 inLobe = 0;
		for (const GeneratedPack& p : set.packs)
		{
			bool lobe = false;
			for (const auto& pt : p.route.points)
				lobe = lobe || (pt.second < -16000.f);
			inLobe += lobe ? 1u : 0u;
			if (p.startSector == rules.packCount + 1)
			{
				EXPECT_TRUE(lobe) << "route " << p.route.routeId << " is a side route but not in the west lobe";
				EXPECT_GE(p.lengthCm, 8000.f);
			}
		}
		EXPECT_GE(inLobe, 1u);
		EXPECT_EQ(set.packs.size(), 10u);
		EXPECT_EQ(set.stats.crossings + set.stats.sameSide + set.stats.pockets, 10u);
	}
}

// 거부 쪽 경계 - ValidateRoute 는 집 house_min · 해안 coast_min 에서 허용 오차 1 cm(표본 반올림) 를 넘겨 모자라면 거부한다: −2 cm 거부 · +1 cm 통과 (검증자 수정 9 · r2 N1)
TEST(RouteGen, ValidateRoute_는_집과_해안_거리가_허용오차_1cm_넘게_모자라면_거부한다_AC_PR_16)
{
	HouseDef house;
	house.houseId = "H";
	house.x0 = -2000.f; house.x1 = 2000.f; house.y0 = -2000.f; house.y1 = 2000.f;
	Synthetic s({}, { house });
	PackRules rules = SyntheticRules(4, 25);
	string error;

	// 집 북쪽을 지나는 고리 - 가장 가까운 선분이 x = x1 + d 를 지난다 (집 테두리 x1 = 2000)
	auto loopAt = [](float x) { PatrolRoute r; r.routeId = 1; r.points = { { x, -5000.f }, { x, 5000.f }, { x + 400.f, 5000.f }, { x + 400.f, -5000.f } }; return r; };
	EXPECT_TRUE(PatrolRouteGen::ValidateRoute(s.world, rules, loopAt(2000.f + rules.houseMinCm + 1.f), OUT error)) << error;
	EXPECT_FALSE(PatrolRouteGen::ValidateRoute(s.world, rules, loopAt(2000.f + rules.houseMinCm - 2.f), OUT error));
	EXPECT_NE(error.find("house"), string::npos) << error;

	// 해안(정사각형 가장자리 HALF_CM) - coast_min 1 cm 안쪽은 거부
	auto edgeLoop = [](float x) { PatrolRoute r; r.routeId = 2; r.points = { { x, -5000.f }, { x, 5000.f }, { x - 400.f, 5000.f }, { x - 400.f, -5000.f } }; return r; };
	EXPECT_TRUE(PatrolRouteGen::ValidateRoute(s.world, rules, edgeLoop(FloorBounds::HALF_CM - rules.coastMinCm - 1.f), OUT error)) << error;
	EXPECT_FALSE(PatrolRouteGen::ValidateRoute(s.world, rules, edgeLoop(FloorBounds::HALF_CM - rules.coastMinCm + 2.f), OUT error));
	EXPECT_NE(error.find("coast"), string::npos) << error;
}

TEST(RouteGen, 시드가_같으면_같은_길_다르면_다른_길_AC_PR_06)
{
	string error;
	ASSERT_TRUE(LoadLive(OUT error)) << error;
	const RouteGenWorld world = LiveWorld();
	const PackRules rules = LiveRules();

	RouteSet a, b, c;
	ASSERT_TRUE(PatrolRouteGen::Generate(world, rules, MonsterTable::AllTypes(), 7, OUT a, OUT error)) << error;
	ASSERT_TRUE(PatrolRouteGen::Generate(world, rules, MonsterTable::AllTypes(), 7, OUT b, OUT error)) << error;
	ASSERT_TRUE(PatrolRouteGen::Generate(world, rules, MonsterTable::AllTypes(), 8, OUT c, OUT error)) << error;

	ASSERT_EQ(a.packs.size(), b.packs.size());
	for (size_t i = 0; i < a.packs.size(); i++)
	{
		EXPECT_EQ(a.packs[i].route.points, b.packs[i].route.points);
		EXPECT_EQ(a.packs[i].def.kind, b.packs[i].def.kind);
		EXPECT_EQ(a.packs[i].def.startSeq, b.packs[i].def.startSeq);
	}
	uint32 different = 0;
	for (size_t i = 0; i < a.packs.size() && i < c.packs.size(); i++)
		different += (a.packs[i].route.points != c.packs[i].route.points) ? 1 : 0;
	EXPECT_GE(different, 5u) << "seed 8 should not reproduce seed 7";
}

TEST(RouteGen, 시드_8개가_전부_성공하고_덮임과_횡단이_기준_이상이다_AC_PR_07)
{
	string error;
	ASSERT_TRUE(LoadLive(OUT error)) << error;
	const RouteGenWorld world = LiveWorld();
	const PackRules rules = LiveRules();

	float worstCoverage = 100.f;
	uint32 worstCrossings = 99;
	float worstSep = FLT_MAX;
	uint64 slowestUs = 0;
	for (uint64 seed = 1; seed <= 8; seed++)
	{
		RouteSet set;
		ASSERT_TRUE(PatrolRouteGen::Generate(world, rules, MonsterTable::AllTypes(), seed, OUT set, OUT error)) << "seed " << seed << ": " << error;
		EXPECT_EQ(set.packs.size(), 10u) << seed;
		worstCoverage = (std::min)(worstCoverage, set.stats.coveragePct);
		worstCrossings = (std::min)(worstCrossings, set.stats.crossings);
		worstSep = (std::min)(worstSep, set.stats.minSepCm);
		slowestUs = (std::max)(slowestUs, set.stats.elapsedUs);
	}
	EXPECT_GE(worstCoverage, 50.f);
	EXPECT_GE(worstCrossings, 4u);
	EXPECT_GE(worstSep, rules.sepMinCm - 1.f);
	EXPECT_LT(slowestUs, 5'000'000u) << "one set must not take 5 s - boot makes route_sets of them";
}

TEST(RouteGen, 부팅_경로는_층마다_route_sets_묶음을_만들고_CSV_무리를_버린다_AC_PR_09)
{
	string error;
	ASSERT_TRUE(LoadLive(OUT error)) << error;
	ASSERT_TRUE(MonsterTable::GenerateRouteSets(4, 12345, OUT error)) << error;
	const PackRules rules = LiveRules();
	ASSERT_EQ(MonsterTable::RouteSetCount(0), rules.routeSets);
	EXPECT_EQ(MonsterTable::RouteSetCount(1), 0u);
	EXPECT_EQ(MonsterTable::RouteSetAt(0, rules.routeSets), nullptr);
	for (size_t k = 0; k < rules.routeSets; k++)
	{
		const RouteSet* set = MonsterTable::RouteSetAt(0, k);
		ASSERT_NE(set, nullptr);
		EXPECT_EQ(set->packs.size(), 10u);
		EXPECT_EQ(set->floorId, 0u);
	}
	// 묶음끼리도 다르다 (인스턴스마다 다른 동선)
	EXPECT_NE(MonsterTable::RouteSetAt(0, 0)->packs[0].route.points, MonsterTable::RouteSetAt(0, 1)->packs[0].route.points);
	// 층 0 의 monster_packs.csv 무리는 남지 않는다
	for (const MonsterPackDef& p : MonsterTable::Packs())
		EXPECT_NE(p.floorId, 0u);
	MonsterTable::ClearRouteSets();
	EXPECT_EQ(MonsterTable::RouteSetCount(0), 0u);
}

/*-------- 합성 층 --------*/

TEST(RouteGen, 반응형_비율은_반올림이고_경계값이_맞다_AC_PR_08)
{
	Synthetic s;
	string error;
	const vector<MonsterTypeDef> types = PackTypes();

	struct Case { uint32 packs; uint32 pct; uint32 reactive; };
	for (const Case& c : { Case{ 10, 20, 2 }, Case{ 5, 20, 1 }, Case{ 10, 0, 0 }, Case{ 10, 100, 10 }, Case{ 10, 25, 3 }, Case{ 4, 20, 1 }, Case{ 2, 20, 0 } })
	{
		RouteSet set;
		ASSERT_TRUE(PatrolRouteGen::Generate(s.world, SyntheticRules(c.packs, c.pct), types, 3, OUT set, OUT error)) << c.packs << '/' << c.pct << ": " << error;
		EXPECT_EQ(set.packs.size(), c.packs);
		EXPECT_EQ(CountKind(set, MonsterAggroKind::Reactive), c.reactive) << c.packs << '/' << c.pct;
		EXPECT_EQ(CountKind(set, MonsterAggroKind::Detect), c.packs - c.reactive);
	}

	// 방식에 맞는 종류가 없으면 실패 (반응형 1 이상인데 반응형 종류 0)
	vector<MonsterTypeDef> detectOnly = { types[0], types[1] };
	RouteSet set;
	EXPECT_FALSE(PatrolRouteGen::Generate(s.world, SyntheticRules(10, 20), detectOnly, 3, OUT set, OUT error));
	EXPECT_TRUE(PatrolRouteGen::Generate(s.world, SyntheticRules(10, 0), detectOnly, 3, OUT set, OUT error)) << error;
}

TEST(RouteGen, 합성_층의_집은_house_min_만큼_비켜_가고_길은_정사각형_끝에서_끝이다_AC_PR_04)
{
	// 가운데 60 × 60 m 집 (벽으로도 막는다 - 집 테두리 벽 4 개)
	HouseDef house;
	house.houseId = "H";
	house.x0 = -3000.f; house.x1 = 3000.f; house.y0 = -3000.f; house.y1 = 3000.f;
	vector<WallBox> boxes;
	for (int32 k = 0; k < 4; k++)
	{
		WallBox w;
		w.kind = WallKind::Wall;
		w.halfLength = 3000.f;
		w.halfThickness = 25.f;
		w.cx = (k == 0) ? 3000.f : (k == 1) ? -3000.f : 0.f;
		w.cy = (k == 2) ? 3000.f : (k == 3) ? -3000.f : 0.f;
		w.yawDeg = (k < 2) ? 90.f : 0.f;
		boxes.push_back(w);
	}
	Synthetic s(boxes, { house });
	string error;
	PackRules rules = SyntheticRules(6, 50);
	rules.houseMinCm = 2500.f;
	RouteSet set;
	ASSERT_TRUE(PatrolRouteGen::Generate(s.world, rules, PackTypes(), 11, OUT set, OUT error)) << error;
	ASSERT_EQ(set.packs.size(), 6u);

	for (const GeneratedPack& p : set.packs)
	{
		EXPECT_TRUE(PatrolRouteGen::ValidateRoute(s.world, rules, p.route, OUT error)) << error;
		float closest = FLT_MAX;
		for (float t = -3000.f; t <= 3000.f; t += 100.f)
		{
			closest = (std::min)(closest, PatrolRouteGen::PointToRoute(t, -3000.f, p.route));
			closest = (std::min)(closest, PatrolRouteGen::PointToRoute(t, 3000.f, p.route));
			closest = (std::min)(closest, PatrolRouteGen::PointToRoute(-3000.f, t, p.route));
			closest = (std::min)(closest, PatrolRouteGen::PointToRoute(3000.f, t, p.route));
		}
		EXPECT_GE(closest, rules.houseMinCm - 1.f);
		// 끝에서 끝 - 횡단은 정사각형 가장자리 띠(coast_min + 2.3 칸)에 닿는 두 점이 100 m 넘게 떨어져 있다.
		// 집이 띠를 막은 곳은 같은 쪽 길 - 가장자리에서 나와 집 곁(중심에 가장 가까운 칸)까지 갔다 온다
		if (p.crossing)
			EXPECT_GE(FarthestCoastTouch(s.walls, p.route, rules.coastMinCm + rules.cellCm * 4.f), 10000.f);
		else
			EXPECT_LE(p.centreDistCm, p.bandCentreMinCm + rules.cellCm * 1.5f + rules.laneGapCm);
	}
	for (size_t i = 0; i < set.packs.size(); i++)
		for (size_t j = i + 1; j < set.packs.size(); j++)
			EXPECT_GE(PatrolRouteGen::RouteDistance(set.packs[i].route, set.packs[j].route), rules.sepMinCm - 1.f);
}

/*-------- pack_rules.csv --------*/

TEST(PackRulesTable, 범위_밖과_중복_층은_부팅을_거부하고_경계값은_통과한다_AC_PR_10)
{
	string error;
	const string header = "floor_id,pack_count,reactive_pct,member_count,spacing_cm,patrol_speed_cms,respawn_s,route_sets,sep_min_cm,house_min_cm,coast_min_cm,cover_radius_cm,cell_cm,candidates,lane_gap_cm\n";
	auto row = [](const string& packs = "10", const string& pct = "20", const string& members = "3", const string& speed = "170", const string& sets = "3",
		const string& sep = "1200", const string& cell = "200", const string& gap = "300", const string& floor = "0")
		{
			return floor + "," + packs + "," + pct + "," + members + ",150," + speed + ",1800," + sets + "," + sep + ",1500,1000,3000," + cell + ",16," + gap + "\n";
		};

	EXPECT_TRUE(PackRulesTable::Parse(header + row(), 4, OUT error)) << error;
	ASSERT_EQ(PackRulesTable::All().size(), 1u);
	EXPECT_EQ(PackRulesTable::Find(0)->packCount, 10u);
	EXPECT_EQ(PackRulesTable::Find(1), nullptr);

	EXPECT_TRUE(PackRulesTable::Parse(header + row("1"), 4, OUT error)) << error;			// pack_count 하한
	EXPECT_FALSE(PackRulesTable::Parse(header + row("0"), 4, OUT error));
	EXPECT_TRUE(PackRulesTable::Parse(header + row("40"), 4, OUT error)) << error;
	EXPECT_FALSE(PackRulesTable::Parse(header + row("41"), 4, OUT error));
	EXPECT_TRUE(PackRulesTable::Parse(header + row("10", "100"), 4, OUT error)) << error;
	EXPECT_FALSE(PackRulesTable::Parse(header + row("10", "101"), 4, OUT error));
	EXPECT_FALSE(PackRulesTable::Parse(header + row("10", "20", "6"), 4, OUT error));			// PACK_MAX_MEMBERS 5
	EXPECT_FALSE(PackRulesTable::Parse(header + row("10", "20", "3", "307"), 4, OUT error));	// 플레이어보다 빠른 순찰
	EXPECT_FALSE(PackRulesTable::Parse(header + row("10", "20", "3", "170", "0"), 4, OUT error));
	EXPECT_FALSE(PackRulesTable::Parse(header + row("10", "20", "3", "170", "3", "400"), 4, OUT error));	// sep 하한 500
	EXPECT_FALSE(PackRulesTable::Parse(header + row("10", "20", "3", "170", "3", "1200", "200", "150"), 4, OUT error));	// lane_gap 하한 200
	EXPECT_FALSE(PackRulesTable::Parse(header + row("10", "20", "3", "170", "3", "1200", "200", "300", "4"), 4, OUT error));	// 층 범위
	EXPECT_FALSE(PackRulesTable::Parse(header + row() + row(), 4, OUT error));				// 같은 층 두 번
	EXPECT_FALSE(PackRulesTable::Parse(header + "0,ten,20,3,150,170,1800,3,1200,1500,1000,3000,200,8,300\n", 4, OUT error));
	EXPECT_TRUE(PackRulesTable::Parse(header, 4, OUT error)) << error;						// 행 0 = 규칙 없음
	EXPECT_TRUE(PackRulesTable::All().empty());
}

/*-------- 덤프 (증거 · 그림) --------*/

// 도구 (테스트 아님 - DISABLED_, 검증자 수정 6): FBTK_ROUTE_DUMP="1,2,3" 으로 그 시드들의 길을 %TEMP%\rg\gen-<seed>.csv 로 쓴다 - tools/level/draw_routes.py 로 그린다.
//   GameServerTests.exe --gtest_filter=*FBTK_ROUTE_DUMP* --gtest_also_run_disabled_tests
TEST(RouteGen, DISABLED_덤프_FBTK_ROUTE_DUMP)
{
	char* raw = nullptr;
	size_t length = 0;
	if (_dupenv_s(&raw, &length, "FBTK_ROUTE_DUMP") != 0 || raw == nullptr)
	{
		ADD_FAILURE() << "set FBTK_ROUTE_DUMP=<seed,seed,...> (this is a dump tool, not a test)";
		return;
	}
	const string seeds = raw;
	::free(raw);
	char* tmp = nullptr;
	if (_dupenv_s(&tmp, &length, "TEMP") != 0 || tmp == nullptr)
		return;
	const string dir = string(tmp) + "\\rg";
	::free(tmp);

	string error;
	ASSERT_TRUE(LoadLive(OUT error)) << error;
	const RouteGenWorld world = LiveWorld();
	const PackRules rules = LiveRules();
	size_t pos = 0;
	while (pos < seeds.size())
	{
		size_t next = seeds.find(',', pos);
		if (next == string::npos) next = seeds.size();
		const uint64 seed = ::strtoull(seeds.substr(pos, next - pos).c_str(), nullptr, 10);
		pos = next + 1;
		RouteSet set;
		const bool ok = PatrolRouteGen::Generate(world, rules, MonsterTable::AllTypes(), seed, OUT set, OUT error);
		printf("seed %llu: %s attempts=%u routes=%u crossings=%u same=%u pockets=%u side=%u comps=%u dropped=%u coverage=%.0f%% min_sep=%.0f avg_len=%.0fm merged=%u overflow=%u retries=%u (path %u pull %u lane %u) deg=%.0f %s\n",
			static_cast<unsigned long long>(seed), ok ? "ok" : "FAIL", set.stats.attempts, set.stats.routes, set.stats.crossings, set.stats.sameSide, set.stats.pockets, set.stats.sideRoutes, set.stats.components, set.stats.droppedCells, set.stats.coveragePct, set.stats.minSepCm,
			set.stats.avgLengthM, set.stats.merged, set.stats.overflow, set.stats.retries, set.stats.failPath, set.stats.failPull, set.stats.failLane, set.stats.crossDeg, ok ? "" : error.c_str());
		EXPECT_TRUE(ok) << error;
		for (const GeneratedPack& p : set.packs)
		{
			// 길의 끝 둘 - 첫 점과 첫 점에서 가장 먼 점의 해안 거리
			const auto& p0 = p.route.points.front();
			float farD = 0.f;
			pair<float, float> farPt = p0;
			for (const auto& pt : p.route.points)
			{
				const float d = Dist(pt.first, pt.second, p0.first, p0.second);
				if (d > farD) { farD = d; farPt = pt; }
			}
			printf("  route %u band %u %s coastEnds=%u len=%.0fm centre=%.0fm bandCentreMin=%.0fm pts=%zu coastGap=%.0f/%.0f\n", p.route.routeId, p.startSector,
				p.crossing ? "cross" : "same ", p.coastEnds, p.lengthCm / 100.f, p.centreDistCm / 100.f, p.bandCentreMinCm / 100.f, p.route.points.size(),
				world.walls->CoastGap(p0.first, p0.second), world.walls->CoastGap(farPt.first, farPt.second));
		}
		ofstream file(dir + "\\gen-" + to_string(seed) + ".csv", ios::binary | ios::trunc);
		file << PatrolRouteGen::ToCsv(set);
	}
}
