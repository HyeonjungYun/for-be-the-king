#pragma once

/*----------------
	Economy
----------------*/

// 경제 1차 (2026-10-17 - design/quick-specs/economy-loot-2026-10-17.md).
// 던전에는 골드가 없다 - 몬스터가 팔기 전용 "전리품"을 떨구고, 베이스캠프의 매입 행수가 전리품 · 장비를 골드로 바꾼다.
// 골드는 강화에 쓰고, 던전에서 죽으면 창고 골드의 일부를 잃는다. 순수 로직 - Room · DB 를 모른다 (EconomyTest.cpp)
//
//   economy.csv       key,value                                         강화 비용 · 매입가 · 사망 차감
//   loot_types.csv    id,name_key,sell_price,stack_max                  전리품 종류 (id 900..999)
//   loot_drops.csv    monster_type_id,loot_id,chance_pct,min,max        몬스터별 전리품 드랍

struct ItemEntry;

struct EconomyRules
{
	uint32		enhanceCostBase = 20;
	float		enhanceCostGrowth = 1.25f;
	uint32		deathGoldLossPct = 10;
	uint32		sellGrade[5] = { 0, 20, 60, 180, 500 };		// [grade] - 0 은 쓰지 않는다
	float		sellLevelBonus = 0.10f;

	bool		Parse(const string& csv, OUT string& error);
	bool		Load(const string& dataDir, OUT string& error);	// 파일이 없으면 기본값

	static EconomyRules& Get();
};

struct LootTypeDef
{
	uint32		id = 0;
	string		nameKey;
	uint32		sellPrice = 0;
	uint32		stackMax = 1;
};

struct LootDropDef
{
	uint32		monsterTypeId = 0;
	uint32		lootId = 0;
	uint32		chancePct = 0;
	uint32		minCount = 1;
	uint32		maxCount = 1;
};

class LootTable
{
public:
	static constexpr uint32	ID_MIN = 900;
	static constexpr uint32	ID_MAX = 999;

	// 두 파일 모두 없어도 된다 (전리품 없음). 잘못이면 error 를 남기고 false - 부팅 거부
	static bool		Load(const string& dataDir, OUT string& error);
	static bool		Parse(const string* typesCsv, const string* dropsCsv, OUT string& error);

	static const LootTypeDef*			Find(uint32 id);
	static bool							IsLoot(uint32 itemTypeId) { return Find(itemTypeId) != nullptr; }
	static const vector<LootDropDef>&	Drops() { return s_drops; }
	static size_t						TypeCount() { return s_types.size(); }

private:
	static unordered_map<uint32, LootTypeDef>	s_types;
	static vector<LootDropDef>					s_drops;
};

namespace Economy
{
	// 강화 비용 - 지금 레벨 L 에서 L+1 로: round(base × growth^L)
	uint64		EnhanceCost(uint32 level, const EconomyRules& rules);

	// 장비 매입가: round(sellGrade[grade] × (1 + sellLevelBonus × level)). 등급 밖이면 0
	uint64		GearSellPrice(uint32 grade, uint32 level, const EconomyRules& rules);

	// 한 칸의 매입가 - 전리품이면 sellPrice × 개수, 아니면 장비 식
	uint64		SellPrice(const ItemEntry& e, const EconomyRules& rules);

	// 사망 차감: floor(gold × pct / 100) - 넘치지 않게
	uint64		DeathGoldLoss(uint64 gold, const EconomyRules& rules);

	// 몬스터 하나의 전리품 - 행마다 확률을 굴려 min..max 개. 같은 종류는 한 칸으로 (stack_max 를 넘으면 칸을 나눈다)
	vector<ItemEntry>	RollLoot(uint32 monsterTypeId, mt19937& rng, const function<uint64()>& nextInstanceId);

	// 줍기의 겹침 (R2) - incoming 을 inventory 의 같은 종류 칸에 먼저 채운다.
	// 결과: 채운 개수 · 남은 개수(새 칸이 필요한 몫) · 채운 칸 수. inventory 는 바뀌고 incoming.stackCount 는 남은 개수가 된다
	struct AbsorbResult
	{
		uint32	absorbed = 0;
		uint32	left = 0;
		int32	touchedCells = 0;
	};
	AbsorbResult	AbsorbIntoStacks(vector<ItemEntry>& inventory, ItemEntry& incoming);
}
