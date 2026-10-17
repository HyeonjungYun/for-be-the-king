#include "pch.h"
#include "Economy.h"
#include "CsvTable.h"
#include "Player.h"
#include <cmath>

unordered_map<uint32, LootTypeDef>	LootTable::s_types;
vector<LootDropDef>					LootTable::s_drops;

EconomyRules& EconomyRules::Get()
{
	static EconomyRules s_rules;
	return s_rules;
}

bool EconomyRules::Parse(const string& csv, OUT string& error)
{
	CsvTable table;
	vector<int32> c;
	if (Csv::Parse(csv, "economy.csv", OUT table, OUT error) == false
		|| Csv::FindColumns(table, "economy.csv", { "key", "value" }, OUT c, OUT error) == false)
		return false;

	for (size_t r = 0; r < table.rows.size(); r++)
	{
		const string& key = table.rows[r][c[0]];
		const string& value = table.rows[r][c[1]];
		const string where = "economy.csv:" + to_string(table.lineNumbers[r]) + " " + key;

		uint32 u = 0;
		float f = 0.f;
		const bool isUInt = Csv::ParseUInt(value, OUT u);
		const bool isFloat = Csv::ParseFloat(value, OUT f);
		auto uintIn = [&](uint32 lo, uint32 hi) { if (isUInt == false || u < lo || u > hi) { error = where + " must be an integer " + to_string(lo) + ".." + to_string(hi); return false; } return true; };
		auto floatIn = [&](float lo, float hi) { if (isFloat == false || f < lo || f > hi) { error = where + " must be a number " + to_string(lo) + ".." + to_string(hi); return false; } return true; };

		if (key == "enhance_cost_base") { if (uintIn(1, 100000) == false) return false; enhanceCostBase = u; }
		else if (key == "enhance_cost_growth") { if (floatIn(1.f, 2.f) == false) return false; enhanceCostGrowth = f; }
		else if (key == "death_gold_loss_pct") { if (uintIn(0, 50) == false) return false; deathGoldLossPct = u; }
		else if (key == "sell_level_bonus") { if (floatIn(0.f, 1.f) == false) return false; sellLevelBonus = f; }
		else if (key.rfind("sell_grade_", 0) == 0 && key.size() == 12 && key[11] >= '1' && key[11] <= '4')
		{
			if (uintIn(0, 1000000) == false)
				return false;
			sellGrade[key[11] - '0'] = u;
		}
		else
		{
			error = where + " is not a known key";
			return false;
		}
	}
	return true;
}

bool EconomyRules::Load(const string& dataDir, OUT string& error)
{
	*this = EconomyRules();
	string text;
	if (Csv::ReadFile(dataDir + "\\economy.csv", OUT text) && Parse(text, OUT error) == false)
		return false;
	return true;
}

bool LootTable::Load(const string& dataDir, OUT string& error)
{
	string types, drops;
	const bool hasTypes = Csv::ReadFile(dataDir + "\\loot_types.csv", OUT types);
	const bool hasDrops = Csv::ReadFile(dataDir + "\\loot_drops.csv", OUT drops);
	return Parse(hasTypes ? &types : nullptr, hasDrops ? &drops : nullptr, OUT error);
}

bool LootTable::Parse(const string* typesCsv, const string* dropsCsv, OUT string& error)
{
	unordered_map<uint32, LootTypeDef> types;
	vector<LootDropDef> drops;

	if (typesCsv != nullptr)
	{
		CsvTable table;
		vector<int32> c;
		if (Csv::Parse(*typesCsv, "loot_types.csv", OUT table, OUT error) == false
			|| Csv::FindColumns(table, "loot_types.csv", { "id", "name_key", "sell_price", "stack_max" }, OUT c, OUT error) == false)
			return false;

		for (size_t r = 0; r < table.rows.size(); r++)
		{
			const vector<string>& row = table.rows[r];
			const string where = "loot_types.csv:" + to_string(table.lineNumbers[r]);
			LootTypeDef t;
			t.nameKey = row[c[1]];
			if (Csv::ParseUInt(row[c[0]], OUT t.id) == false || Csv::ParseUInt(row[c[2]], OUT t.sellPrice) == false
				|| Csv::ParseUInt(row[c[3]], OUT t.stackMax) == false)
			{
				error = where + " id, sell_price and stack_max must be non-negative integers";
				return false;
			}
			if (t.id < ID_MIN || t.id > ID_MAX || t.nameKey.empty() || t.stackMax < 1 || t.stackMax > 99)
			{
				error = where + " id must be 900..999, name_key not empty, stack_max 1..99";
				return false;
			}
			if (types.count(t.id) > 0)
			{
				error = where + " duplicate id " + to_string(t.id);
				return false;
			}
			types[t.id] = t;
		}
	}

	if (dropsCsv != nullptr)
	{
		CsvTable table;
		vector<int32> c;
		if (Csv::Parse(*dropsCsv, "loot_drops.csv", OUT table, OUT error) == false
			|| Csv::FindColumns(table, "loot_drops.csv", { "monster_type_id", "loot_id", "chance_pct", "min", "max" }, OUT c, OUT error) == false)
			return false;

		for (size_t r = 0; r < table.rows.size(); r++)
		{
			const vector<string>& row = table.rows[r];
			const string where = "loot_drops.csv:" + to_string(table.lineNumbers[r]);
			LootDropDef d;
			if (Csv::ParseUInt(row[c[0]], OUT d.monsterTypeId) == false || Csv::ParseUInt(row[c[1]], OUT d.lootId) == false
				|| Csv::ParseUInt(row[c[2]], OUT d.chancePct) == false || Csv::ParseUInt(row[c[3]], OUT d.minCount) == false
				|| Csv::ParseUInt(row[c[4]], OUT d.maxCount) == false)
			{
				error = where + " every value must be a non-negative integer";
				return false;
			}
			if (types.count(d.lootId) == 0)
			{
				error = where + " loot_id " + to_string(d.lootId) + " is not in loot_types.csv";
				return false;
			}
			if (d.chancePct > 100 || d.minCount < 1 || d.maxCount < d.minCount || d.maxCount > 99)
			{
				error = where + " chance_pct 0..100, 1 <= min <= max <= 99";
				return false;
			}
			drops.push_back(d);
		}
	}

	s_types = move(types);
	s_drops = move(drops);
	return true;
}

const LootTypeDef* LootTable::Find(uint32 id)
{
	auto it = s_types.find(id);
	return it != s_types.end() ? &it->second : nullptr;
}

uint64 Economy::EnhanceCost(uint32 level, const EconomyRules& rules)
{
	const double cost = static_cast<double>(rules.enhanceCostBase) * std::pow(static_cast<double>(rules.enhanceCostGrowth), static_cast<double>(level));
	return static_cast<uint64>(std::llround(cost));
}

uint64 Economy::GearSellPrice(uint32 grade, uint32 level, const EconomyRules& rules)
{
	if (grade < 1 || grade > 4)
		return 0;
	const double price = static_cast<double>(rules.sellGrade[grade]) * (1.0 + static_cast<double>(rules.sellLevelBonus) * level);
	return static_cast<uint64>(std::llround(price));
}

uint64 Economy::SellPrice(const ItemEntry& e, const EconomyRules& rules)
{
	if (const LootTypeDef* loot = LootTable::Find(e.itemTypeId))
		return static_cast<uint64>(loot->sellPrice) * max<uint32>(e.stackCount, 1);
	return GearSellPrice(static_cast<uint32>(e.grade), e.level, rules);
}

uint64 Economy::DeathGoldLoss(uint64 gold, const EconomyRules& rules)
{
	// gold / 100 * pct + (gold % 100) * pct / 100 - 곱셈이 넘치지 않는다
	const uint64 pct = rules.deathGoldLossPct;
	return gold / 100 * pct + (gold % 100) * pct / 100;
}

vector<ItemEntry> Economy::RollLoot(uint32 monsterTypeId, mt19937& rng, const function<uint64()>& nextInstanceId)
{
	vector<ItemEntry> out;
	for (const LootDropDef& d : LootTable::Drops())
	{
		if (d.monsterTypeId != monsterTypeId)
			continue;
		if (uniform_int_distribution<uint32>(0, 99)(rng) >= d.chancePct)
			continue;

		const LootTypeDef* type = LootTable::Find(d.lootId);
		uint32 count = uniform_int_distribution<uint32>(d.minCount, d.maxCount)(rng);
		while (count > 0 && type != nullptr)
		{
			const uint32 n = min(count, type->stackMax);
			ItemEntry e;
			e.instanceId = nextInstanceId();
			e.itemTypeId = d.lootId;
			e.grade = Protocol::ITEM_GRADE_COMMON;
			e.stackCount = n;
			e.isNew = true;
			out.push_back(e);
			count -= n;
		}
	}
	return out;
}

Economy::AbsorbResult Economy::AbsorbIntoStacks(vector<ItemEntry>& inventory, ItemEntry& incoming)
{
	AbsorbResult result;
	const LootTypeDef* type = LootTable::Find(incoming.itemTypeId);
	if (type == nullptr)
	{
		result.left = incoming.stackCount;
		return result;
	}

	for (ItemEntry& cell : inventory)
	{
		if (incoming.stackCount == 0)
			break;
		if (cell.itemTypeId != incoming.itemTypeId || cell.stackCount >= type->stackMax)
			continue;
		const uint32 room = type->stackMax - cell.stackCount;
		const uint32 moved = min(room, incoming.stackCount);
		cell.stackCount += moved;
		incoming.stackCount -= moved;
		result.absorbed += moved;
		result.touchedCells++;
	}
	result.left = incoming.stackCount;
	return result;
}
