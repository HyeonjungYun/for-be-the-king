#include "pch.h"
#include "MonsterTable.h"
#include "MonsterBrain.h"
#include "TowerTable.h"
#include "WallTable.h"
#include "EntryPointTable.h"
#include "ItemTable.h"
#include "ItemRoller.h"
#include "CsvTable.h"
#include "SkillTable.h"
#include "Player.h"

// design/gdd/monster-ai.md § Acceptance Criteria - 이름 끝의 AC 번호가 그 행이다.
// MonsterBrain · MonsterMover · MonsterDrop 은 Room 을 모르는 순수 로직이라 시계 · 플레이어 · 벽을 주입한다.

namespace
{
	constexpr uint64 MS = 1000;
	constexpr uint64 SEC = 1'000'000;

	const char* TYPES_HEADER = "monster_type_id,name_key,tier,max_hp,attack_power,attack_speed,attack_range_cm,armor,magic_resist,resistance,"
		"move_speed_cms,radius_cm,aggro_radius_cm,leash_radius_cm,search_s,give_up_s,assist_radius_cm,displace_immune,skill_id,drop_tier,drop_chance_pct,drop_rolls\n";
	const char* SPAWNS_HEADER = "spawn_id,floor_id,monster_type_id,x,y,z,yaw,group_id,respawn_s\n";

	// monster_types.csv 초기값 들개 행 - 바꿀 열만 넘긴다
	string HoundRow(const string& speed = "300", const string& radius = "40", const string& leash = "1800",
		const string& resistance = "0", const string& skill = "0", const string& dropTier = "none", uint32 id = 1)
	{
		return to_string(id) + ",monster.hound,field,120,6,1.0,150,0,0," + resistance + "," + speed + "," + radius + ",600," + leash
			+ ",2,6,1000,0," + skill + "," + dropTier + ",30,1\n";
	}

	MonsterTypeDef Hound()
	{
		MonsterTypeDef t;
		t.typeId = 1;
		t.nameKey = "monster.hound";
		t.maxHp = 120;
		t.attackPower = 6;
		t.attackRangeCm = 150.f;
		t.moveSpeedCms = 300.f;
		t.radiusCm = 40.f;
		t.aggroRadiusCm = 600.f;
		t.leashRadiusCm = 1800.f;
		t.searchS = 2.f;
		t.giveUpS = 6.f;
		t.assistRadiusCm = 1000.f;
		t.drops = true;
		t.dropTier = ChestTier::Open;
		t.dropChancePct = 30;
		t.dropRolls = 1;
		return t;
	}

	// 몬스터는 (0, 0) 에 서 있고 집도 (0, 0) - 바꿀 것만 바꾼다
	BrainInput In(uint64 now, const vector<BrainPlayer>& players, bool clear = true, float x = 0.f, float y = 0.f)
	{
		BrainInput in;
		in.nowUs = now;
		in.x = x;
		in.y = y;
		in.players = &players;
		in.lineClear = [clear](float, float, float, float) { return clear; };
		return in;
	}

	// 잠금이 풀린 Idle (Reset 직후 1 초는 감지하지 않는다)
	MonsterBrainState FreshIdle(uint64 now)
	{
		MonsterBrainState s;
		MonsterBrain::Reset(s, now - 2 * SEC);
		return s;
	}

	MonsterBrainState Chasing(uint64 now, uint64 targetId)
	{
		MonsterBrainState s = FreshIdle(now);
		MonsterBrain::Alert(s, targetId, now);
		return s;
	}

	// 실제 데이터 - 부팅 순서 그대로 (GameServer.cpp)
	bool LoadLiveWorld(uint32 floorCount, OUT string& error)
	{
		const string dir = ItemTable::DefaultDataDir();
		// 실제 풀은 실제 로스터(skills.csv)를 가리킨다 - 코드 픽스처(Init)로는 풀 검사가 실패한다 (2026-10-10)
		return SkillTable::Load(dir, OUT error)
			&& ItemTable::Load(dir, OUT error)
			&& FloorItemPool::Load(dir, OUT error)
			&& EntryPointTable::Load(dir, floorCount, OUT error)
			&& WallTable::Load(dir, floorCount, EntryPointTable::All(), OUT error)
			&& TowerTable::Load(dir, floorCount, OUT error);
	}

	// 합성 세계 - 진입 지점 (0, 0) 하나 · 타워 (10000, 0) 하나 · 벽 없음 · 층 2개
	void SyntheticWorld()
	{
		string error;
		SkillTable::Init();
		ASSERT_TRUE(EntryPointTable::Parse("entry_id,floor_id,x,y,z,yaw\n1,0,0,0,100,0\n2,1,0,0,100,0\n", 2, OUT error)) << error;
		ASSERT_TRUE(WallTable::Parse(nullptr, nullptr, 2, EntryPointTable::All(), OUT error)) << error;
		ASSERT_TRUE(TowerTable::Parse("tower_id,floor_id,x,y\nTa,0,10000,0\n", 2, OUT error)) << error;
	}

	bool ParseSpawn(const string& typeRow, const string& spawnRow, OUT string& error)
	{
		const string types = string(TYPES_HEADER) + typeRow;
		const string spawns = string(SPAWNS_HEADER) + spawnRow;
		return MonsterTable::Parse(&types, &spawns, 2, OUT error);
	}

	float Dist(float ax, float ay, float bx, float by)
	{
		return ::sqrtf((ax - bx) * (ax - bx) + (ay - by) * (ay - by));
	}
}

/*-------- 데이터 --------*/

TEST(MonsterTable, 실제_데이터_7종_8자리_무리7개가_1층_벽_진입_타워_검사를_통과한다_AC_MA_01_02)
{
	string error;
	ASSERT_TRUE(LoadLiveWorld(4, OUT error)) << error;
	ASSERT_TRUE(MonsterTable::Load(ItemTable::DefaultDataDir(), 4, OUT error)) << error;

	// 2026-10-11 몬스터 개선 - 들판의 혼자 다니던 들개 10 자리가 무리 7 개(우두머리 1 + 기본 3)로 바뀌었다.
	// 집 지키는 병사 7 · 보스 1 은 그대로 (guard). 들개(1)는 테스트 더미용으로 종류만 남는다
	EXPECT_EQ(MonsterTable::TypeCount(), 7u);
	EXPECT_EQ(MonsterTable::Spawns().size(), 8u);
	EXPECT_EQ(MonsterTable::Packs().size(), 7u);

	// 몬스터는 플레이어보다 느리다 - 0.9 × 340 (Core Rule 1)
	for (uint32 id = 1; id <= 7; id++)
	{
		const MonsterTypeDef* t = MonsterTable::FindType(id);
		ASSERT_NE(t, nullptr);
		EXPECT_LE(t->moveSpeedCms, 306.f);
	}

	// F5 - 주기당 몬스터 드랍 공급 ≤ 상자의 30 % (AC-MA-52). 기대 개수 open 1.3 · house 2.7 (floor-item-pool F1).
	// 무리는 우두머리 + 기본 member_count 를 센다
	float supply = 0.f;
	auto add = [&supply](const MonsterTypeDef* t, float count)
	{
		const float perRoll = (t->dropTier == ChestTier::House) ? 2.7f : 1.3f;
		supply += count * (t->drops ? 1.f : 0.f) * t->dropChancePct / 100.f * t->dropRolls * perRoll;
	};
	for (const MonsterSpawnDef& s : MonsterTable::Spawns())
		add(MonsterTable::FindType(s.typeId), 1.f);
	for (const MonsterPackDef& p : MonsterTable::Packs())
	{
		add(MonsterTable::FindType(p.leaderTypeId), 1.f);
		add(MonsterTable::FindType(p.memberTypeId), static_cast<float>(p.memberCount));
	}
	EXPECT_LE(supply / 48.f, 0.30f) << supply;
}

TEST(MonsterTable, 종류_범위_밖은_부팅을_거부하고_경계값은_통과한다_AC_MA_01)
{
	SyntheticWorld();
	string error;
	auto typesOnly = [&error](const string& rows)
	{
		const string types = string(TYPES_HEADER) + rows;
		return MonsterTable::Parse(&types, nullptr, 2, OUT error);
	};

	EXPECT_TRUE(typesOnly(HoundRow())) << error;
	EXPECT_TRUE(typesOnly(HoundRow("306"))) << error;			// 0.9 × 340
	EXPECT_FALSE(typesOnly(HoundRow("307")));					// 도망이 불가능해진다
	EXPECT_TRUE(typesOnly(HoundRow("300", "150"))) << error;
	EXPECT_FALSE(typesOnly(HoundRow("300", "151")));			// 문 · 기둥 틈을 못 지난다
	EXPECT_FALSE(typesOnly(HoundRow("300", "40", "600")));		// leash ≤ aggro
	EXPECT_FALSE(typesOnly(HoundRow("300", "40", "1800", "1.01")));
	EXPECT_FALSE(typesOnly(HoundRow("300", "40", "1800", "0", "9999")));	// 모르는 스킬
	EXPECT_TRUE(typesOnly(HoundRow("240", "110", "2200", "0.5", "9001"))) << error;
	EXPECT_FALSE(typesOnly(HoundRow() + HoundRow()));			// id 중복
	EXPECT_FALSE(typesOnly(HoundRow("300", "40", "1800", "0", "0", "chest")));	// 모르는 drop_tier
}

TEST(MonsterTable, 자리는_진입_20m_타워_35m_경계_10m_를_지키고_경계값은_통과한다_AC_MA_02)
{
	SyntheticWorld();
	string error;
	const string hound = HoundRow();

	EXPECT_TRUE(ParseSpawn(hound, "301,0,1,2000,0,100,0,0,1800\n", OUT error)) << error;		// 진입에서 정확히 20 m
	EXPECT_FALSE(ParseSpawn(hound, "301,0,1,1999,0,100,0,0,1800\n", OUT error));
	EXPECT_TRUE(ParseSpawn(hound, "301,0,1,6500,0,100,0,0,1800\n", OUT error)) << error;		// 타워에서 정확히 35 m
	EXPECT_FALSE(ParseSpawn(hound, "301,0,1,6501,0,100,0,0,1800\n", OUT error));
	EXPECT_TRUE(ParseSpawn(hound, "301,0,1,0,13000,100,0,0,1800\n", OUT error)) << error;		// 경계에서 정확히 10 m
	EXPECT_FALSE(ParseSpawn(hound, "301,0,1,0,13001,100,0,0,1800\n", OUT error));
	EXPECT_FALSE(ParseSpawn(hound, "301,0,1,0,14001,100,0,0,1800\n", OUT error));				// 층 밖
	EXPECT_FALSE(ParseSpawn(hound, "301,0,1,0,5000,100,0,0,59\n", OUT error));					// respawn_s < 60
	EXPECT_TRUE(ParseSpawn(hound, "301,0,1,0,5000,100,0,0,60\n", OUT error)) << error;
	EXPECT_FALSE(ParseSpawn(hound, "301,0,1,0,5000,100,0,0,1800\n302,0,1,0,5000,100,0,0,1800\n", OUT error));	// 같은 자리 같은 종류
	EXPECT_FALSE(ParseSpawn(hound, "301,0,1,0,5000,100,0,0,1800\n301,0,1,0,6000,100,0,0,1800\n", OUT error));	// spawn_id 중복
	EXPECT_FALSE(ParseSpawn(hound, "301,0,7,0,5000,100,0,0,1800\n", OUT error));					// 모르는 종류

	// 드랍할 풀이 그 층에 없다 - 1층(0)은 실제 풀이 있고 2층(1)은 없다. 실제 풀은 실제 종류 표가 필요하다
	ASSERT_TRUE(SkillTable::Load(ItemTable::DefaultDataDir(), OUT error) && ItemTable::Load(ItemTable::DefaultDataDir(), OUT error)) << error;
	ASSERT_TRUE(FloorItemPool::Load(ItemTable::DefaultDataDir(), OUT error)) << error;
	const string dropsOpen = HoundRow("300", "40", "1800", "0", "0", "open");
	EXPECT_TRUE(ParseSpawn(dropsOpen, "301,0,1,0,5000,100,0,0,1800\n", OUT error)) << error;
	EXPECT_FALSE(ParseSpawn(dropsOpen, "301,1,1,0,5000,100,0,0,1800\n", OUT error));

	// 집 반경 원이 벽과 겹친다
	const string walls = "floor_id,kind,cx,cy,half_length,half_thickness,yaw\n0,wall,5000,5000,200,25,0\n";
	const string versions = "floor_id,data_version\n0,test\n";
	ASSERT_TRUE(WallTable::Parse(&walls, &versions, 2, EntryPointTable::All(), OUT error)) << error;
	EXPECT_FALSE(ParseSpawn(hound, "301,0,1,5000,5060,100,0,0,1800\n", OUT error));		// 면에서 35 cm < r 40
	EXPECT_TRUE(ParseSpawn(hound, "301,0,1,5000,5070,100,0,0,1800\n", OUT error)) << error;	// 면에서 45 cm
}

TEST(MonsterTable, 파일이_없으면_몬스터_0_자리_파일만_있으면_거부_AC_MA_03)
{
	SyntheticWorld();
	string error;
	EXPECT_TRUE(MonsterTable::Parse(nullptr, nullptr, 2, OUT error)) << error;
	EXPECT_TRUE(MonsterTable::Spawns().empty());

	const string spawns = string(SPAWNS_HEADER) + "301,0,1,0,5000,100,0,0,1800\n";
	EXPECT_FALSE(MonsterTable::Parse(nullptr, &spawns, 2, OUT error));

	const string types = string(TYPES_HEADER) + HoundRow();
	EXPECT_TRUE(MonsterTable::Parse(&types, nullptr, 2, OUT error)) << error;
	EXPECT_TRUE(MonsterTable::Spawns().empty());
}

TEST(TowerTable, 실제_타워_3개를_읽고_파일이_없으면_0개)
{
	string error;
	ASSERT_TRUE(TowerTable::Load(ItemTable::DefaultDataDir(), 4, OUT error)) << error;
	ASSERT_EQ(TowerTable::All().size(), 3u);
	EXPECT_EQ(TowerTable::All()[0].towerId, "Ta");

	EXPECT_TRUE(TowerTable::Load("C:\\no\\such\\dir", 4, OUT error));
	EXPECT_TRUE(TowerTable::All().empty());

	EXPECT_FALSE(TowerTable::Parse("tower_id,floor_id,x,y\nTa,0,0,0\nTa,0,10,0\n", 4, OUT error));	// 중복
	EXPECT_FALSE(TowerTable::Parse("tower_id,floor_id,x,y\nTa,0,15000,0\n", 4, OUT error));		// 층 밖
}

/*-------- 상태기 --------*/

TEST(MonsterBrain, 감지는_반경_경계를_포함하고_벽_뒤와_잠금_중에는_못_한다_AC_MA_10)
{
	const MonsterTypeDef hound = Hound();
	const uint64 now = 100 * SEC;

	for (float d : { 599.f, 600.f })
	{
		MonsterBrainState s = FreshIdle(now);
		const vector<BrainPlayer> players = { { 7, d, 0.f } };
		const BrainOutput out = MonsterBrain::Think(s, hound, In(now, players));
		EXPECT_EQ(s.state, MonsterState::Chase) << d;
		EXPECT_EQ(s.targetId, 7u);
		EXPECT_TRUE(out.aggroed);
	}

	{
		MonsterBrainState s = FreshIdle(now);
		const vector<BrainPlayer> players = { { 7, 601.f, 0.f } };
		MonsterBrain::Think(s, hound, In(now, players));
		EXPECT_EQ(s.state, MonsterState::Idle);
	}

	{
		MonsterBrainState s = FreshIdle(now);
		const vector<BrainPlayer> players = { { 7, 300.f, 0.f } };
		MonsterBrain::Think(s, hound, In(now, players, /*clear*/ false));
		EXPECT_EQ(s.state, MonsterState::Idle);
	}

	// 태어난 뒤 1 초 - 스폰킬 방지 잠금
	{
		MonsterBrainState s;
		MonsterBrain::Reset(s, now);
		const vector<BrainPlayer> players = { { 7, 300.f, 0.f } };
		MonsterBrain::Think(s, hound, In(now + 999 * MS, players));
		EXPECT_EQ(s.state, MonsterState::Idle);
		MonsterBrain::Think(s, hound, In(now + 1000 * MS, players));
		EXPECT_EQ(s.state, MonsterState::Chase);
	}
}

TEST(MonsterBrain, 감지_밖에서_맞으면_쫓고_추격_한계_밖에서_쏘면_쫓지_않는다_AC_MA_11)
{
	const MonsterTypeDef hound = Hound();
	const uint64 now = 100 * SEC;

	{
		MonsterBrainState s = FreshIdle(now);
		const vector<BrainPlayer> players = { { 7, 1000.f, 0.f } };
		MonsterBrain::OnDamaged(s, 7, 10, 1000.f, 0.f, now);
		MonsterBrain::Think(s, hound, In(now, players));
		EXPECT_EQ(s.state, MonsterState::Chase);
		EXPECT_EQ(s.targetId, 7u);
		EXPECT_FLOAT_EQ(MonsterBrain::ThreatOf(s, 7), 10.f);
	}

	{
		MonsterBrainState s = FreshIdle(now);
		const vector<BrainPlayer> players = { { 7, 1801.f, 0.f } };
		MonsterBrain::OnDamaged(s, 7, 10, 1801.f, 0.f, now);
		MonsterBrain::Think(s, hound, In(now, players));
		EXPECT_NE(s.state, MonsterState::Chase);
		EXPECT_TRUE(s.threat.empty());
	}
}

TEST(MonsterBrain, 집에서_추격_한계를_넘으면_복귀하고_위협을_비운다_AC_MA_12)
{
	const MonsterTypeDef hound = Hound();
	const uint64 now = 100 * SEC;
	const vector<BrainPlayer> players = { { 7, 1850.f, 0.f } };

	{
		MonsterBrainState s = Chasing(now, 7);
		MonsterBrain::Think(s, hound, In(now, players, true, 1800.f, 0.f));
		EXPECT_EQ(s.state, MonsterState::Chase);
	}

	{
		MonsterBrainState s = Chasing(now, 7);
		const BrainOutput out = MonsterBrain::Think(s, hound, In(now, players, true, 1801.f, 0.f));
		EXPECT_EQ(s.state, MonsterState::Return);
		EXPECT_TRUE(out.enteredReturn);
		EXPECT_TRUE(s.threat.empty());
		EXPECT_EQ(s.targetId, 0u);
	}
}

TEST(MonsterBrain, 시야가_500ms_끊기면_수색하고_다시_보면_추격_못_찾으면_복귀_AC_MA_13)
{
	const MonsterTypeDef hound = Hound();
	const uint64 t0 = 100 * SEC;
	const vector<BrainPlayer> seen = { { 7, 400.f, 100.f } };
	const vector<BrainPlayer> hidden = { { 7, 500.f, 300.f } };

	MonsterBrainState s = Chasing(t0, 7);
	MonsterBrain::Think(s, hound, In(t0, seen));						// 본다 - lastSeen = (400, 100)

	MonsterBrain::Think(s, hound, In(t0 + 100 * MS, hidden, false));	// 끊김 시작
	MonsterBrain::Think(s, hound, In(t0 + 599 * MS, hidden, false));	// 499 ms
	EXPECT_EQ(s.state, MonsterState::Chase);

	BrainOutput out = MonsterBrain::Think(s, hound, In(t0 + 600 * MS, hidden, false));
	EXPECT_EQ(s.state, MonsterState::Search);
	EXPECT_FLOAT_EQ(s.lastSeenX, 400.f);
	EXPECT_FLOAT_EQ(s.lastSeenY, 100.f);
	EXPECT_TRUE(out.hasGoal);
	EXPECT_FLOAT_EQ(out.goalX, 400.f);

	// 다시 보인다 (감지 × 1.5 = 900 안) - 추격
	{
		MonsterBrainState again = s;
		MonsterBrain::Think(again, hound, In(t0 + 700 * MS, hidden, true));
		EXPECT_EQ(again.state, MonsterState::Chase);
	}

	// 마지막 본 자리에 닿고 search_s 2 초 - 복귀
	MonsterBrain::Think(s, hound, In(t0 + 700 * MS, hidden, false, 400.f, 100.f));
	EXPECT_EQ(s.state, MonsterState::Search);
	MonsterBrain::Think(s, hound, In(t0 + 2699 * MS, hidden, false, 400.f, 100.f));
	EXPECT_EQ(s.state, MonsterState::Search);
	MonsterBrain::Think(s, hound, In(t0 + 2700 * MS, hidden, false, 400.f, 100.f));
	EXPECT_EQ(s.state, MonsterState::Return);
}

TEST(MonsterBrain, 교전_없이_give_up_s_를_넘으면_복귀_사거리_안이면_리셋_AC_MA_14)
{
	const MonsterTypeDef hound = Hound();
	const uint64 t0 = 100 * SEC;
	const vector<BrainPlayer> distant = { { 7, 1000.f, 0.f } };		// 사거리 150 밖 · 시야 clear ("far" 는 windows.h 매크로다)
	const vector<BrainPlayer> inReach = { { 7, 100.f, 0.f } };

	{
		MonsterBrainState s = Chasing(t0, 7);
		MonsterBrain::Think(s, hound, In(t0 + 5990 * MS, distant));
		EXPECT_EQ(s.state, MonsterState::Chase);
		MonsterBrain::Think(s, hound, In(t0 + 6010 * MS, distant));
		EXPECT_EQ(s.state, MonsterState::Return);
	}

	{
		MonsterBrainState s = Chasing(t0, 7);
		const BrainOutput out = MonsterBrain::Think(s, hound, In(t0 + 5000 * MS, inReach));	// 사거리 안 - 공격 · 리셋
		EXPECT_EQ(out.attackTargetId, 7u);
		MonsterBrain::Think(s, hound, In(t0 + 10'000 * MS, distant));
		EXPECT_EQ(s.state, MonsterState::Chase);
		MonsterBrain::Think(s, hound, In(t0 + 11'010 * MS, distant));
		EXPECT_EQ(s.state, MonsterState::Return);
	}
}

TEST(MonsterBrain, 목표는_점수가_1_25배를_넘을_때만_바뀌고_windup_중엔_안_바뀐다_AC_MA_15)
{
	const MonsterTypeDef hound = Hound();
	const uint64 now = 100 * SEC;
	const vector<BrainPlayer> players = { { 1, 800.f, 0.f }, { 2, 100.f, 0.f } };	// A 는 w 1.0 · B 는 사거리 안 w 2.0

	auto make = [now](float threatB)
	{
		MonsterBrainState s = FreshIdle(now);
		s.state = MonsterState::Chase;
		s.targetId = 1;
		s.lastEngagedUs = now;
		s.threat = { { 1, 100.f }, { 2, threatB } };
		return s;
	};

	{
		MonsterBrainState s = make(41.f);			// 82 < 125
		MonsterBrain::Think(s, hound, In(now, players));
		EXPECT_EQ(s.targetId, 1u);
	}
	{
		MonsterBrainState s = make(63.f);			// 126 ≥ 125
		MonsterBrain::Think(s, hound, In(now, players));
		EXPECT_EQ(s.targetId, 2u);
	}
	{
		MonsterBrainState s = make(63.f);
		BrainInput in = In(now, players);
		in.windingUp = true;
		MonsterBrain::Think(s, hound, in);
		EXPECT_EQ(s.targetId, 1u);
	}
	{
		// A 가 2·R_a(12 m) 밖 - 100 × 0.25 = 25, B 82 ≥ 31.25
		const vector<BrainPlayer> fled = { { 1, 1300.f, 0.f }, { 2, 100.f, 0.f } };
		MonsterBrainState s = make(41.f);
		MonsterBrain::Think(s, hound, In(now, fled));
		EXPECT_EQ(s.targetId, 2u);
	}
}

TEST(MonsterBrain, 목표가_사라지면_다음_목표로_아무도_없으면_바로_복귀_AC_MA_16)
{
	const MonsterTypeDef hound = Hound();
	const uint64 now = 100 * SEC;

	MonsterBrainState s = FreshIdle(now);
	s.state = MonsterState::Chase;
	s.targetId = 1;
	s.lastEngagedUs = now;
	s.threat = { { 1, 50.f }, { 2, 10.f } };

	const vector<BrainPlayer> onlyB = { { 2, 300.f, 0.f } };
	MonsterBrain::Think(s, hound, In(now, onlyB));
	EXPECT_EQ(s.state, MonsterState::Chase);
	EXPECT_EQ(s.targetId, 2u);
	EXPECT_FLOAT_EQ(MonsterBrain::ThreatOf(s, 1), 0.f);

	const vector<BrainPlayer> nobody;
	MonsterBrain::Think(s, hound, In(now + 100 * MS, nobody));
	EXPECT_EQ(s.state, MonsterState::Return);
}

TEST(MonsterBrain, 복귀는_어그로가_없고_도착하면_만피_잠금_10초면_순간이동_AC_MA_17)
{
	const MonsterTypeDef hound = Hound();
	const uint64 t0 = 100 * SEC;
	const vector<BrainPlayer> players = { { 7, 1500.f, 0.f } };

	MonsterBrainState s = Chasing(t0, 7);
	MonsterBrain::Think(s, hound, In(t0, players, true, 1801.f, 0.f));
	ASSERT_EQ(s.state, MonsterState::Return);

	// 복귀 중 피격 - 위협 없음 · 추격 없음
	MonsterBrain::OnDamaged(s, 7, 50, 1500.f, 0.f, t0 + 100 * MS);
	BrainOutput out = MonsterBrain::Think(s, hound, In(t0 + 200 * MS, players, true, 1000.f, 0.f));
	EXPECT_EQ(s.state, MonsterState::Return);
	EXPECT_TRUE(s.threat.empty());
	EXPECT_FLOAT_EQ(out.speedMult, 1.5f);

	// 집 50 cm 안 - Idle · 만피 · 1 초 잠금
	out = MonsterBrain::Think(s, hound, In(t0 + 300 * MS, players, true, 50.f, 0.f));
	EXPECT_EQ(s.state, MonsterState::Idle);
	EXPECT_TRUE(out.resetOnArrive);
	EXPECT_FALSE(out.snapHome);

	const vector<BrainPlayer> inRange = { { 7, 300.f, 0.f } };
	MonsterBrain::Think(s, hound, In(t0 + 1299 * MS, inRange));
	EXPECT_EQ(s.state, MonsterState::Idle);
	MonsterBrain::Think(s, hound, In(t0 + 1300 * MS, inRange));
	EXPECT_EQ(s.state, MonsterState::Chase);

	// 10 초 안에 못 돌아오면 순간 이동
	MonsterBrainState stuck = Chasing(t0, 7);
	MonsterBrain::Think(stuck, hound, In(t0, players, true, 1801.f, 0.f));
	out = MonsterBrain::Think(stuck, hound, In(t0 + 9999 * MS, players, true, 1700.f, 0.f));
	EXPECT_EQ(stuck.state, MonsterState::Return);
	out = MonsterBrain::Think(stuck, hound, In(t0 + 10'000 * MS, players, true, 1700.f, 0.f));
	EXPECT_EQ(stuck.state, MonsterState::Idle);
	EXPECT_TRUE(out.snapHome);
	EXPECT_TRUE(out.resetOnArrive);
}

TEST(MonsterBrain, 죽은_몬스터는_판단도_위협도_없다_AC_MA_20)
{
	const MonsterTypeDef hound = Hound();
	const uint64 now = 100 * SEC;
	const vector<BrainPlayer> players = { { 7, 100.f, 0.f } };

	MonsterBrainState s = Chasing(now, 7);
	MonsterBrain::Kill(s, now);
	MonsterBrain::OnDamaged(s, 7, 99, 100.f, 0.f, now);
	const BrainOutput out = MonsterBrain::Think(s, hound, In(now, players));

	EXPECT_EQ(s.state, MonsterState::Dead);
	EXPECT_TRUE(s.threat.empty());
	EXPECT_EQ(out.attackTargetId, 0u);
	EXPECT_FALSE(out.hasGoal);
}

TEST(MonsterBrain, 무리_호출은_쉬는_동료만_따라온다_AC_MA_21)
{
	const uint64 now = 100 * SEC;

	MonsterBrainState idle = FreshIdle(now);
	EXPECT_TRUE(MonsterBrain::Alert(idle, 7, now));
	EXPECT_EQ(idle.state, MonsterState::Chase);
	EXPECT_EQ(idle.targetId, 7u);

	MonsterBrainState locked;
	MonsterBrain::Reset(locked, now);				// 잠금 1 초
	EXPECT_FALSE(MonsterBrain::Alert(locked, 7, now));

	MonsterBrainState busy = Chasing(now, 3);
	EXPECT_FALSE(MonsterBrain::Alert(busy, 7, now));
	EXPECT_EQ(busy.targetId, 3u);
}

TEST(MonsterBrain, 위협표는_32개를_넘지_않고_가장_낮은_항목을_밀어낸다)
{
	MonsterBrainState s;
	for (uint64 id = 1; id <= 32; id++)
		MonsterBrain::AddThreat(s, id, static_cast<float>(id));

	MonsterBrain::AddThreat(s, 99, 0.5f);			// 가장 낮은(1)보다 작다 - 못 들어온다
	EXPECT_EQ(s.threat.size(), 32u);
	EXPECT_FLOAT_EQ(MonsterBrain::ThreatOf(s, 99), 0.f);

	MonsterBrain::AddThreat(s, 100, 5.f);
	EXPECT_EQ(s.threat.size(), 32u);
	EXPECT_FLOAT_EQ(MonsterBrain::ThreatOf(s, 100), 5.f);
	EXPECT_FLOAT_EQ(MonsterBrain::ThreatOf(s, 1), 0.f);
}

/*-------- 이동 --------*/

namespace
{
	// 1 초를 33 ms 걸음으로 - Room::FlushMoves 와 같은 주기
	StepOutput Walk(MonsterBrainState& s, float& x, float& y, float gx, float gy, float stop, float speed, uint64& now, uint64 durationUs,
		const SweepBlockedFn& blocked = nullptr, float radius = 40.f)
	{
		StepOutput last;
		const uint64 end = now + durationUs;
		while (now < end)
		{
			now += 33 * MS;
			StepInput in;
			in.nowUs = now;
			in.dtUs = 33 * MS;
			in.x = x;
			in.y = y;
			in.goalX = gx;
			in.goalY = gy;
			in.stopDistCm = stop;
			in.speedCms = speed;
			in.radiusCm = radius;
			in.blocked = blocked;
			last = MonsterMover::Step(s, in);
			x = last.x;
			y = last.y;
		}
		return last;
	}

	WallMap MapOf(vector<WallBox> walls)
	{
		WallMap map;
		string error;
		EXPECT_TRUE(map.Build(move(walls), OUT error)) << error;
		return map;
	}

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
}

TEST(MonsterMover, 벽이_없으면_1초에_v_eff_만큼_가고_정지_거리에서_멈춘다_AC_MA_30)
{
	MonsterBrainState s;
	float x = 0.f, y = 0.f;
	uint64 now = 100 * SEC;

	Walk(s, x, y, 5000.f, 0.f, 0.f, 300.f, now, 990 * MS);			// 30 걸음 = 0.99 s
	EXPECT_NEAR(x, 297.f, 1.f);
	EXPECT_NEAR(y, 0.f, 0.01f);

	MonsterBrainState slowed;
	float sx = 0.f, sy = 0.f;
	Walk(slowed, sx, sy, 5000.f, 0.f, 0.f, 280.f * 0.6f, now, 990 * MS);	// 병사 감속 0.4
	EXPECT_NEAR(sx, 168.f * 0.99f, 1.f);

	// 정지 거리 120 (= 150 × 0.8) - 목표 앞에서 멈추고 더 다가가지 않는다
	MonsterBrainState stop;
	float px = 0.f, py = 0.f;
	const StepOutput last = Walk(stop, px, py, 300.f, 0.f, 120.f, 300.f, now, 2 * SEC);
	EXPECT_NEAR(300.f - px, 120.f, 1.f);
	EXPECT_TRUE(last.arrived);
	EXPECT_EQ(stop.stuckAccumUs, 0u);
}

TEST(MonsterMover, 짧은_벽은_돌아가고_긴_벽은_2초_막힘으로_복귀한다_AC_MA_31)
{
	// 짧은 벽 2 m - 목표 쪽 정면을 막는다. 미끄러져 돌아 목표에 닿는다
	{
		const WallMap map = MapOf({ Box(500.f, 0.f, 25.f, 100.f, 0.f) });
		auto blocked = [&map](float ax, float ay, float bx, float by, float r) { return map.SweepBlocked(ax, ay, bx, by, r); };

		MonsterBrainState s;
		float x = 0.f, y = 0.f;
		uint64 now = 100 * SEC;
		Walk(s, x, y, 1000.f, 0.f, 0.f, 300.f, now, 6 * SEC, blocked);
		EXPECT_LT(Dist(x, y, 1000.f, 0.f), 5.f) << x << "," << y;
		EXPECT_LT(s.stuckAccumUs, MonsterConfig::STUCK_US);
	}

	// 긴 벽 20 m - ±90° 로도 못 넘는다. 2 초 막힘 → Think 가 복귀시킨다 (T3)
	{
		const WallMap map = MapOf({ Box(500.f, 0.f, 25.f, 1000.f, 0.f) });
		auto blocked = [&map](float ax, float ay, float bx, float by, float r) { return map.SweepBlocked(ax, ay, bx, by, r); };

		// 벽 바로 앞에서 시작 - 창(약 1 s) 두 개가 다 막힘이어야 T3 다
		MonsterBrainState s = Chasing(100 * SEC, 7);
		float x = 400.f, y = 0.f;
		uint64 now = 100 * SEC;
		Walk(s, x, y, 1000.f, 0.f, 0.f, 300.f, now, 2200 * MS, blocked);
		EXPECT_GE(s.stuckAccumUs, MonsterConfig::STUCK_US);
		EXPECT_LT(x, 500.f - 25.f - 40.f + 0.5f);					// 관통 0
		EXPECT_LT(::fabsf(y), 1000.f);								// 벽 끝까지 못 갔다

		const vector<BrainPlayer> players = { { 7, 1000.f, 0.f } };
		MonsterBrain::Think(s, Hound(), In(now, players, true, x, y));
		EXPECT_EQ(s.state, MonsterState::Return);
	}
}

TEST(MonsterMover, 더_빠른_목표를_곧게_쫓는_것은_막힘이_아니다_AC_MA_18)
{
	// 목표가 계속 멀어져도(340 > 300) 자기 변위가 목표 방향으로 300 cm/s - 막힘 0
	MonsterBrainState s;
	float x = 0.f, y = 0.f;
	uint64 now = 100 * SEC;
	for (int32 i = 0; i < 3; i++)
	{
		const float targetX = 1000.f + 340.f * (i + 1);
		Walk(s, x, y, targetX, 0.f, 120.f, 300.f, now, SEC);
	}
	EXPECT_EQ(s.stuckAccumUs, 0u);

	// 벽을 따라 옆으로만 미끄러진다(목표 방향 진척 0) - 막힘이 쌓인다
	MonsterBrainState sliding;
	float sx = 0.f, sy = 0.f;
	uint64 t = 100 * SEC;
	auto onlySideways = [](float ax, float, float bx, float, float) { return ::fabsf(bx - ax) > 0.01f; };	// 90° 만 열려 있다
	Walk(sliding, sx, sy, 1000.f, 0.f, 0.f, 300.f, t, 2100 * MS, onlySideways);
	EXPECT_GE(sliding.stuckAccumUs, MonsterConfig::STUCK_US);

	// CC · windup 으로 서 있는 시간은 막힘이 아니다
	MonsterBrainState held;
	float hx = 0.f, hy = 0.f;
	uint64 th = 100 * SEC;
	Walk(held, hx, hy, 1000.f, 0.f, 0.f, 0.f, th, 5 * SEC);
	EXPECT_EQ(held.stuckAccumUs, 0u);
}

TEST(MonsterMover, 층_밖으로_나가지_않고_오래_멈춘_뒤에도_한_걸음이_100ms_를_넘지_않는다_AC_MA_33_35)
{
	MonsterBrainState s;
	float x = 13900.f, y = 0.f;
	uint64 now = 100 * SEC;
	Walk(s, x, y, 20000.f, 0.f, 0.f, 300.f, now, 2 * SEC);
	EXPECT_LE(x, 14000.f - 40.f);
	EXPECT_LE(::fabsf(y), 14000.f - 40.f);

	MonsterBrainState t;
	StepInput in;
	in.nowUs = 200 * SEC;
	in.dtUs = 5 * SEC;								// 서버가 5 초 멈췄다
	in.goalX = 5000.f;
	in.speedCms = 300.f;
	in.radiusCm = 40.f;
	const StepOutput o = MonsterMover::Step(t, in);
	EXPECT_NEAR(o.x, 30.f, 0.01f);					// 300 × 0.1 s
}

/*-------- 드랍 --------*/

TEST(MonsterDrop, 드랍_확률과_개수가_데이터와_맞고_전부_새_아이템이다_AC_MA_50)
{
	string error;
	ASSERT_TRUE(LoadLiveWorld(4, OUT error)) << error;
	ASSERT_TRUE(MonsterTable::Load(ItemTable::DefaultDataDir(), 4, OUT error)) << error;

	mt19937 rng(20261007);
	uint64 nextId = 1;
	auto issuer = [&nextId]() { return nextId++; };

	struct Expect { uint32 typeId; float rate; uint32 minItems; uint32 maxItems; };
	for (const Expect& e : { Expect{ 1, 0.30f, 1, 2 }, Expect{ 2, 0.55f, 1, 2 }, Expect{ 3, 1.00f, 4, 8 } })
	{
		const MonsterTypeDef* type = MonsterTable::FindType(e.typeId);
		ASSERT_NE(type, nullptr);
		const ChestPool* pool = FloorItemPool::Find(0, type->dropTier);
		ASSERT_NE(pool, nullptr);

		int32 bags = 0;
		for (int32 i = 0; i < 10'000; i++)
		{
			const vector<ItemEntry> items = MonsterDrop::Roll(*type, pool, rng, issuer);
			if (items.empty())
				continue;

			bags++;
			EXPECT_GE(items.size(), e.minItems);
			EXPECT_LE(items.size(), e.maxItems);
			for (const ItemEntry& item : items)
			{
				EXPECT_TRUE(item.isNew);
				EXPECT_LE(item.grade, Protocol::ITEM_GRADE_RARE);		// 1층 풀에 영웅 · 전설 없음
			}
		}

		EXPECT_NEAR(bags / 10'000.f, e.rate, 0.02f) << "type " << e.typeId;
	}

	// 풀이 없거나 드랍이 꺼진 종류는 가방을 만들지 않는다
	MonsterTypeDef none = Hound();
	none.drops = false;
	EXPECT_TRUE(MonsterDrop::Roll(none, FloorItemPool::Find(0, ChestTier::Open), rng, issuer).empty());
	EXPECT_TRUE(MonsterDrop::Roll(Hound(), nullptr, rng, issuer).empty());
}
