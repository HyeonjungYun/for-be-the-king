#include "pch.h"
#include "ItemRoller.h"
#include "ItemTable.h"
#include "SkillTable.h"
#include "Player.h"

// 🔴 테스트는 파일을 읽지 않는다 - Server/Data 와 같은 내용의 문자열 픽스처를 쓴다.
//    시드를 고정해 매번 같은 결과가 나온다 (결정론). 허용 오차 ±1%p 는 AC-CH-01 의 기준이다.
namespace
{
	const char* TYPES =
		"id,name_key,equip_slot,archetype,damage_type,grade_mode,attack_range_cm,skill_pool_id\n"
		"101,item.101,1,heavy_weapon,physical,rolled,175,weapon\n"
		"102,item.102,1,light_weapon,physical,rolled,175,weapon\n"
		"103,item.103,1,heavy_weapon,magic,rolled,175,weapon\n"
		"104,item.104,1,light_weapon,magic,rolled,175,weapon\n"
		"301,item.301,3,helmet_scout,none,rolled,0,helmet\n"
		"302,item.302,3,helmet_guard,none,rolled,0,helmet\n"
		"401,item.401,4,armor_heavy,none,rolled,0,armor\n"
		"402,item.402,4,armor_light,none,rolled,0,armor\n"
		"501,item.501,5,boots_sprint,none,rolled,0,boots\n"
		"502,item.502,5,boots_endurance,none,rolled,0,boots\n"
		"601,item.601,6,trinket,none,rolled,0,trinket\n";

	const char* POOLS =
		"pool_id,skill_id,weight\n"
		"weapon,1001,1\n"
		"armor,3001,1\n"
		"boots,2001,1\n"
		"trinket,2001,1\n"
		"trinket,3001,1\n";

	const char* FLOOR_HEADER = "floor_id,tier,category,key,weight\n";

	string TierRows(const char* tier, const char* counts, const char* grades)
	{
		const string t = string("0,") + tier + ",";
		return string(counts) + grades
			+ t + "part,1,28\n" + t + "part,3,22\n" + t + "part,4,22\n" + t + "part,5,23\n" + t + "part,6,5\n"
			+ t + "damage,physical,70\n" + t + "damage,magic,30\n";
	}

	// quick-spec § 5 의 1층 값
	string FloorPool()
	{
		return string(FLOOR_HEADER)
			+ TierRows("open", "0,open,count,1,70\n0,open,count,2,30\n", "0,open,grade,1,90\n0,open,grade,2,10\n")
			+ TierRows("house", "0,house,count,2,45\n0,house,count,3,40\n0,house,count,4,15\n", "0,house,grade,1,70\n0,house,grade,2,30\n");
	}

	void LoadTables(const char* pools = POOLS)
	{
		SkillTable::Init();

		string error;
		ASSERT_TRUE(ItemTable::Parse(TYPES, pools, OUT error)) << error;
		ASSERT_TRUE(FloorItemPool::Parse(FloorPool(), OUT error)) << error;
	}

	InstanceIdIssuer Counter(uint64 start = 1)
	{
		auto next = make_shared<uint64>(start);
		return [next]() { return (*next)++; };
	}

	double Percent(int64 part, int64 whole)
	{
		return 100.0 * static_cast<double>(part) / static_cast<double>(whole);
	}
}

TEST(FloorItemPool, 층과_등급별_풀을_싣고_상한을_계산한다)
{
	LoadTables();

	const ChestPool* open = FloorItemPool::Find(0, ChestTier::Open);
	const ChestPool* house = FloorItemPool::Find(0, ChestTier::House);

	ASSERT_NE(open, nullptr);
	ASSERT_NE(house, nullptr);
	EXPECT_EQ(open->maxCount, 2u);
	EXPECT_EQ(house->maxCount, 4u);

	EXPECT_EQ(FloorItemPool::Find(1, ChestTier::Open), nullptr) << "2층 풀은 아직 없다";
}

TEST(ItemRoller, AC_CH_01_10만_번_롤하면_가중치대로_나오고_영웅_전설은_없다)
{
	LoadTables();
	const ChestPool& pool = *FloorItemPool::Find(0, ChestTier::Open);

	mt19937 rng(20261004);
	InstanceIdIssuer nextId = Counter();

	constexpr int64 N = 100'000;
	map<uint32, int64> byType;
	map<int32, int64> byGrade;
	map<int32, int64> byPart;

	for (int64 i = 0; i < N; i++)
	{
		const ItemEntry e = ItemRoller::RollItem(pool, rng, nextId);
		byType[e.itemTypeId]++;
		byGrade[e.grade]++;
		byPart[ItemTable::Find(e.itemTypeId)->equipSlot]++;
	}

	// 등급 90 : 10, 영웅 · 전설 0 (AC-IE-41)
	EXPECT_NEAR(Percent(byGrade[Protocol::ITEM_GRADE_COMMON], N), 90.0, 1.0);
	EXPECT_NEAR(Percent(byGrade[Protocol::ITEM_GRADE_RARE], N), 10.0, 1.0);
	EXPECT_EQ(byGrade[Protocol::ITEM_GRADE_EPIC], 0);
	EXPECT_EQ(byGrade[Protocol::ITEM_GRADE_LEGENDARY], 0);

	// 부위 28 · 22 · 22 · 23 · 5
	EXPECT_NEAR(Percent(byPart[Protocol::SLOT_WEAPON_PRIMARY], N), 28.0, 1.0);
	EXPECT_NEAR(Percent(byPart[Protocol::SLOT_HELMET], N), 22.0, 1.0);
	EXPECT_NEAR(Percent(byPart[Protocol::SLOT_ARMOR], N), 22.0, 1.0);
	EXPECT_NEAR(Percent(byPart[Protocol::SLOT_BOOTS], N), 23.0, 1.0);
	EXPECT_NEAR(Percent(byPart[Protocol::SLOT_TRINKET], N), 5.0, 1.0);

	// 종류별 유효 확률 (§ 5 표) - 무기 물리 : 마법 70 : 30, 아키타입 50 : 50 이 곱해진 값
	const map<uint32, double> expected = {
		{ 101, 9.8 }, { 102, 9.8 }, { 103, 4.2 }, { 104, 4.2 },
		{ 301, 11.0 }, { 302, 11.0 }, { 401, 11.0 }, { 402, 11.0 },
		{ 501, 11.5 }, { 502, 11.5 }, { 601, 5.0 },
	};

	for (const auto& [typeId, percent] : expected)
		EXPECT_NEAR(Percent(byType[typeId], N), percent, 1.0) << "type " << typeId;

	EXPECT_EQ(byType.size(), expected.size()) << "표에 없는 종류가 나왔다";

	// 무기 안에서 마법 30% · 투구 안에서 아키타입 50%
	const int64 weapons = byPart[Protocol::SLOT_WEAPON_PRIMARY];
	EXPECT_NEAR(Percent(byType[103] + byType[104], weapons), 30.0, 1.0);
	EXPECT_NEAR(Percent(byType[301], byPart[Protocol::SLOT_HELMET]), 50.0, 1.0);
}

TEST(ItemRoller, AC_CH_01_집_상자는_희귀가_30퍼센트다)
{
	LoadTables();
	const ChestPool& pool = *FloorItemPool::Find(0, ChestTier::House);

	mt19937 rng(7);
	InstanceIdIssuer nextId = Counter();

	constexpr int64 N = 100'000;
	int64 rare = 0;

	for (int64 i = 0; i < N; i++)
	{
		if (ItemRoller::RollItem(pool, rng, nextId).grade == Protocol::ITEM_GRADE_RARE)
			rare++;
	}

	EXPECT_NEAR(Percent(rare, N), 30.0, 1.0);
}

TEST(ItemRoller, AC_CH_02_상자_아이템_수는_1_이상_등급_상한_이하이고_id는_겹치지_않는다)
{
	LoadTables();

	mt19937 rng(42);
	InstanceIdIssuer nextId = Counter(900'007);
	unordered_set<uint64> ids;

	for (ChestTier tier : { ChestTier::Open, ChestTier::House })
	{
		const ChestPool& pool = *FloorItemPool::Find(0, tier);
		size_t minCount = SIZE_MAX;
		size_t maxCount = 0;

		for (int32 i = 0; i < 10'000; i++)
		{
			const vector<ItemEntry> items = ItemRoller::RollChest(pool, rng, nextId);
			minCount = min(minCount, items.size());
			maxCount = max(maxCount, items.size());

			for (const ItemEntry& e : items)
			{
				EXPECT_TRUE(ids.insert(e.instanceId).second) << "id " << e.instanceId << " 가 두 번 나왔다";
				EXPECT_EQ(e.level, 0u);
			}
		}

		EXPECT_GE(minCount, 1u) << FloorItemPool::TierName(tier) << " 빈 상자가 나왔다";
		EXPECT_EQ(maxCount, pool.maxCount) << FloorItemPool::TierName(tier);
	}
}

TEST(ItemRoller, 스킬은_부위별_풀에서_붙는다)
{
	LoadTables();
	mt19937 rng(1);

	// § 4 "지금 롤 결과" 표
	set<uint32> trinketSkills;

	for (int32 i = 0; i < 1'000; i++)
	{
		uint32 p = 0;
		uint32 s = 0;

		ItemRoller::RollSkills(*ItemTable::Find(101), rng, OUT p, OUT s);
		EXPECT_EQ(p, 1001u);
		EXPECT_EQ(s, 0u) << "1001 은 Shift 전용 - 무기 풀에 Q 후보가 없다";

		ItemRoller::RollSkills(*ItemTable::Find(301), rng, OUT p, OUT s);
		EXPECT_EQ(p, 0u) << "투구 풀이 비어 있다 (EB-15)";

		ItemRoller::RollSkills(*ItemTable::Find(401), rng, OUT p, OUT s);
		EXPECT_EQ(p, 3001u);

		ItemRoller::RollSkills(*ItemTable::Find(502), rng, OUT p, OUT s);
		EXPECT_EQ(p, 2001u);
		EXPECT_EQ(s, 0u) << "무기만 두 번째 스킬을 가진다";

		ItemRoller::RollSkills(*ItemTable::Find(601), rng, OUT p, OUT s);
		trinketSkills.insert(p);
	}

	EXPECT_EQ(trinketSkills, (set<uint32>{ 2001, 3001 })) << "장신구는 2001 또는 3001";
}

TEST(ItemRoller, 무기_Q는_Shift와_다르고_Shift_전용_스킬은_받지_않는다)
{
	// 무기 풀에 2001 을 더한다 - 1001(하드 CC · Shift 전용)과 2001 이 경쟁한다
	const string pools = string(POOLS) + "weapon,2001,1\n";
	LoadTables(pools.c_str());

	mt19937 rng(3);
	map<pair<uint32, uint32>, int32> seen;

	for (int32 i = 0; i < 2'000; i++)
	{
		uint32 shift = 0;
		uint32 q = 0;
		ItemRoller::RollSkills(*ItemTable::Find(102), rng, OUT shift, OUT q);
		seen[{ shift, q }]++;

		EXPECT_NE(q, 1001u) << "Shift 전용 스킬이 Q 로 나왔다";
		if (q != 0)
			EXPECT_NE(q, shift) << "같은 스킬이 두 키에 붙었다";
	}

	// Shift 1001 → Q 2001 , Shift 2001 → Q 없음(1001 은 Q 불가). 두 경우 다 나와야 한다
	EXPECT_GT((seen[{ 1001, 2001 }]), 0);
	EXPECT_GT((seen[{ 2001, 0 }]), 0);
	EXPECT_EQ(seen.size(), 2u);
}

TEST(ItemRoller, 같은_시드면_같은_아이템이_나온다)
{
	LoadTables();
	const ChestPool& pool = *FloorItemPool::Find(0, ChestTier::House);

	mt19937 a(99);
	mt19937 b(99);
	InstanceIdIssuer idA = Counter();
	InstanceIdIssuer idB = Counter();

	for (int32 i = 0; i < 200; i++)
	{
		const vector<ItemEntry> x = ItemRoller::RollChest(pool, a, idA);
		const vector<ItemEntry> y = ItemRoller::RollChest(pool, b, idB);

		ASSERT_EQ(x.size(), y.size());
		for (size_t k = 0; k < x.size(); k++)
		{
			EXPECT_EQ(x[k].itemTypeId, y[k].itemTypeId);
			EXPECT_EQ(x[k].grade, y[k].grade);
			EXPECT_EQ(x[k].skillIdPrimary, y[k].skillIdPrimary);
			EXPECT_EQ(x[k].skillIdSecondary, y[k].skillIdSecondary);
		}
	}
}

TEST(FloorItemPool, 잘못된_행은_기동을_막는다)
{
	LoadTables();

	const struct { const char* rows; const char* expect; } cases[] = {
		{ "0,open,count,0,10\n",		"count" },		// 빈 상자 금지
		{ "0,open,grade,5,10\n",		"grade" },
		{ "0,open,part,2,10\n",			"part" },		// 보조 무기 칸은 부위가 아니다
		{ "0,open,count,1,0\n",			"weight" },
		{ "0,open,count,1,5\n",			"duplicate" },
		{ "0,cave,count,1,5\n",			"tier" },
		{ "0,open,color,red,5\n",		"category" },
		{ "0,open,damage,fire,5\n",		"damage" },
	};

	for (const auto& c : cases)
	{
		string error;
		EXPECT_FALSE(FloorItemPool::Parse(FloorPool() + c.rows, OUT error)) << c.rows;
		EXPECT_NE(error.find(c.expect), string::npos) << c.rows << " -> " << error;
		EXPECT_EQ(FloorItemPool::Count(), 0u) << "실패했는데 표가 남았다";
	}
}

TEST(FloorItemPool, 풀마다_개수_등급_부위가_다_있어야_한다)
{
	LoadTables();

	// 2층에 개수 행만 있다
	string error;
	EXPECT_FALSE(FloorItemPool::Parse(FloorPool() + "1,open,count,1,10\n", OUT error));
	EXPECT_NE(error.find("floor 1"), string::npos) << error;

	// 무기 부위가 없는데 물리/마법 행이 있다
	const string noWeapon = string(FLOOR_HEADER)
		+ "0,open,count,1,1\n0,open,grade,1,1\n0,open,part,3,1\n0,open,damage,physical,1\n";
	EXPECT_FALSE(FloorItemPool::Parse(noWeapon, OUT error));
	EXPECT_NE(error.find("no weapon part"), string::npos) << error;
}
