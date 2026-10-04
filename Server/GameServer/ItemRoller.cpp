#include "pch.h"
#include "ItemRoller.h"
#include "CsvTable.h"
#include "SkillTable.h"
#include "Player.h"
#include <algorithm>
#include <tuple>

map<pair<uint32, ChestTier>, ChestPool> FloorItemPool::s_pools;

namespace
{
	// 상자 하나가 낼 수 있는 아이템 수 상한 - 데이터 오타(20 → 200)를 막는 안전 범위다. 설계값은 CSV 에 있다.
	constexpr uint32 MAX_ITEMS_PER_CHEST = 10;

	bool IsWearableSlot(uint32 slot)
	{
		return slot >= Protocol::SLOT_WEAPON_PRIMARY && slot <= Protocol::SLOT_TRINKET && slot != Protocol::SLOT_WEAPON_SECONDARY;
	}

	// 그 부위(와 무기면 물리/마법)에 ItemTable 종류가 하나라도 있는가
	bool HasType(Protocol::EquipSlot slot, ItemDamage damage)
	{
		for (const auto& [typeId, def] : ItemTable::All())
		{
			if (def.equipSlot == slot && (damage == ItemDamage::None || def.damage == damage))
				return true;
		}

		return false;
	}

	template<typename T>
	const T& PickWeighted(const vector<Weighted<T>>& options, mt19937& rng)
	{
		uint32 total = 0;
		for (const Weighted<T>& o : options)
			total += o.weight;

		uniform_int_distribution<uint32> dist(0, total - 1);
		uint32 r = dist(rng);

		for (const Weighted<T>& o : options)
		{
			if (r < o.weight)
				return o.value;

			r -= o.weight;
		}

		return options.back().value;
	}

	template<typename T>
	const T& PickUniform(const vector<T>& options, mt19937& rng)
	{
		uniform_int_distribution<size_t> dist(0, options.size() - 1);
		return options[dist(rng)];
	}

	// allowedSlot 이 있는 스킬은 그 키에만 (§ 4-6). 1001 은 Shift 전용이라 Q 로는 나오지 않는다.
	bool CanUseKey(const SkillDef& skill, Protocol::EquipSlot key)
	{
		return skill.allowedSlot == Protocol::SLOT_NONE || skill.allowedSlot == key;
	}

	// 풀에서 key 에 붙일 스킬 하나. 후보가 없으면 0
	uint32 PickSkill(const vector<SkillPoolEntry>& pool, Protocol::EquipSlot key, uint32 excludeSkillId, bool excludeHardCc, mt19937& rng)
	{
		vector<Weighted<uint32>> candidates;

		for (const SkillPoolEntry& e : pool)
		{
			const SkillDef* skill = SkillTable::Find(e.skillId);
			if (skill == nullptr || e.skillId == excludeSkillId || CanUseKey(*skill, key) == false)
				continue;

			if (excludeHardCc && skill->HasHardCc())
				continue;

			candidates.push_back({ e.skillId, e.weight });
		}

		if (candidates.empty())
			return 0;

		return PickWeighted(candidates, rng);
	}
}

/*-------------------
	FloorItemPool
-------------------*/

bool FloorItemPool::Load(const string& dataDir, OUT string& error)
{
	string text;
	if (Csv::ReadFile(dataDir + "\\floor_item_pool.csv", OUT text) == false)
	{
		error = "cannot open floor_item_pool.csv";
		s_pools.clear();
		return false;
	}

	return Parse(text, OUT error);
}

bool FloorItemPool::Parse(const string& csv, OUT string& error)
{
	s_pools.clear();

	// 다 검사한 뒤에만 정적 표에 옮긴다.
	map<pair<uint32, ChestTier>, ChestPool> pools;

	CsvTable table;
	vector<int32> c;

	if (Csv::Parse(csv, "floor_item_pool.csv", OUT table, OUT error) == false
		|| Csv::FindColumns(table, "floor_item_pool.csv", { "floor_id", "tier", "category", "key", "weight" }, OUT c, OUT error) == false)
		return false;

	set<tuple<uint32, ChestTier, string, string>> seen;

	for (size_t r = 0; r < table.rows.size(); r++)
	{
		const vector<string>& row = table.rows[r];
		const string where = "floor_item_pool.csv:" + to_string(table.lineNumbers[r]);

		uint32 floorId = 0;
		uint32 weight = 0;
		ChestTier tier = ChestTier::Open;

		if (Csv::ParseUInt(row[c[0]], OUT floorId) == false || Csv::ParseUInt(row[c[4]], OUT weight) == false)
		{
			error = where + " floor_id and weight must be numbers";
			return false;
		}

		if (ParseTier(row[c[1]], OUT tier) == false)
		{
			error = where + " unknown tier '" + row[c[1]] + "' (open / house)";
			return false;
		}

		// 0 은 "없음"과 구분되지 않는다. 빼려면 행을 지운다.
		if (weight == 0)
		{
			error = where + " weight 0 is not allowed - delete the row instead";
			return false;
		}

		const string& category = row[c[2]];
		const string& key = row[c[3]];

		if (seen.insert({ floorId, tier, category, key }).second == false)
		{
			error = where + " duplicate row " + category + "=" + key;
			return false;
		}

		ChestPool& pool = pools[{ floorId, tier }];
		uint32 number = 0;

		if (category == "count")
		{
			// 0 개는 없다 - 빈 상자는 "누가 털었다" 신호를 오염시킨다 (§ 5)
			if (Csv::ParseUInt(key, OUT number) == false || number < 1 || number > MAX_ITEMS_PER_CHEST)
			{
				error = where + " count must be 1.." + to_string(MAX_ITEMS_PER_CHEST);
				return false;
			}

			pool.counts.push_back({ number, weight });
			pool.maxCount = max(pool.maxCount, number);
		}
		else if (category == "grade")
		{
			if (Csv::ParseUInt(key, OUT number) == false || number < Protocol::ITEM_GRADE_COMMON || number > Protocol::ITEM_GRADE_LEGENDARY)
			{
				error = where + " grade must be 1..4";
				return false;
			}

			pool.grades.push_back({ static_cast<Protocol::ItemGrade>(number), weight });
		}
		else if (category == "part")
		{
			if (Csv::ParseUInt(key, OUT number) == false || IsWearableSlot(number) == false)
			{
				error = where + " part must be a wearable slot (1, 3, 4, 5, 6)";
				return false;
			}

			const Protocol::EquipSlot slot = static_cast<Protocol::EquipSlot>(number);
			if (HasType(slot, ItemDamage::None) == false)
			{
				error = where + " part " + key + " has no item type in item_types.csv";
				return false;
			}

			pool.parts.push_back({ slot, weight });
		}
		else if (category == "damage")
		{
			ItemDamage damage = ItemDamage::None;
			if (key == "physical")
				damage = ItemDamage::Physical;
			else if (key == "magic")
				damage = ItemDamage::Magic;
			else
			{
				error = where + " damage must be physical or magic";
				return false;
			}

			if (HasType(Protocol::SLOT_WEAPON_PRIMARY, damage) == false)
			{
				error = where + " no " + key + " weapon in item_types.csv";
				return false;
			}

			pool.damages.push_back({ damage, weight });
		}
		else
		{
			error = where + " unknown category '" + category + "' (count / grade / part / damage)";
			return false;
		}
	}

	if (pools.empty())
	{
		error = "floor_item_pool.csv has no rows";
		return false;
	}

	// 풀마다 네 가지가 다 있어야 롤이 끝까지 간다.
	for (const auto& [key, pool] : pools)
	{
		const string name = "floor_item_pool.csv floor " + to_string(key.first) + " " + TierName(key.second);

		if (pool.counts.empty() || pool.grades.empty() || pool.parts.empty())
		{
			error = name + " needs count, grade and part rows";
			return false;
		}

		bool hasWeapon = false;
		for (const Weighted<Protocol::EquipSlot>& p : pool.parts)
		{
			if (p.value == Protocol::SLOT_WEAPON_PRIMARY)
				hasWeapon = true;
		}

		if (hasWeapon && pool.damages.empty())
		{
			error = name + " has a weapon part but no damage rows";
			return false;
		}

		if (hasWeapon == false && pool.damages.empty() == false)
		{
			error = name + " has damage rows but no weapon part";
			return false;
		}
	}

	s_pools = move(pools);
	return true;
}

const ChestPool* FloorItemPool::Find(uint32 floorId, ChestTier tier)
{
	auto findIt = s_pools.find({ floorId, tier });
	if (findIt == s_pools.end())
		return nullptr;

	return &findIt->second;
}

bool FloorItemPool::ParseTier(const string& s, OUT ChestTier& out)
{
	if (s == "open")
		out = ChestTier::Open;
	else if (s == "house")
		out = ChestTier::House;
	else
		return false;

	return true;
}

const char* FloorItemPool::TierName(ChestTier tier)
{
	return (tier == ChestTier::House) ? "house" : "open";
}

/*----------------
	ItemRoller
----------------*/

vector<ItemEntry> ItemRoller::RollChest(const ChestPool& pool, mt19937& rng, const InstanceIdIssuer& nextId)
{
	const uint32 count = PickWeighted(pool.counts, rng);

	vector<ItemEntry> items;
	items.reserve(count);

	for (uint32 i = 0; i < count; i++)
		items.push_back(RollItem(pool, rng, nextId));

	return items;
}

ItemEntry ItemRoller::RollItem(const ChestPool& pool, mt19937& rng, const InstanceIdIssuer& nextId)
{
	// 등급은 부위와 독립이고 스킬과도 독립이다 (EQ2 · EQ3 폴백)
	const Protocol::ItemGrade grade = PickWeighted(pool.grades, rng);
	const Protocol::EquipSlot part = PickWeighted(pool.parts, rng);
	const ItemDamage damage = (part == Protocol::SLOT_WEAPON_PRIMARY) ? PickWeighted(pool.damages, rng) : ItemDamage::None;

	// 후보 종류 - id 순으로 정렬한다. unordered_map 순회 순서에 기대면 같은 시드라도 결과가 바뀔 수 있다.
	vector<const ItemTypeDef*> candidates;
	for (const auto& [typeId, def] : ItemTable::All())
	{
		if (def.equipSlot == part && def.damage == damage)
			candidates.push_back(&def);
	}

	sort(candidates.begin(), candidates.end(),
		[](const ItemTypeDef* a, const ItemTypeDef* b) { return a->typeId < b->typeId; });

	// 아키타입 균등 → 그 안에서 종류 균등 (§ 5 "부위 안 50 : 50"). 종류 수로 나누면 일련번호가 늘어난
	// 아키타입이 더 자주 나온다 - T2 에서 종류가 늘어도 아키타입 비율이 유지되게 두 번 나눠 뽑는다.
	vector<string> archetypes;
	for (const ItemTypeDef* def : candidates)
	{
		if (find(archetypes.begin(), archetypes.end(), def->archetype) == archetypes.end())
			archetypes.push_back(def->archetype);
	}

	const string& archetype = PickUniform(archetypes, rng);

	vector<const ItemTypeDef*> ofArchetype;
	for (const ItemTypeDef* def : candidates)
	{
		if (def->archetype == archetype)
			ofArchetype.push_back(def);
	}

	const ItemTypeDef* type = PickUniform(ofArchetype, rng);

	ItemEntry e;
	e.instanceId = nextId();
	e.itemTypeId = type->typeId;
	e.grade = grade;
	e.level = 0;
	RollSkills(*type, rng, OUT e.skillIdPrimary, OUT e.skillIdSecondary);

	return e;
}

void ItemRoller::RollSkills(const ItemTypeDef& type, mt19937& rng, OUT uint32& primary, OUT uint32& secondary)
{
	primary = 0;
	secondary = 0;

	// 행이 없는 풀 - 스킬 없이 나온다 (EB-15)
	const vector<SkillPoolEntry>* pool = ItemTable::FindPool(type.skillPoolId);
	if (pool == nullptr)
		return;

	if (type.equipSlot != Protocol::SLOT_WEAPON_PRIMARY)
	{
		primary = PickSkill(*pool, type.equipSlot, 0, false, rng);
		return;
	}

	// 무기 - Shift 를 먼저 뽑고 Q 를 뽑는다. 같은 스킬은 두 번 붙지 않는다.
	// Shift 가 하드 CC 면 Q 는 하드 CC 가 아닌 것에서만 (E2) - 무기 하나에 하드 CC 둘은 AC-5 예산을 깬다.
	// "둘 다 하드 CC 면 비CC 풀에서 다시 롤"과 결과 분포가 같다 - 처음부터 비CC 풀에서 뽑는다.
	primary = PickSkill(*pool, Protocol::SLOT_WEAPON_PRIMARY, 0, false, rng);

	const SkillDef* first = SkillTable::Find(primary);
	const bool firstIsHardCc = (first != nullptr && first->HasHardCc());

	secondary = PickSkill(*pool, Protocol::SLOT_WEAPON_SECONDARY, primary, firstIsHardCc, rng);
}
