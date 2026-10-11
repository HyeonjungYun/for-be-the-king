#include "pch.h"
#include "MonsterTable.h"
#include "MonsterPack.h"
#include "TowerTable.h"
#include "WallTable.h"
#include "EntryPointTable.h"
#include "ItemTable.h"
#include "SkillTable.h"

// 몬스터 무리 (2026-10-11 몬스터 개선 · design/gdd/monster-ai.md § J). PackBrain · PackTrail 은 Room 을 모르는 순수 로직이라
// 시계 · 플레이어 · 시선을 주입해 돌린다.

namespace
{
	constexpr uint64 MS = 1000;
	constexpr uint64 SEC = 1'000'000;

	const char* TYPES_HEADER = "monster_type_id,name_key,tier,max_hp,attack_power,attack_speed,attack_range_cm,armor,magic_resist,resistance,"
		"move_speed_cms,radius_cm,aggro_radius_cm,leash_radius_cm,search_s,give_up_s,assist_radius_cm,displace_immune,skill_id,drop_tier,drop_chance_pct,drop_rolls,aggro_kind\n";
	const char* ROUTES_HEADER = "route_id,floor_id,seq,x,y\n";
	const char* PACKS_HEADER = "pack_id,floor_id,route_id,start_seq,leader_type_id,member_type_id,member_count,spacing_cm,patrol_speed_cms,respawn_s\n";

	// 4 탐지 기본 · 5 탐지 우두머리 · 6 반응 기본 · 7 반응 우두머리 (드랍 없음 - 풀이 필요 없다)
	const char* PACK_TYPES =
		"4,monster.wolf,field,60,3,1.0,150,0,0,0,300,40,600,0,3,0,0,0,0,none,0,0,detect\n"
		"5,monster.wolf_alpha,leader,180,7,1.0,160,10,10,0.1,300,50,600,0,3,0,0,0,0,none,0,0,detect\n"
		"6,monster.boar,field,60,3,1.0,150,0,0,0,280,45,600,1500,2,8,0,0,0,none,0,0,reactive\n"
		"7,monster.boar_chief,leader,180,7,1.0,170,10,10,0.1,280,55,600,1500,2,8,0,0,0,none,0,0,reactive\n";

	// 진입 (0, 0) · 타워 (10000, 0) · 벽 없음 · 층 2개
	void SyntheticWorld(const string* walls = nullptr)
	{
		string error;
		SkillTable::Init();
		ASSERT_TRUE(EntryPointTable::Parse("entry_id,floor_id,x,y,z,yaw\n1,0,0,0,100,0\n2,1,0,0,100,0\n", 2, OUT error)) << error;
		const string versions = "floor_id,data_version\n0,test\n";
		ASSERT_TRUE(WallTable::Parse(walls, walls != nullptr ? &versions : nullptr, 2, EntryPointTable::All(), OUT error)) << error;
		ASSERT_TRUE(TowerTable::Parse("tower_id,floor_id,x,y\nTa,0,10000,0\n", 2, OUT error)) << error;
	}

	// 길 하나: 가로 두 줄 (x = 5000 · 4600, y -4000 ~ 4000) - 진입 · 타워에서 20 m 밖
	const char* LOOP_ROUTE = "1,0,0,5000,-4000\n1,0,1,5000,4000\n1,0,2,4600,4000\n1,0,3,4600,-4000\n";

	bool ParseAll(const string& typeRows, const string& routeRows, const string& packRows, OUT string& error)
	{
		const string types = string(TYPES_HEADER) + typeRows;
		const string routes = string(ROUTES_HEADER) + routeRows;
		const string packs = string(PACKS_HEADER) + packRows;
		return MonsterTable::Parse(&types, nullptr, 2, OUT error) && MonsterTable::ParsePacks(&routes, &packs, 2, OUT error);
	}

	PackMemberView View(uint32 index, float x, float y, float leash = 0.f, float giveUp = 0.f)
	{
		PackMemberView v;
		v.index = index;
		v.x = x;
		v.y = y;
		v.attackRangeCm = 150.f;
		v.aggroRadiusCm = 600.f;
		v.leashRadiusCm = leash;
		v.giveUpS = giveUp;
		v.moveSpeedCms = 300.f;
		return v;
	}

	MonsterPackDef PackDef(const PatrolRoute& route, MonsterAggroKind kind)
	{
		MonsterPackDef d;
		d.packId = 601;
		d.routeId = route.routeId;
		d.memberCount = 3;
		d.spacingCm = 150.f;
		d.patrolSpeedCms = 180.f;
		d.respawnSeconds = 1800;
		d.kind = kind;
		d.route = &route;
		return d;
	}

	PatrolRoute StraightRoute()
	{
		PatrolRoute r;
		r.routeId = 1;
		r.points = { { 0.f, 0.f }, { 0.f, 4000.f }, { -400.f, 4000.f }, { -400.f, 0.f } };
		return r;
	}

	float Dist(float ax, float ay, float bx, float by)
	{
		return ::sqrtf((ax - bx) * (ax - bx) + (ay - by) * (ay - by));
	}
}

/*-------- 데이터 --------*/

TEST(MonsterPackTable, 어그로_방식_열이_없으면_guard_탐지형만_끈과_포기시간_0을_쓴다)
{
	SyntheticWorld();
	string error;

	// 예전 헤더(열 없음) - 전부 guard
	const string legacy = "monster_type_id,name_key,tier,max_hp,attack_power,attack_speed,attack_range_cm,armor,magic_resist,resistance,"
		"move_speed_cms,radius_cm,aggro_radius_cm,leash_radius_cm,search_s,give_up_s,assist_radius_cm,displace_immune,skill_id,drop_tier,drop_chance_pct,drop_rolls\n"
		"1,monster.hound,field,120,6,1.0,150,0,0,0,300,40,600,1800,2,6,1000,0,0,none,0,0\n";
	ASSERT_TRUE(MonsterTable::Parse(&legacy, nullptr, 2, OUT error)) << error;
	EXPECT_EQ(MonsterTable::FindType(1)->aggroKind, MonsterAggroKind::Guard);

	// 탐지형 - 끈 0 · 포기 0 허용
	const string types = string(TYPES_HEADER) + PACK_TYPES;
	ASSERT_TRUE(MonsterTable::Parse(&types, nullptr, 2, OUT error)) << error;
	EXPECT_EQ(MonsterTable::FindType(5)->aggroKind, MonsterAggroKind::Detect);
	EXPECT_EQ(MonsterTable::FindType(7)->aggroKind, MonsterAggroKind::Reactive);
	EXPECT_EQ(MonsterTable::FindType(5)->tier, MonsterTier::Leader);

	// 반응형은 끈 0 이 안 된다 (돌아올 거리가 있어야 한다)
	const string badReactive = string(TYPES_HEADER) + "6,monster.boar,field,60,3,1.0,150,0,0,0,280,45,600,0,2,8,0,0,0,none,0,0,reactive\n";
	EXPECT_FALSE(MonsterTable::Parse(&badReactive, nullptr, 2, OUT error));

	// 반응형 포기 0 도 안 된다
	const string badGiveUp = string(TYPES_HEADER) + "6,monster.boar,field,60,3,1.0,150,0,0,0,280,45,600,1500,2,0,0,0,0,none,0,0,reactive\n";
	EXPECT_FALSE(MonsterTable::Parse(&badGiveUp, nullptr, 2, OUT error));

	const string unknown = string(TYPES_HEADER) + "6,monster.boar,field,60,3,1.0,150,0,0,0,280,45,600,1500,2,8,0,0,0,none,0,0,sleepy\n";
	EXPECT_FALSE(MonsterTable::Parse(&unknown, nullptr, 2, OUT error));
}

TEST(MonsterPackTable, 길과_무리를_읽고_잘못된_무리는_부팅을_거부한다)
{
	SyntheticWorld();
	string error;

	ASSERT_TRUE(ParseAll(PACK_TYPES, LOOP_ROUTE, "601,0,1,0,5,4,3,150,180,1800\n", OUT error)) << error;
	ASSERT_EQ(MonsterTable::Packs().size(), 1u);
	EXPECT_EQ(MonsterTable::Packs()[0].kind, MonsterAggroKind::Detect);
	ASSERT_NE(MonsterTable::Packs()[0].route, nullptr);
	EXPECT_EQ(MonsterTable::Packs()[0].route->points.size(), 4u);

	// 우두머리와 기본의 방식이 다르다
	EXPECT_FALSE(ParseAll(PACK_TYPES, LOOP_ROUTE, "601,0,1,0,5,6,3,150,180,1800\n", OUT error));
	// 6 마리는 너무 많다
	EXPECT_FALSE(ParseAll(PACK_TYPES, LOOP_ROUTE, "601,0,1,0,5,4,6,150,180,1800\n", OUT error));
	// 줄 간격이 두 몸 + 20 보다 좁다 (50 + 40 + 20 = 110)
	EXPECT_FALSE(ParseAll(PACK_TYPES, LOOP_ROUTE, "601,0,1,0,5,4,3,100,180,1800\n", OUT error));
	EXPECT_TRUE(ParseAll(PACK_TYPES, LOOP_ROUTE, "601,0,1,0,5,4,3,110,180,1800\n", OUT error)) << error;
	// 순찰 속도 > 0.8 × 가장 느린 몸 (300 × 0.8 = 240)
	EXPECT_FALSE(ParseAll(PACK_TYPES, LOOP_ROUTE, "601,0,1,0,5,4,3,150,241,1800\n", OUT error));
	EXPECT_TRUE(ParseAll(PACK_TYPES, LOOP_ROUTE, "601,0,1,0,5,4,3,150,240,1800\n", OUT error)) << error;
	// 없는 길 · 시작점이 길 밖
	EXPECT_FALSE(ParseAll(PACK_TYPES, LOOP_ROUTE, "601,0,2,0,5,4,3,150,180,1800\n", OUT error));
	EXPECT_FALSE(ParseAll(PACK_TYPES, LOOP_ROUTE, "601,0,1,4,5,4,3,150,180,1800\n", OUT error));
	// guard 종류로는 무리를 못 만든다
	EXPECT_FALSE(ParseAll(string(PACK_TYPES) + "1,monster.hound,field,120,6,1.0,150,0,0,0,300,40,600,1800,2,6,1000,0,0,none,0,0,guard\n",
		LOOP_ROUTE, "601,0,1,0,1,1,3,150,180,1800\n", OUT error));
}

TEST(MonsterPackTable, 순찰_길은_닫힌_고리이고_벽_진입_타워를_피한다)
{
	SyntheticWorld();
	string error;
	const string pack = "601,0,1,0,5,4,3,150,180,1800\n";

	// 점 2 개는 고리가 아니다 · seq 가 건너뛴다
	EXPECT_FALSE(ParseAll(PACK_TYPES, "1,0,0,5000,-4000\n1,0,1,5000,4000\n", pack, OUT error));
	EXPECT_FALSE(ParseAll(PACK_TYPES, "1,0,0,5000,-4000\n1,0,2,5000,4000\n1,0,3,4600,4000\n", pack, OUT error));

	// 진입 지점 (0, 0) 에서 20 m 안을 지난다 (y = 1900 줄)
	EXPECT_FALSE(ParseAll(PACK_TYPES, "1,0,0,1900,-4000\n1,0,1,1900,4000\n1,0,2,1500,4000\n1,0,3,1500,-4000\n", pack, OUT error));

	// 타워 (10000, 0) 에서 20 m 안을 지난다 (x = 8100 줄)
	EXPECT_FALSE(ParseAll(PACK_TYPES, "1,0,0,8100,-4000\n1,0,1,8100,4000\n1,0,2,7700,4000\n1,0,3,7700,-4000\n", pack, OUT error));

	// 길이 벽을 지난다 - 몸 50 + 20 으로 쓸어 본다
	const string walls = "floor_id,kind,cx,cy,half_length,half_thickness,yaw\n0,wall,5000,0,100,25,90\n";
	SyntheticWorld(&walls);
	EXPECT_FALSE(ParseAll(PACK_TYPES, LOOP_ROUTE, pack, OUT error));
}

/*-------- 발자국 --------*/

TEST(PackTrail, 발자국은_호_길이로_점을_찾고_머리가_바뀌면_자른다)
{
	PackTrail trail;
	trail.SetMaxLength(10000.f);
	trail.Seed({ { 0.f, 0.f }, { 0.f, 300.f } });		// 꼬리 → 머리
	EXPECT_FLOAT_EQ(trail.HeadS(), 300.f);

	float x = 0.f;
	float y = 0.f;
	ASSERT_TRUE(trail.PointAt(150.f, OUT x, OUT y));
	EXPECT_NEAR(y, 150.f, 0.01f);

	// 머리가 걸어간다 - 25 cm 마다 점이 남는다
	for (int32 i = 1; i <= 40; i++)
		trail.Record(0.f, 300.f + 10.f * i);
	EXPECT_NEAR(trail.HeadS(), 700.f, 0.5f);
	ASSERT_TRUE(trail.PointAt(trail.HeadS() - 150.f, OUT x, OUT y));
	EXPECT_NEAR(y, 550.f, 1.f);

	// 머리가 죽었다 - 한 칸(150) 자른다. 새 머리 = 550
	trail.TruncateTo(trail.HeadS() - 150.f);
	EXPECT_NEAR(trail.HeadS(), 550.f, 1.f);
	ASSERT_TRUE(trail.PointAt(trail.HeadS(), OUT x, OUT y));
	EXPECT_NEAR(y, 550.f, 1.f);
}

TEST(PackTrail, 줄_선_자리는_머리부터_길을_거슬러_간격만큼)
{
	const PatrolRoute route = StraightRoute();
	// 머리 = 점 1 (0, 4000). 뒤는 점 0 쪽으로 150 씩
	const auto pos = PackBrain::FilePositions(route, 1, 4, 150.f);
	ASSERT_EQ(pos.size(), 4u);
	EXPECT_NEAR(pos[0].second, 4000.f, 0.01f);
	EXPECT_NEAR(pos[1].second, 3850.f, 0.01f);
	EXPECT_NEAR(pos[3].second, 3550.f, 0.01f);

	// 모서리를 돈다 - 머리 = 점 0 (0, 0), 거꾸로 가면 점 3 (-400, 0) 쪽
	const auto corner = PackBrain::FilePositions(route, 0, 4, 150.f);
	EXPECT_NEAR(corner[1].first, -150.f, 0.01f);
	EXPECT_NEAR(corner[3].first, -400.f, 0.01f);
	EXPECT_NEAR(corner[3].second, 50.f, 0.01f);		// 450 - 400 = 점 3 에서 (−400, 0)→(−400, 4000) 쪽으로 50
}

/*-------- 판단 --------*/

TEST(PackBrain, 탐지형은_몸_하나라도_감지_반경_안의_플레이어를_무리째_쫓는다)
{
	const PatrolRoute route = StraightRoute();
	const MonsterPackDef def = PackDef(route, MonsterAggroKind::Detect);
	PackState p;
	PackBrain::Spawn(p, def, { 11, 12, 13, 14 }, PackBrain::FilePositions(route, 1, 4, 150.f), 0);

	// 꼬리(0, 3550) 에서 601 cm - 아무도 안 본다
	vector<PackMemberView> living = { View(0, 0.f, 4000.f), View(1, 0.f, 3850.f), View(2, 0.f, 3700.f), View(3, 0.f, 3550.f) };
	vector<BrainPlayer> players = { { 77, 0.f, 3550.f - 601.f } };

	PackInput in;
	in.def = &def;
	in.living = &living;
	in.players = &players;

	// 태어난 뒤 1 초는 감지하지 않는다
	players[0].y = 3550.f - 100.f;
	in.nowUs = 500 * MS;
	EXPECT_EQ(PackBrain::Think(p, in).aggroTargetId, 0u);

	players[0].y = 3550.f - 601.f;
	in.nowUs = 2 * SEC;
	EXPECT_EQ(PackBrain::Think(p, in).aggroTargetId, 0u);
	EXPECT_EQ(p.mode, PackMode::Patrol);

	// 599 cm - 꼬리가 봤다 → 무리 전체가 교전
	players[0].y = 3550.f - 599.f;
	in.nowUs = 2 * SEC + 100 * MS;
	const PackOutput out = PackBrain::Think(p, in);
	EXPECT_EQ(out.aggroTargetId, 77u);
	EXPECT_STREQ(out.aggroBy, "detect");
	EXPECT_EQ(p.mode, PackMode::Engage);

	// 시선이 막히면 못 본다
	PackState blind;
	PackBrain::Spawn(blind, def, { 11, 12, 13, 14 }, PackBrain::FilePositions(route, 1, 4, 150.f), 0);
	in.lineClear = [](float, float, float, float) { return false; };
	EXPECT_EQ(PackBrain::Think(blind, in).aggroTargetId, 0u);
}

TEST(PackBrain, 탐지형은_처음_목표만_쫓고_다른_사람이_때리면_그_사람으로)
{
	const PatrolRoute route = StraightRoute();
	const MonsterPackDef def = PackDef(route, MonsterAggroKind::Detect);
	PackState p;
	PackBrain::Spawn(p, def, { 11, 12, 13, 14 }, PackBrain::FilePositions(route, 1, 4, 150.f), 0);

	vector<PackMemberView> living = { View(0, 0.f, 4000.f), View(1, 0.f, 3850.f), View(2, 0.f, 3700.f), View(3, 0.f, 3550.f) };
	vector<BrainPlayer> players = { { 77, 0.f, 4300.f }, { 88, 0.f, 4200.f } };
	PackInput in;
	in.def = &def;
	in.living = &living;
	in.players = &players;
	in.nowUs = 2 * SEC;

	// 더 가까운 88 에게 붙는다
	PackBrain::Think(p, in);
	ASSERT_EQ(p.targetId, 88u);

	// 77 이 다가와도(공격 없이) 목표는 그대로 - "어그로가 끌린 유저만"
	players[0].y = 4050.f;
	in.nowUs += 100 * MS;
	PackBrain::Think(p, in);
	EXPECT_EQ(p.targetId, 88u);

	// 77 이 무리를 때렸다 → 목표가 77 로
	PackBrain::OnMemberDamaged(p, 77, 0.f, 4050.f, in.nowUs);
	in.nowUs += 100 * MS;
	const PackOutput out = PackBrain::Think(p, in);
	EXPECT_TRUE(out.retargeted);
	EXPECT_EQ(p.targetId, 77u);

	// 몬스터 id(플레이어 아님)가 때린 것은 무시한다 - 몬스터끼리는 적이 아니다
	PackBrain::OnMemberDamaged(p, 4'000'000'123ull, 0.f, 0.f, in.nowUs);
	in.nowUs += 100 * MS;
	PackBrain::Think(p, in);
	EXPECT_EQ(p.targetId, 77u);

	// 탐지형은 끈이 없다 - 멀리 가도 쫓는다. 목표가 사라지면(사망 · 퇴장) 돌아간다
	players[0].y = 40000.f;
	in.nowUs += 100 * MS;
	PackBrain::Think(p, in);
	EXPECT_EQ(p.mode, PackMode::Engage);

	players.erase(players.begin());
	in.nowUs += 100 * MS;
	const PackOutput gone = PackBrain::Think(p, in);
	EXPECT_EQ(p.mode, PackMode::Return);
	EXPECT_STREQ(gone.returnReason, "target_gone");
}

TEST(PackBrain, 반응형은_가까이_있어도_맞기_전엔_쫓지_않고_끈을_벗어나면_앵커로_돌아가_길을_잇는다)
{
	const PatrolRoute route = StraightRoute();
	const MonsterPackDef def = PackDef(route, MonsterAggroKind::Reactive);
	PackState p;
	PackBrain::Spawn(p, def, { 11, 12, 13, 14 }, PackBrain::FilePositions(route, 1, 4, 150.f), 0);
	const uint32 waypointBefore = p.waypoint;

	vector<PackMemberView> living = { View(0, 0.f, 4000.f, 1500.f, 8.f), View(1, 0.f, 3850.f, 1500.f, 8.f),
		View(2, 0.f, 3700.f, 1500.f, 8.f), View(3, 0.f, 3550.f, 1500.f, 8.f) };
	vector<BrainPlayer> players = { { 77, 0.f, 4100.f } };
	PackInput in;
	in.def = &def;
	in.living = &living;
	in.players = &players;

	// 1 m 옆에 10 초 서 있어도 아무 일 없다
	for (uint64 t = 2 * SEC; t < 12 * SEC; t += 100 * MS)
	{
		in.nowUs = t;
		EXPECT_EQ(PackBrain::Think(p, in).aggroTargetId, 0u);
	}

	// 셋째 몸이 맞았다 → 무리 전체가 그 사람에게, 앵커 = 지금 자리
	PackBrain::OnMemberDamaged(p, 77, 0.f, 4100.f, in.nowUs);
	in.nowUs += 100 * MS;
	const PackOutput aggro = PackBrain::Think(p, in);
	EXPECT_EQ(aggro.aggroTargetId, 77u);
	EXPECT_STREQ(aggro.aggroBy, "attack");
	ASSERT_EQ(aggro.perMember.size(), 4u);
	for (const BrainOutput& o : aggro.perMember)
	{
		EXPECT_TRUE(o.hasGoal);
		EXPECT_FLOAT_EQ(o.goalY, 4100.f);
	}
	EXPECT_FLOAT_EQ(p.members[2].anchorY, 3700.f);

	// 1500 cm 까지는 쫓는다
	living[0].y = 4000.f + 1500.f;
	in.nowUs += 100 * MS;
	PackBrain::Think(p, in);
	EXPECT_EQ(p.mode, PackMode::Engage);

	// 한 몸이라도 1501 → 그 순간 돌아간다
	living[0].y = 4000.f + 1501.f;
	in.nowUs += 100 * MS;
	const PackOutput back = PackBrain::Think(p, in);
	EXPECT_EQ(p.mode, PackMode::Return);
	EXPECT_STREQ(back.returnReason, "leash");

	// 돌아가는 중엔 맞아도 어그로가 없다
	PackBrain::OnMemberDamaged(p, 77, 0.f, 0.f, in.nowUs);
	EXPECT_EQ(p.pendingAttackerId, 0u);

	// 다 도착하면 순찰로 - 만피(resumed), 향하던 길의 점 그대로
	living[0].y = 4000.f;
	in.nowUs += 100 * MS;
	const PackOutput resume = PackBrain::Think(p, in);
	EXPECT_TRUE(resume.resumed);
	EXPECT_EQ(p.mode, PackMode::Patrol);
	EXPECT_EQ(p.waypoint, waypointBefore);
}

TEST(PackBrain, 우두머리가_죽으면_다음_몸이_앞장서고_뒤의_몸들의_목표는_그대로다)
{
	const PatrolRoute route = StraightRoute();
	const MonsterPackDef def = PackDef(route, MonsterAggroKind::Detect);
	PackState p;
	PackBrain::Spawn(p, def, { 11, 12, 13, 14 }, PackBrain::FilePositions(route, 1, 4, 150.f), 0);

	// 머리가 몇 걸음 걸었다 - 발자국
	for (int32 i = 1; i <= 20; i++)
		p.trail.Record(-10.f * i, 4000.f);

	// 셋째(rank 2) · 넷째(rank 3) 의 목표
	const BrainOutput before2 = PackBrain::PatrolGoal(p, def, 2, 0.f, 0.f);
	const BrainOutput before3 = PackBrain::PatrolGoal(p, def, 3, 0.f, 0.f);

	EXPECT_TRUE(PackBrain::OnMemberDied(p, 0, 5 * SEC));
	EXPECT_EQ(PackBrain::HeadIndex(p), 1);

	// rank 가 하나씩 당겨졌다 - 셋째는 이제 rank 1, 넷째는 rank 2. 목표는 1 cm 안에서 그대로
	const BrainOutput after2 = PackBrain::PatrolGoal(p, def, 1, 0.f, 0.f);
	const BrainOutput after3 = PackBrain::PatrolGoal(p, def, 2, 0.f, 0.f);
	EXPECT_LT(Dist(before2.goalX, before2.goalY, after2.goalX, after2.goalY), 1.f);
	EXPECT_LT(Dist(before3.goalX, before3.goalY, after3.goalX, after3.goalY), 1.f);

	// 다 죽으면 전멸
	EXPECT_TRUE(PackBrain::OnMemberDied(p, 1, 6 * SEC));
	EXPECT_TRUE(PackBrain::OnMemberDied(p, 2, 6 * SEC));
	EXPECT_FALSE(PackBrain::OnMemberDied(p, 3, 6 * SEC));
	EXPECT_EQ(PackBrain::HeadIndex(p), -1);
}

TEST(PackBrain, 머리는_길의_점에_닿으면_멈추지_않고_다음_점으로_돌고_뒤는_발자국을_밟는다)
{
	const PatrolRoute route = StraightRoute();
	const MonsterPackDef def = PackDef(route, MonsterAggroKind::Detect);
	PackState p;
	PackBrain::Spawn(p, def, { 11, 12, 13, 14 }, PackBrain::FilePositions(route, 0, 4, 150.f), 0);
	ASSERT_EQ(p.waypoint, 1u);

	// 점 1 (0, 4000) 앞 40 cm - 다음 점(2)으로 넘어간다
	BrainOutput head = PackBrain::PatrolGoal(p, def, 0, 0.f, 3960.f);
	EXPECT_EQ(p.waypoint, 2u);
	EXPECT_FLOAT_EQ(head.goalX, -400.f);

	// 고리 - 마지막 점 다음은 첫 점
	p.waypoint = 3;
	head = PackBrain::PatrolGoal(p, def, 0, -400.f, 30.f);
	EXPECT_EQ(p.waypoint, 0u);

	// 뒤의 몸 - 머리 발자국에서 rank × 150 뒤
	p.trail.Seed({ { 0.f, 0.f }, { 0.f, 600.f } });
	const BrainOutput follower = PackBrain::PatrolGoal(p, def, 2, 0.f, 0.f);
	EXPECT_NEAR(follower.goalY, 300.f, 0.01f);
}

TEST(PackBrain, 실명한_몸은_감지하지_못하고_전원이_오래_막히면_돌아간다)
{
	const PatrolRoute route = StraightRoute();
	const MonsterPackDef def = PackDef(route, MonsterAggroKind::Detect);
	PackState p;
	PackBrain::Spawn(p, def, { 11, 12 }, PackBrain::FilePositions(route, 1, 2, 150.f), 0);

	// Room 은 실명한 몸의 감지 반경을 0 으로 넘긴다
	vector<PackMemberView> living = { View(0, 0.f, 4000.f), View(1, 0.f, 3850.f) };
	living[0].aggroRadiusCm = 0.f;
	living[1].aggroRadiusCm = 0.f;
	vector<BrainPlayer> players = { { 77, 0.f, 4100.f } };
	PackInput in;
	in.def = &def;
	in.living = &living;
	in.players = &players;
	in.nowUs = 2 * SEC;
	EXPECT_EQ(PackBrain::Think(p, in).aggroTargetId, 0u);

	// 보이게 되면 교전, 그다음 전원이 막혀 6 초 → 돌아간다
	living[0].aggroRadiusCm = 600.f;
	PackBrain::Think(p, in);
	ASSERT_EQ(p.mode, PackMode::Engage);

	for (PackMemberView& v : living)
		v.stuckAccumUs = MonsterConfig::STUCK_US;
	for (uint64 t = 0; t <= MonsterConfig::PACK_STUCK_GIVEUP_US; t += 100 * MS)
	{
		in.nowUs = 3 * SEC + t;
		PackBrain::Think(p, in);
	}
	EXPECT_EQ(p.mode, PackMode::Return);
}
