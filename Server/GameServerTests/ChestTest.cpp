#include "pch.h"
#include "Bag.h"
#include "ChestSpawnTable.h"
#include "ItemRoller.h"
#include "ItemTable.h"
#include "SkillTable.h"

// 🔴 상자 상태 전이는 Room 밖에서 시험한다 - 시계를 주입해 30분을 기다리지 않는다 (AC-CH-03).
namespace
{
	// 테스트 pch 는 GameServer pch 의 USING_SHARED_PTR(Bag) 을 가져오지 않는다
	using BagRef = shared_ptr<Bag>;

	const char* TYPES =
		"id,name_key,equip_slot,archetype,damage_type,grade_mode,attack_range_cm,skill_pool_id\n"
		"101,item.101,1,heavy_weapon,physical,rolled,175,weapon\n"
		"103,item.103,1,heavy_weapon,magic,rolled,175,weapon\n"
		"301,item.301,3,helmet_scout,none,rolled,0,helmet\n"
		"401,item.401,4,armor_heavy,none,rolled,0,armor\n";

	const char* POOLS =
		"pool_id,skill_id,weight\n"
		"weapon,1001,1\n"
		"armor,3001,1\n";

	// 0층만 풀이 있다. house 는 2~4개
	const char* FLOOR_POOL =
		"floor_id,tier,category,key,weight\n"
		"0,open,count,1,1\n"
		"0,open,grade,1,1\n"
		"0,open,part,4,1\n"
		"0,house,count,2,1\n0,house,count,4,1\n"
		"0,house,grade,1,9\n0,house,grade,2,1\n"
		"0,house,part,1,1\n0,house,part,3,1\n"
		"0,house,damage,physical,7\n0,house,damage,magic,3\n";

	const char* SPAWN_HEADER = "chest_id,floor_id,tier,x,y,z,yaw,respawn_s\n";

	constexpr uint32 FLOOR_COUNT_FOR_TEST = 4;
	constexpr uint64 SECOND_US = 1'000'000;

	void LoadTables()
	{
		SkillTable::Init();

		string error;
		ASSERT_TRUE(ItemTable::Parse(TYPES, POOLS, OUT error)) << error;
		ASSERT_TRUE(FloorItemPool::Parse(FLOOR_POOL, OUT error)) << error;
	}

	BagRef MakeChest(ChestTier tier, uint32 respawnSeconds)
	{
		BagRef chest = make_shared<Bag>();
		chest->chestId = 201;
		chest->tier = tier;
		chest->respawnUs = respawnSeconds * SECOND_US;
		return chest;
	}

	InstanceIdIssuer Counter()
	{
		auto next = make_shared<uint64>(1);
		return [next]() { return (*next)++; };
	}
}

TEST(ChestSpawnTable, 위치_등급_리스폰을_싣는다)
{
	LoadTables();

	const string csv = string(SPAWN_HEADER)
		+ "101,0,open,-1500,-1200.5,100,0,1800\n"
		+ "201,0,house,-1750,2350,100,90,1800\n";

	string error;
	ASSERT_TRUE(ChestSpawnTable::Parse(csv, FLOOR_COUNT_FOR_TEST, OUT error)) << error;
	ASSERT_EQ(ChestSpawnTable::All().size(), 2u);

	const ChestSpawn& house = ChestSpawnTable::All()[1];
	EXPECT_EQ(house.chestId, 201u);
	EXPECT_EQ(house.tier, ChestTier::House);
	EXPECT_FLOAT_EQ(house.x, -1750.f);
	EXPECT_FLOAT_EQ(house.yaw, 90.f);
	EXPECT_EQ(house.respawnSeconds, 1800u);
	EXPECT_FLOAT_EQ(ChestSpawnTable::All()[0].y, -1200.5f);
}

TEST(ChestSpawnTable, 잘못된_행은_기동을_막는다)
{
	LoadTables();

	const struct { const char* row; const char* expect; } cases[] = {
		{ "0,0,open,0,0,100,0,1800\n",		"chest_id" },
		{ "101,4,open,0,0,100,0,1800\n",	"floor_id" },		// 층은 0..3
		{ "101,1,open,0,0,100,0,1800\n",	"no floor_item_pool" },	// 2층 풀이 없다 - 빈 상자가 된다
		{ "101,0,cave,0,0,100,0,1800\n",	"tier" },
		{ "101,0,open,abc,0,100,0,1800\n",	"numbers" },
		{ "101,0,open,0,0,100,0,0\n",		"respawn_s" },
	};

	for (const auto& c : cases)
	{
		string error;
		EXPECT_FALSE(ChestSpawnTable::Parse(string(SPAWN_HEADER) + c.row, FLOOR_COUNT_FOR_TEST, OUT error)) << c.row;
		EXPECT_NE(error.find(c.expect), string::npos) << c.row << " -> " << error;
		EXPECT_TRUE(ChestSpawnTable::All().empty()) << "실패했는데 표가 남았다";
	}

	string error;
	const string duplicate = string(SPAWN_HEADER) + "101,0,open,0,0,100,0,1800\n" + "101,0,house,500,0,100,0,1800\n";
	EXPECT_FALSE(ChestSpawnTable::Parse(duplicate, FLOOR_COUNT_FOR_TEST, OUT error));
	EXPECT_NE(error.find("duplicate"), string::npos) << error;
}

TEST(Chest, AC_CH_04_열기만_하면_Sealed_로_남고_같은_내용물을_보여_준다)
{
	LoadTables();
	BagRef chest = MakeChest(ChestTier::House, 1800);
	mt19937 rng(5);
	InstanceIdIssuer nextId = Counter();
	const ChestPool& pool = *FloorItemPool::Find(0, ChestTier::House);

	ASSERT_TRUE(chest->RollIfNeeded(pool, rng, nextId)) << "처음 열면 롤한다";
	const vector<ItemEntry> first = chest->items;
	ASSERT_GE(first.size(), 2u);

	// 다시 열어도 다시 롤하지 않는다 - 둘이 동시에 열어도 한 번만 (Room 직렬화 + rolled)
	EXPECT_FALSE(chest->RollIfNeeded(pool, rng, nextId));
	ASSERT_EQ(chest->items.size(), first.size());
	for (size_t i = 0; i < first.size(); i++)
		EXPECT_EQ(chest->items[i].instanceId, first[i].instanceId);

	// 획득이 없으니 아무리 시간이 지나도 리셋되지 않는다 - "털렸다" 신호도 없다
	EXPECT_EQ(chest->lootedAtUs, 0u);
	EXPECT_FALSE(chest->IsDueForReset(100'000 * SECOND_US));
}

TEST(Chest, AC_CH_03_첫_획득부터_1800초가_지나야_Sealed_로_돌아가고_다음_열기에_새로_롤한다)
{
	LoadTables();
	BagRef chest = MakeChest(ChestTier::House, 1800);
	mt19937 rng(6);
	InstanceIdIssuer nextId = Counter();
	const ChestPool& pool = *FloorItemPool::Find(0, ChestTier::House);

	chest->RollIfNeeded(pool, rng, nextId);
	const uint64 firstId = chest->items.front().instanceId;

	const uint64 t0 = 50 * SECOND_US;
	EXPECT_TRUE(chest->MarkTaken(t0)) << "첫 획득에서 Looted";
	EXPECT_FALSE(chest->MarkTaken(t0 + 600 * SECOND_US)) << "두 번째 획득은 시계를 다시 돌리지 않는다";
	EXPECT_EQ(chest->lootedAtUs, t0);

	EXPECT_FALSE(chest->IsDueForReset(t0 + 1799 * SECOND_US)) << "t0 + 1799s 는 아직 Looted";
	EXPECT_TRUE(chest->IsDueForReset(t0 + 1801 * SECOND_US)) << "t0 + 1801s 는 리셋";

	const size_t left = chest->items.size();
	EXPECT_EQ(chest->ResetChest(), left) << "남은 내용물은 버린다";
	EXPECT_TRUE(chest->items.empty());
	EXPECT_FALSE(chest->rolled);
	EXPECT_EQ(chest->lootedAtUs, 0u);
	EXPECT_FALSE(chest->IsDueForReset(t0 + 5000 * SECOND_US)) << "리셋 뒤엔 다시 Sealed";

	ASSERT_TRUE(chest->RollIfNeeded(pool, rng, nextId)) << "다음 열기에 새로 롤한다";
	EXPECT_NE(chest->items.front().instanceId, firstId) << "새 아이템 - id 가 새로 발급된다";
}

TEST(Chest, 사망_가방은_상자_규칙을_타지_않는다)
{
	LoadTables();
	BagRef bag = make_shared<Bag>();		// chestId 0
	mt19937 rng(1);
	InstanceIdIssuer nextId = Counter();

	EXPECT_FALSE(bag->IsChest());
	EXPECT_FALSE(bag->RollIfNeeded(*FloorItemPool::Find(0, ChestTier::Open), rng, nextId));
	EXPECT_TRUE(bag->items.empty());
	EXPECT_FALSE(bag->MarkTaken(SECOND_US));
	EXPECT_FALSE(bag->IsDueForReset(100'000 * SECOND_US));
}
