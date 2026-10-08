#include "pch.h"
#include "StarterKitTable.h"
#include "CsvTable.h"
#include "ItemTable.h"
#include "Player.h"

vector<StarterKitPiece> StarterKitTable::s_pieces;

bool StarterKitTable::Load(const string& dataDir, OUT string& error)
{
	string text;
	if (Csv::ReadFile(dataDir + "\\starter_kit.csv", OUT text) == false)
	{
		error = "cannot open starter_kit.csv";
		s_pieces.clear();
		return false;
	}

	return Parse(text, OUT error);
}

bool StarterKitTable::Parse(const string& csv, OUT string& error)
{
	s_pieces.clear();

	vector<StarterKitPiece> pieces;
	unordered_set<uint32> slots;

	CsvTable table;
	vector<int32> c;

	if (Csv::Parse(csv, "starter_kit.csv", OUT table, OUT error) == false
		|| Csv::FindColumns(table, "starter_kit.csv", { "slot", "item_type_id", "skill_id" }, OUT c, OUT error) == false)
		return false;

	for (size_t r = 0; r < table.rows.size(); r++)
	{
		const vector<string>& row = table.rows[r];
		const string where = "starter_kit.csv:" + to_string(table.lineNumbers[r]);

		uint32 slot = 0;
		StarterKitPiece p;

		if (Csv::ParseUInt(row[c[0]], OUT slot) == false
			|| slot < Protocol::SLOT_WEAPON_PRIMARY || slot > Protocol::SLOT_TRINKET)
		{
			error = where + " slot must be " + to_string(Protocol::SLOT_WEAPON_PRIMARY) + ".." + to_string(Protocol::SLOT_TRINKET);
			return false;
		}
		p.slot = static_cast<Protocol::EquipSlot>(slot);

		if (slots.insert(slot).second == false)
		{
			error = where + " duplicate slot " + to_string(slot);
			return false;
		}

		if (Csv::ParseUInt(row[c[1]], OUT p.itemTypeId) == false || ItemTable::Find(p.itemTypeId) == nullptr)
		{
			error = where + " unknown item_type_id '" + row[c[1]] + "'";
			return false;
		}

		// 던전 · 베이스캠프 착용 경로와 같은 검사 - 그 부위에 입을 수 없는 종류를 입혀 들여보내지 않는다
		if (ItemTable::CanEquip(p.itemTypeId, p.slot) == false)
		{
			error = where + " item type " + to_string(p.itemTypeId) + " cannot be worn in slot " + to_string(slot);
			return false;
		}

		if (Csv::ParseUInt(row[c[2]], OUT p.skillId) == false)
		{
			error = where + " skill_id must be a number (0 = none)";
			return false;
		}

		// 상자에서 나올 수 없는 스킬을 기본 장비로 주지 않는다 - 그 종류의 풀에 있어야 한다
		if (p.skillId != 0)
		{
			const ItemTypeDef* def = ItemTable::Find(p.itemTypeId);
			const vector<SkillPoolEntry>* pool = ItemTable::FindPool(def->skillPoolId);

			bool inPool = false;
			if (pool != nullptr)
			{
				for (const SkillPoolEntry& entry : *pool)
					inPool = inPool || entry.skillId == p.skillId;
			}

			if (inPool == false)
			{
				error = where + " skill " + to_string(p.skillId) + " is not in pool '" + def->skillPoolId + "' of item type " + to_string(p.itemTypeId);
				return false;
			}
		}

		pieces.push_back(p);
	}

	// 빈 표는 "맨몸으로 들여보낸다" 가 되어 규칙이 조용히 꺼진다 - 기동을 거부한다
	if (pieces.empty())
	{
		error = "starter_kit.csv has no pieces";
		return false;
	}

	s_pieces = move(pieces);
	return true;
}

int32 StarterKitTable::EquipInto(ItemEntry* equipped, int32 slotCount, const function<uint64()>& nextInstanceId)
{
	int32 filled = 0;

	for (const StarterKitPiece& piece : s_pieces)
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
		worn.grade = Protocol::ITEM_GRADE_COMMON;
		worn.skillIdPrimary = piece.skillId;
		worn.isNew = true;		// DB 행은 없다 - 사망이면 가방으로, 탈출이면 소유로 INSERT 된다 (S-4)
		filled++;
	}

	return filled;
}
