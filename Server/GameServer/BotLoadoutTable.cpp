#include "pch.h"
#include "BotLoadoutTable.h"
#include "CsvTable.h"
#include "ItemTable.h"
#include "Player.h"

vector<BotLoadout> BotLoadoutTable::s_loadouts;

uint32 BotLoadout::WeaponTypeId() const
{
	for (const BotLoadoutPiece& p : pieces)
		if (p.slot == Protocol::SLOT_WEAPON_PRIMARY)
			return p.itemTypeId;
	return 0;
}

bool BotLoadoutTable::Load(const string& dataDir, OUT string& error)
{
	s_loadouts.clear();

	string text;
	if (Csv::ReadFile(dataDir + "\\bot_loadouts.csv", OUT text) == false)
		return true;		// 선택 기능 - 없으면 봇은 기본 장비로 들어온다

	return Parse(text, OUT error);
}

bool BotLoadoutTable::Parse(const string& csv, OUT string& error)
{
	s_loadouts.clear();

	CsvTable table;
	vector<int32> c;

	if (Csv::Parse(csv, "bot_loadouts.csv", OUT table, OUT error) == false
		|| Csv::FindColumns(table, "bot_loadouts.csv", { "build_id", "code", "role", "weapon_type", "shift_skill", "q_skill",
			"helmet_type", "e_skill", "armor_type", "r_skill", "boots_type", "z_skill", "trinket_type", "x_skill", "weight" }, OUT c, OUT error) == false)
		return false;

	vector<BotLoadout> loadouts;
	unordered_set<uint32> ids;

	for (size_t r = 0; r < table.rows.size(); r++)
	{
		const vector<string>& row = table.rows[r];
		const string where = "bot_loadouts.csv:" + to_string(table.lineNumbers[r]);

		BotLoadout l;
		if (Csv::ParseUInt(row[c[0]], OUT l.buildId) == false || l.buildId == 0 || ids.insert(l.buildId).second == false)
		{
			error = where + " build_id must be a unique number > 0";
			return false;
		}
		l.code = row[c[1]];
		l.role = row[c[2]];
		if (l.role != "duel" && l.role != "pack" && l.role != "runner" && l.role != "balanced")
		{
			error = where + " role must be duel | pack | runner | balanced";
			return false;
		}
		if (Csv::ParseUInt(row[c[14]], OUT l.weight) == false || l.weight == 0)
		{
			error = where + " weight must be a number > 0";
			return false;
		}

		// (부위, 종류 열, 첫 스킬 열, 둘째 스킬 열 - 무기만)
		const struct { Protocol::EquipSlot slot; int32 type; int32 skill; int32 skill2; } cols[] = {
			{ Protocol::SLOT_WEAPON_PRIMARY, c[3], c[4], c[5] },
			{ Protocol::SLOT_HELMET, c[6], c[7], -1 },
			{ Protocol::SLOT_ARMOR, c[8], c[9], -1 },
			{ Protocol::SLOT_BOOTS, c[10], c[11], -1 },
			{ Protocol::SLOT_TRINKET, c[12], c[13], -1 },
		};

		for (const auto& col : cols)
		{
			BotLoadoutPiece p;
			p.slot = col.slot;
			if (Csv::ParseUInt(row[col.type], OUT p.itemTypeId) == false
				|| Csv::ParseUInt(row[col.skill], OUT p.skillId) == false
				|| (col.skill2 >= 0 && Csv::ParseUInt(row[col.skill2], OUT p.skillIdSecondary) == false))
			{
				error = where + " item types and skills must be numbers (0 = none)";
				return false;
			}
			if (p.itemTypeId == 0)
			{
				if (col.slot == Protocol::SLOT_WEAPON_PRIMARY)
				{
					error = where + " weapon_type is required";
					return false;
				}
				continue;
			}

			// 기본 장비와 같은 검사 (StarterKitTable::Parse) - 그 부위에 입을 수 있고, 스킬은 그 종류의 풀에서 나올 수 있어야 한다
			const ItemTypeDef* def = ItemTable::Find(p.itemTypeId);
			if (def == nullptr || ItemTable::CanEquip(p.itemTypeId, p.slot) == false)
			{
				error = where + " item type " + to_string(p.itemTypeId) + " cannot be worn in slot " + to_string(p.slot);
				return false;
			}
			if (p.skillIdSecondary != 0 && p.skillIdSecondary == p.skillId)
			{
				error = where + " the two weapon skills must differ (EB-03)";
				return false;
			}
			for (const uint32 skillId : { p.skillId, p.skillIdSecondary })
			{
				if (skillId == 0)
					continue;
				const vector<SkillPoolEntry>* pool = ItemTable::FindPool(def->skillPoolId);
				bool inPool = false;
				if (pool != nullptr)
				{
					for (const SkillPoolEntry& entry : *pool)
						inPool = inPool || entry.skillId == skillId;
				}
				if (inPool == false)
				{
					error = where + " skill " + to_string(skillId) + " is not in pool '" + def->skillPoolId + "' of item type " + to_string(p.itemTypeId);
					return false;
				}
			}
			l.pieces.push_back(p);
		}

		loadouts.push_back(move(l));
	}

	s_loadouts = move(loadouts);
	return true;
}

const BotLoadout* BotLoadoutTable::Pick(uint32 weaponTypeId, uint64 roll)
{
	uint64 total = 0;
	for (const BotLoadout& l : s_loadouts)
		if (weaponTypeId == 0 || l.WeaponTypeId() == weaponTypeId)
			total += l.weight;
	if (total == 0)
		return nullptr;

	uint64 at = roll % total;
	for (const BotLoadout& l : s_loadouts)
	{
		if (weaponTypeId != 0 && l.WeaponTypeId() != weaponTypeId)
			continue;
		if (at < l.weight)
			return &l;
		at -= l.weight;
	}
	return nullptr;
}

int32 BotLoadoutTable::EquipInto(const BotLoadout& loadout, ItemEntry* equipped, int32 slotCount, const function<uint64()>& nextInstanceId)
{
	int32 filled = 0;
	for (const BotLoadoutPiece& piece : loadout.pieces)
	{
		const int32 index = piece.slot - 1;
		if (index < 0 || index >= slotCount)
			continue;

		ItemEntry& worn = equipped[index];
		if (worn.itemTypeId != 0)
			continue;

		worn = ItemEntry{};
		worn.instanceId = nextInstanceId();
		worn.itemTypeId = piece.itemTypeId;
		worn.grade = Protocol::ITEM_GRADE_COMMON;		// 봇이 죽으면 가방이 된다 - 기본 장비와 같은 등급으로 경제를 부풀리지 않는다
		worn.skillIdPrimary = piece.skillId;
		worn.skillIdSecondary = piece.skillIdSecondary;
		worn.isNew = true;
		filled++;
	}
	return filled;
}
