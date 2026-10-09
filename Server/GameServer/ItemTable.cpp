#include "pch.h"
#include "ItemTable.h"
#include "SkillTable.h"
#include "CsvTable.h"

unordered_map<uint32, ItemTypeDef> ItemTable::s_types;
unordered_map<string, vector<SkillPoolEntry>> ItemTable::s_pools;
float ItemTable::s_gradeMultipliers[Protocol::ITEM_GRADE_LEGENDARY + 1] = { 1.f, 1.f, 1.f, 1.f, 1.f };

namespace
{
	bool IsArmorSlot(Protocol::EquipSlot slot)
	{
		return slot == Protocol::SLOT_HELMET || slot == Protocol::SLOT_ARMOR || slot == Protocol::SLOT_BOOTS;
	}

	// item_type_stats.csv 의 stat 이름 - 클라 S1ItemUIStyle 의 라벨 키와 같은 철자
	bool ParseStatName(const string& name, OUT ItemStat& out)
	{
		static const pair<const char*, ItemStat> NAMES[] =
		{
			{ "attack_power",			ItemStat::AttackPower },
			{ "magic_power",			ItemStat::MagicPower },
			{ "max_hp",					ItemStat::MaxHp },
			{ "armor",					ItemStat::Armor },
			{ "magic_resist",			ItemStat::MagicResist },
			{ "crit_chance",			ItemStat::CritChance },
			{ "crit_multiplier_bonus",	ItemStat::CritMultiplierBonus },
			{ "percent_as_bonus",		ItemStat::PercentAsBonus },
			{ "percent_ms_bonus",		ItemStat::PercentMsBonus },
			{ "resistance",				ItemStat::Resistance },
		};

		for (const auto& [text, stat] : NAMES)
		{
			if (name == text)
			{
				out = stat;
				return true;
			}
		}
		return false;
	}

	void ResetGrades(float (&grades)[Protocol::ITEM_GRADE_LEGENDARY + 1])
	{
		for (float& g : grades)
			g = 1.f;
	}
}

bool ItemTable::Load(const string& dataDir, OUT string& error)
{
	const char* names[4] = { "item_types.csv", "skill_pools.csv", "item_type_stats.csv", "item_grades.csv" };
	string texts[4];

	for (int32 i = 0; i < 4; i++)
	{
		if (Csv::ReadFile(dataDir + "\\" + names[i], OUT texts[i]) == false)
		{
			error = string("cannot open ") + names[i];
			s_types.clear();
			s_pools.clear();
			ResetGrades(s_gradeMultipliers);
			return false;
		}
	}

	if (Parse(texts[0], texts[1], OUT error) == false)
		return false;

	if (ParseStats(texts[2], texts[3], OUT error) == false)
	{
		// 스탯 없이 뜨면 장비가 전투에 아무 영향이 없는 서버가 된다 - 부분 성공으로 두지 않는다
		s_types.clear();
		s_pools.clear();
		return false;
	}

	return true;
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
		|| Csv::FindColumns(typeCsv, "item_types.csv", { "id", "equip_slot", "archetype", "damage_type", "skill_pool_id", "attack_range_cm" }, OUT tc, OUT error) == false)
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

		// 사거리는 무기만 공급한다 (C3-b). 맨몸 150 하한은 합산이 지킨다 - 여기서는 음수만 막는다
		if (Csv::ParseFloat(row[tc[5]], OUT def.attackRangeCm) == false || def.attackRangeCm < 0.f
			|| (isWeapon == false && def.attackRangeCm != 0.f))
		{
			error = where + " attack_range_cm must be a number >= 0, and 0 for non-weapons";
			return false;
		}

		// 평타 - 무기마다 다르다 (2026-10-10). 열이 없으면 예전 값(1.0 · 근접) - 테스트 픽스처가 그대로 읽힌다
		if (auto asIt = typeCsv.columns.find("attack_speed"); asIt != typeCsv.columns.end())
		{
			if (Csv::ParseFloat(row[asIt->second], OUT def.attackSpeed) == false || def.attackSpeed <= 0.f || def.attackSpeed > 3.f
				|| (isWeapon == false && def.attackSpeed != 1.f))
			{
				error = where + " attack_speed must be (0, 3], and 1.0 for non-weapons";
				return false;
			}
		}

		if (auto kindIt = typeCsv.columns.find("attack_kind"); kindIt != typeCsv.columns.end())
		{
			const string& kind = row[kindIt->second];
			if (kind == "projectile")
				def.rangedAttack = true;
			else if (kind == "melee")
				def.rangedAttack = false;
			else if (kind != "none")
			{
				error = where + " attack_kind must be melee / projectile / none";
				return false;
			}

			if (isWeapon == (kind == "none"))
			{
				error = where + " attack_kind must be melee/projectile for weapons and none for the rest";
				return false;
			}
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

bool ItemTable::ParseStats(const string& itemTypeStatsCsv, const string& itemGradesCsv, OUT string& error)
{
	for (auto& [typeId, def] : s_types)
		def.stats.clear();
	ResetGrades(s_gradeMultipliers);

	// ── item_grades.csv ── 등급 넷이 전부 있어야 한다. 빠진 등급이 조용히 1.0 이 되면 전설이 일반과 같아진다
	float grades[Protocol::ITEM_GRADE_LEGENDARY + 1] = { 1.f, 1.f, 1.f, 1.f, 1.f };
	bool seen[Protocol::ITEM_GRADE_LEGENDARY + 1] = {};

	CsvTable gradeCsv;
	vector<int32> gc;
	if (Csv::Parse(itemGradesCsv, "item_grades.csv", OUT gradeCsv, OUT error) == false
		|| Csv::FindColumns(gradeCsv, "item_grades.csv", { "grade", "multiplier" }, OUT gc, OUT error) == false)
		return false;

	for (size_t r = 0; r < gradeCsv.rows.size(); r++)
	{
		const vector<string>& row = gradeCsv.rows[r];
		const string where = "item_grades.csv:" + to_string(gradeCsv.lineNumbers[r]);

		uint32 grade = 0;
		float multiplier = 0.f;
		if (Csv::ParseUInt(row[gc[0]], OUT grade) == false || grade < Protocol::ITEM_GRADE_COMMON || grade > Protocol::ITEM_GRADE_LEGENDARY
			|| Csv::ParseFloat(row[gc[1]], OUT multiplier) == false || multiplier <= 0.f)
		{
			error = where + " grade must be 1..4 and multiplier > 0";
			return false;
		}

		if (seen[grade])
		{
			error = where + " grade " + to_string(grade) + " appears twice";
			return false;
		}

		seen[grade] = true;
		grades[grade] = multiplier;
	}

	for (uint32 g = Protocol::ITEM_GRADE_COMMON; g <= Protocol::ITEM_GRADE_LEGENDARY; g++)
	{
		if (seen[g] == false)
		{
			error = "item_grades.csv is missing grade " + to_string(g);
			return false;
		}
	}

	// ── item_type_stats.csv ──
	unordered_map<uint32, vector<ItemStatLine>> lines;

	CsvTable statCsv;
	vector<int32> sc;
	if (Csv::Parse(itemTypeStatsCsv, "item_type_stats.csv", OUT statCsv, OUT error) == false
		|| Csv::FindColumns(statCsv, "item_type_stats.csv", { "id", "stat", "base_stat", "flat_floor" }, OUT sc, OUT error) == false)
		return false;

	for (size_t r = 0; r < statCsv.rows.size(); r++)
	{
		const vector<string>& row = statCsv.rows[r];
		const string where = "item_type_stats.csv:" + to_string(statCsv.lineNumbers[r]);

		uint32 typeId = 0;
		ItemStatLine line;
		if (Csv::ParseUInt(row[sc[0]], OUT typeId) == false
			|| Csv::ParseFloat(row[sc[2]], OUT line.baseStat) == false || Csv::ParseFloat(row[sc[3]], OUT line.flatFloor) == false)
		{
			error = where + " id, base_stat and flat_floor must be numbers";
			return false;
		}

		const ItemTypeDef* def = Find(typeId);
		if (def == nullptr)
		{
			error = where + " id " + to_string(typeId) + " is not in item_types.csv";
			return false;
		}

		if (ParseStatName(row[sc[1]], OUT line.stat) == false)
		{
			error = where + " unknown stat '" + row[sc[1]] + "'";
			return false;
		}

		// 음수 페널티는 없다 - "중병기 crit_chance↓ 의 실제 값은 0" (item-equipment-system.md C3-b)
		if (line.baseStat < 0.f || line.flatFloor < 0.f)
		{
			error = where + " base_stat and flat_floor must be >= 0";
			return false;
		}

		// 공격 스탯은 무기 하나만 공급하고, 물리 무기는 attack_power · 마법 무기는 magic_power 다 (C3-b)
		if ((line.stat == ItemStat::AttackPower && def->damage != ItemDamage::Physical)
			|| (line.stat == ItemStat::MagicPower && def->damage != ItemDamage::Magic))
		{
			error = where + " attack_power belongs to physical weapons and magic_power to magic weapons only";
			return false;
		}

		vector<ItemStatLine>& typeLines = lines[typeId];
		for (const ItemStatLine& existing : typeLines)
		{
			if (existing.stat == line.stat)
			{
				error = where + " " + to_string(typeId) + " " + row[sc[1]] + " appears twice";
				return false;
			}
		}

		typeLines.push_back(line);
	}

	for (auto& [typeId, typeLines] : lines)
		s_types[typeId].stats = move(typeLines);

	memcpy(s_gradeMultipliers, grades, sizeof(grades));
	return true;
}

float ItemTable::GradeMultiplier(Protocol::ItemGrade grade)
{
	if (grade < Protocol::ITEM_GRADE_COMMON || grade > Protocol::ITEM_GRADE_LEGENDARY)
		return 1.f;

	return s_gradeMultipliers[grade];
}

size_t ItemTable::StatLineCount()
{
	size_t count = 0;
	for (const auto& [typeId, def] : s_types)
		count += def.stats.size();
	return count;
}
