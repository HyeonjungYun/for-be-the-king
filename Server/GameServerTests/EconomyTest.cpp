#include "pch.h"
#include "Economy.h"
#include "Player.h"
#include "ItemTable.h"
#include "CommitJournal.h"
#include <random>

// 경제 1차 (2026-10-17) - design/quick-specs/economy-loot-2026-10-17.md AC-EC-01 ~ 03 · 10

namespace
{
	const char* LOOT_TYPES =
		"id,name_key,sell_price,stack_max\n"
		"901,loot.901,5,10\n"
		"905,loot.905,120,5\n"
		"906,loot.906,400,1\n";
	const char* LOOT_DROPS =
		"monster_type_id,loot_id,chance_pct,min,max\n"
		"1,901,100,1,1\n"
		"3,905,100,2,3\n"
		"3,906,0,1,1\n";

	void LoadSampleLoot()
	{
		string types = LOOT_TYPES, drops = LOOT_DROPS, error;
		ASSERT_TRUE(LootTable::Parse(&types, &drops, OUT error)) << error;
	}

	ItemEntry Loot(uint64 id, uint32 type, uint32 count)
	{
		ItemEntry e;
		e.instanceId = id;
		e.itemTypeId = type;
		e.grade = Protocol::ITEM_GRADE_COMMON;
		e.stackCount = count;
		return e;
	}
}

// AC-EC-01 강화 비용 · 매입가 · 사망 차감 (경계 포함)
TEST(Economy, 비용_매입가_사망차감_식)
{
	const EconomyRules r;
	EXPECT_EQ(Economy::EnhanceCost(0, r), 20u);
	EXPECT_EQ(Economy::EnhanceCost(4, r), 49u);		// 48.83
	EXPECT_EQ(Economy::EnhanceCost(9, r), 149u);	// 149.0
	EXPECT_EQ(Economy::EnhanceCost(15, r), 568u);	// 568.4
	uint64 total = 0;
	for (uint32 l = 0; l < 16; l++)
		total += Economy::EnhanceCost(l, r);
	EXPECT_NEAR(static_cast<double>(total), 2760.0, 15.0);

	EXPECT_EQ(Economy::GearSellPrice(1, 0, r), 20u);
	EXPECT_EQ(Economy::GearSellPrice(2, 5, r), 90u);
	EXPECT_EQ(Economy::GearSellPrice(4, 16, r), 1300u);
	EXPECT_EQ(Economy::GearSellPrice(0, 3, r), 0u);
	EXPECT_EQ(Economy::GearSellPrice(5, 0, r), 0u);

	EXPECT_EQ(Economy::DeathGoldLoss(1005, r), 100u);
	EXPECT_EQ(Economy::DeathGoldLoss(9, r), 0u);
	EXPECT_EQ(Economy::DeathGoldLoss(0, r), 0u);
	EXPECT_EQ(Economy::DeathGoldLoss(UINT64_MAX, r), UINT64_MAX / 100 * 10 + (UINT64_MAX % 100) * 10 / 100);	// 넘치지 않는다
}

// AC-EC-01 한 칸 매입가 - 전리품은 개수만큼, 장비는 등급 · 레벨
TEST(Economy, 칸_매입가)
{
	LoadSampleLoot();
	const EconomyRules r;
	EXPECT_EQ(Economy::SellPrice(Loot(1, 905, 3), r), 360u);
	EXPECT_EQ(Economy::SellPrice(Loot(1, 901, 0), r), 5u);		// 개수 0 은 1 로

	ItemEntry gear;
	gear.itemTypeId = 101;
	gear.grade = Protocol::ITEM_GRADE_RARE;
	gear.level = 5;
	EXPECT_EQ(Economy::SellPrice(gear, r), 90u);
}

// AC-EC-02 설정 · 표 검사
TEST(Economy, 설정과_표_검사)
{
	string error;
	EconomyRules r;
	EXPECT_TRUE(r.Parse("key,value\nenhance_cost_base,30\nsell_grade_3,200\ndeath_gold_loss_pct,0\n", error)) << error;
	EXPECT_EQ(r.enhanceCostBase, 30u);
	EXPECT_EQ(r.sellGrade[3], 200u);
	EXPECT_EQ(r.deathGoldLossPct, 0u);

	EconomyRules bad;
	EXPECT_FALSE(bad.Parse("key,value\nnope,1\n", error));
	EXPECT_FALSE(bad.Parse("key,value\ndeath_gold_loss_pct,60\n", error));
	EXPECT_FALSE(bad.Parse("key,value\nenhance_cost_growth,0.9\n", error));
	EXPECT_FALSE(bad.Parse("key,value\nsell_grade_5,10\n", error));

	string types = "id,name_key,sell_price,stack_max\n101,loot.x,5,10\n";
	EXPECT_FALSE(LootTable::Parse(&types, nullptr, error));		// id 900..999
	types = "id,name_key,sell_price,stack_max\n901,loot.x,5,0\n";
	EXPECT_FALSE(LootTable::Parse(&types, nullptr, error));		// stack_max 1..99
	types = "id,name_key,sell_price,stack_max\n901,loot.x,5,10\n";
	string drops = "monster_type_id,loot_id,chance_pct,min,max\n1,902,50,1,1\n";
	EXPECT_FALSE(LootTable::Parse(&types, &drops, error));		// 없는 전리품
	drops = "monster_type_id,loot_id,chance_pct,min,max\n1,901,50,3,2\n";
	EXPECT_FALSE(LootTable::Parse(&types, &drops, error));		// min > max
	LoadSampleLoot();
}

// AC-EC-03 겹침 - 같은 종류 칸 채우기 · 넘치면 남김 · 다른 종류 · 장비는 안 겹침
TEST(Economy, 줍기_겹침)
{
	LoadSampleLoot();
	vector<ItemEntry> inv = { Loot(1, 901, 8), Loot(2, 905, 5), Loot(3, 901, 10) };

	ItemEntry teeth = Loot(10, 901, 3);
	Economy::AbsorbResult r = Economy::AbsorbIntoStacks(inv, teeth);
	EXPECT_EQ(r.absorbed, 2u);			// 8 → 10, 나머지 1
	EXPECT_EQ(r.left, 1u);
	EXPECT_EQ(inv[0].stackCount, 10u);
	EXPECT_EQ(inv[2].stackCount, 10u);	// 이미 가득 - 그대로

	ItemEntry shard = Loot(11, 905, 2);
	r = Economy::AbsorbIntoStacks(inv, shard);
	EXPECT_EQ(r.absorbed, 0u);			// 5/5 가득
	EXPECT_EQ(r.left, 2u);

	ItemEntry crest = Loot(12, 906, 1);
	r = Economy::AbsorbIntoStacks(inv, crest);
	EXPECT_EQ(r.left, 1u);				// 같은 종류 칸 없음

	ItemEntry gear;
	gear.itemTypeId = 101;
	r = Economy::AbsorbIntoStacks(inv, gear);
	EXPECT_EQ(r.absorbed, 0u);
}

// AC-EC-03 몬스터 전리품 - 100 % 행은 늘, 0 % 행은 안 나온다. 개수 범위 · 겹침 상한으로 칸을 나눈다
TEST(Economy, 몬스터_전리품_굴리기)
{
	LoadSampleLoot();
	mt19937 rng(7);
	uint64 nextId = 1000;
	auto id = [&]() { return nextId++; };

	for (int32 i = 0; i < 50; i++)
	{
		vector<ItemEntry> boss = Economy::RollLoot(3, rng, id);
		ASSERT_EQ(boss.size(), 1u);					// 905 만 (906 은 0 %)
		EXPECT_EQ(boss[0].itemTypeId, 905u);
		EXPECT_GE(boss[0].stackCount, 2u);
		EXPECT_LE(boss[0].stackCount, 3u);
		EXPECT_TRUE(boss[0].isNew);
	}
	EXPECT_EQ(Economy::RollLoot(2, rng, id).size(), 0u);	// 표에 없는 몬스터

	// 겹침 상한보다 많이 떨구면 칸을 나눈다
	string types = "id,name_key,sell_price,stack_max\n906,loot.906,400,1\n";
	string drops = "monster_type_id,loot_id,chance_pct,min,max\n3,906,100,3,3\n";
	string error;
	ASSERT_TRUE(LootTable::Parse(&types, &drops, error)) << error;
	EXPECT_EQ(Economy::RollLoot(3, rng, id).size(), 3u);
	LoadSampleLoot();
}

// AC-EC-02 실제 데이터 - 세 표가 읽히고, 전리품 id 가 장비와 겹치지 않는다
TEST(Economy, 실제_데이터)
{
	string error;
	EconomyRules r;
	ASSERT_TRUE(r.Load(ItemTable::DefaultDataDir(), OUT error)) << error;
	EXPECT_EQ(r.deathGoldLossPct, 10u);
	ASSERT_TRUE(LootTable::Load(ItemTable::DefaultDataDir(), OUT error)) << error;
	EXPECT_EQ(LootTable::TypeCount(), 6u);
	EXPECT_GE(LootTable::Drops().size(), 7u);
	ASSERT_TRUE(ItemTable::Load(ItemTable::DefaultDataDir(), OUT error)) << error;
	for (uint32 id = LootTable::ID_MIN; id <= LootTable::ID_MAX; id++)
		EXPECT_FALSE(LootTable::IsLoot(id) && ItemTable::Find(id) != nullptr) << id;
}

// AC-EC-10 저널 - 9 칸 왕복, 옛 7 · 8 칸은 개수 1
TEST(Economy, 저널_개수_왕복)
{
	JournalEntry entry;
	entry.id = "DEATH-1-1-0";
	entry.kind = JournalKind::Death;
	entry.characterId = 1;
	entry.death.bagId = 5;
	entry.death.items.push_back(Loot(77, 905, 4));
	entry.death.items[0].isNew = true;

	JournalEntry back;
	string error;
	ASSERT_TRUE(CommitJournal::Parse(CommitJournal::Format(entry), OUT back, OUT error)) << error;
	ASSERT_EQ(back.death.items.size(), 1u);
	EXPECT_EQ(back.death.items[0].stackCount, 4u);
	EXPECT_EQ(back.death.items[0].itemTypeId, 905u);

	string old = CommitJournal::Format(entry);
	const size_t at = old.find(",0,4\n");		// lonePath 0 · stack 4 → 옛 8 칸
	ASSERT_NE(at, string::npos);
	old.replace(at, 5, ",0\n");
	ASSERT_TRUE(CommitJournal::Parse(old, OUT back, OUT error)) << error;
	EXPECT_EQ(back.death.items[0].stackCount, 1u);
}
