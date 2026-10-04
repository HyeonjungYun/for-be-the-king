#include "pch.h"
#include "ItemTable.h"
#include "SkillTable.h"
#include "CsvTable.h"

unordered_map<uint32, ItemTypeDef> ItemTable::s_types;
unordered_map<string, vector<SkillPoolEntry>> ItemTable::s_pools;

namespace
{
	bool IsArmorSlot(Protocol::EquipSlot slot)
	{
		return slot == Protocol::SLOT_HELMET || slot == Protocol::SLOT_ARMOR || slot == Protocol::SLOT_BOOTS;
	}
}

bool ItemTable::Load(const string& dataDir, OUT string& error)
{
	const char* names[2] = { "item_types.csv", "skill_pools.csv" };
	string texts[2];

	for (int32 i = 0; i < 2; i++)
	{
		if (Csv::ReadFile(dataDir + "\\" + names[i], OUT texts[i]) == false)
		{
			error = string("cannot open ") + names[i];
			s_types.clear();
			s_pools.clear();
			return false;
		}
	}

	return Parse(texts[0], texts[1], OUT error);
}

bool ItemTable::Parse(const string& itemTypesCsv, const string& skillPoolsCsv, OUT string& error)
{
	s_types.clear();
	s_pools.clear();

	// 다 검사한 뒤에만 정적 표에 옮긴다.
	unordered_map<uint32, ItemTypeDef> types;
	unordered_map<string, vector<SkillPoolEntry>> pools;

	// ── item_types.csv ──
	CsvTable typeCsv;
	vector<int32> tc;

	if (Csv::Parse(itemTypesCsv, "item_types.csv", OUT typeCsv, OUT error) == false
		|| Csv::FindColumns(typeCsv, "item_types.csv", { "id", "equip_slot", "archetype", "damage_type", "skill_pool_id" }, OUT tc, OUT error) == false)
		return false;

	for (size_t r = 0; r < typeCsv.rows.size(); r++)
	{
		const vector<string>& row = typeCsv.rows[r];
		const string where = "item_types.csv:" + to_string(typeCsv.lineNumbers[r]);

		ItemTypeDef def;
		uint32 slot = 0;

		if (Csv::ParseUInt(row[tc[0]], OUT def.typeId) == false || def.typeId == 0
			|| Csv::ParseUInt(row[tc[1]], OUT slot) == false)
		{
			error = where + " id and equip_slot must be positive numbers";
			return false;
		}

		// 슬롯 2(보조 무기)는 착용 칸이 아니다 - 무기의 두 번째 스킬 키일 뿐이다.
		if (slot < Protocol::SLOT_WEAPON_PRIMARY || slot > Protocol::SLOT_TRINKET || slot == Protocol::SLOT_WEAPON_SECONDARY)
		{
			error = where + " equip_slot " + to_string(slot) + " is not a wearable slot";
			return false;
		}

		// id 체계: 부위 × 100 + 번호 (quick-spec § 1)
		if (def.typeId / 100 != slot)
		{
			error = where + " id " + to_string(def.typeId) + " does not match equip_slot " + to_string(slot);
			return false;
		}

		def.equipSlot = static_cast<Protocol::EquipSlot>(slot);
		def.archetype = row[tc[2]];
		def.skillPoolId = row[tc[4]];

		const string& damage = row[tc[3]];
		if (damage == "physical")
			def.damage = ItemDamage::Physical;
		else if (damage == "magic")
			def.damage = ItemDamage::Magic;
		else if (damage == "none")
			def.damage = ItemDamage::None;
		else
		{
			error = where + " unknown damage_type '" + damage + "'";
			return false;
		}

		// 무기만 물리 / 마법을 가진다 - 롤러가 무기의 물리 : 마법 비율로 고른다.
		const bool isWeapon = (def.equipSlot == Protocol::SLOT_WEAPON_PRIMARY);
		if (isWeapon != (def.damage != ItemDamage::None))
		{
			error = where + " damage_type must be physical/magic for weapons and none for the rest";
			return false;
		}

		if (def.archetype.empty() || def.skillPoolId.empty())
		{
			error = where + " archetype and skill_pool_id are required";
			return false;
		}

		if (types.emplace(def.typeId, def).second == false)
		{
			error = where + " duplicate id " + to_string(def.typeId);
			return false;
		}
	}

	if (types.empty())
	{
		error = "item_types.csv has no rows";
		return false;
	}

	// ── skill_pools.csv ──
	CsvTable poolCsv;
	vector<int32> pc;

	if (Csv::Parse(skillPoolsCsv, "skill_pools.csv", OUT poolCsv, OUT error) == false
		|| Csv::FindColumns(poolCsv, "skill_pools.csv", { "pool_id", "skill_id", "weight" }, OUT pc, OUT error) == false)
		return false;

	for (size_t r = 0; r < poolCsv.rows.size(); r++)
	{
		const vector<string>& row = poolCsv.rows[r];
		const string where = "skill_pools.csv:" + to_string(poolCsv.lineNumbers[r]);

		const string& poolId = row[pc[0]];
		SkillPoolEntry entry;

		if (poolId.empty() || Csv::ParseUInt(row[pc[1]], OUT entry.skillId) == false || Csv::ParseUInt(row[pc[2]], OUT entry.weight) == false)
		{
			error = where + " pool_id is required, skill_id and weight must be numbers";
			return false;
		}

		// 0 은 "없음"과 구분되지 않는다. 빼려면 행을 지운다.
		if (entry.weight == 0)
		{
			error = where + " weight 0 is not allowed - delete the row instead";
			return false;
		}

		if (SkillTable::Find(entry.skillId) == nullptr)
		{
			error = where + " skill " + to_string(entry.skillId) + " is not in SkillTable";
			return false;
		}

		vector<SkillPoolEntry>& pool = pools[poolId];
		for (const SkillPoolEntry& e : pool)
		{
			if (e.skillId == entry.skillId)
			{
				error = where + " skill " + to_string(entry.skillId) + " is listed twice in pool '" + poolId + "'";
				return false;
			}
		}

		pool.push_back(entry);
	}

	// ── 교차 검사 ──
	// 풀마다 그 풀을 쓰는 부위를 모은다.
	unordered_map<string, set<Protocol::EquipSlot>> poolSlots;
	for (const auto& [typeId, def] : types)
		poolSlots[def.skillPoolId].insert(def.equipSlot);

	unordered_set<uint32> armorSkills;	// 투구 · 갑옷 · 신발 풀에 있는 스킬 - 장신구 풀의 모집단

	for (const auto& [poolId, entries] : pools)
	{
		auto usedIt = poolSlots.find(poolId);

		// 어떤 종류도 쓰지 않는 풀 - 대개 풀 이름 오타다.
		if (usedIt == poolSlots.end())
		{
			error = "skill_pools.csv pool '" + poolId + "' is not used by any item type (typo?)";
			return false;
		}

		const set<Protocol::EquipSlot>& slots = usedIt->second;
		const bool weaponOnly = (slots.size() == 1 && *slots.begin() == Protocol::SLOT_WEAPON_PRIMARY);

		for (const SkillPoolEntry& e : entries)
		{
			const SkillDef* skill = SkillTable::Find(e.skillId);
			const bool weaponKey = (skill->allowedSlot == Protocol::SLOT_WEAPON_PRIMARY || skill->allowedSlot == Protocol::SLOT_WEAPON_SECONDARY);

			// 하드 CC · 무기 키 전용 스킬은 무기 풀에만 (AC-5 예산의 전제 - SkillTableTest 와 같은 규칙)
			if ((skill->HasHardCc() || weaponKey) && weaponOnly == false)
			{
				error = "skill_pools.csv skill " + to_string(e.skillId) + " is weapon-only (hard CC or weapon key) but pool '"
					+ poolId + "' is used by non-weapon items";
				return false;
			}

			for (Protocol::EquipSlot slot : slots)
			{
				if (IsArmorSlot(slot))
					armorSkills.insert(e.skillId);
			}
		}
	}

	// 장신구 풀 ⊆ 투구 ∪ 갑옷 ∪ 신발 풀 (quick-spec § 4-4). 하드 CC 는 위에서 이미 걸렀다.
	for (const auto& [poolId, entries] : pools)
	{
		if (poolSlots[poolId].count(Protocol::SLOT_TRINKET) == 0)
			continue;

		for (const SkillPoolEntry& e : entries)
		{
			if (armorSkills.count(e.skillId) == 0)
			{
				error = "skill_pools.csv trinket pool '" + poolId + "' has skill " + to_string(e.skillId)
					+ " that no helmet/armor/boots pool has";
				return false;
			}
		}
	}

	// 종류가 가리키는 풀에 행이 없으면 그 부위는 스킬 없이 나온다 - 오류가 아니다(EB-15). 알리기만 한다.
	for (const auto& [poolId, slots] : poolSlots)
	{
		if (pools.find(poolId) == pools.end())
			LOG_LINE << "[BOOT] skill pool '" << poolId << "' is empty - those items roll no skill\n";
	}

	s_types = move(types);
	s_pools = move(pools);
	return true;
}

string ItemTable::DefaultDataDir()
{
	char* raw = nullptr;
	size_t length = 0;

	if (_dupenv_s(&raw, &length, "FBTK_DATA_DIR") == 0 && raw != nullptr)
	{
		const string dir = raw;
		::free(raw);
		return dir;
	}

	// 작업 디렉터리는 실행 방법마다 다르다(VS 디버거 · 배치 · 탐색기). exe 위치는 같다.
	char exePath[MAX_PATH] = {};
	::GetModuleFileNameA(nullptr, exePath, MAX_PATH);

	string dir = exePath;
	dir.erase(dir.find_last_of("\\/") + 1);
	return dir + "..\\..\\Data";
}

const ItemTypeDef* ItemTable::Find(uint32 typeId)
{
	auto findIt = s_types.find(typeId);
	if (findIt == s_types.end())
		return nullptr;

	return &findIt->second;
}

const vector<SkillPoolEntry>* ItemTable::FindPool(const string& poolId)
{
	auto findIt = s_pools.find(poolId);
	if (findIt == s_pools.end())
		return nullptr;

	return &findIt->second;
}

bool ItemTable::CanEquip(uint32 typeId, Protocol::EquipSlot slot)
{
	const ItemTypeDef* def = Find(typeId);
	return def != nullptr && def->equipSlot == slot;
}
