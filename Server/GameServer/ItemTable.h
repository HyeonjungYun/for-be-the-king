#pragma once
#include "Enum.pb.h"

/*---------------
	ItemTable
---------------*/

enum class ItemDamage : uint8
{
	None = 0,
	Physical,
	Magic
};

struct ItemTypeDef
{
	uint32					typeId = 0;
	Protocol::EquipSlot		equipSlot = Protocol::SLOT_NONE;
	string					archetype;
	ItemDamage				damage = ItemDamage::None;
	string					skillPoolId;
};

struct SkillPoolEntry
{
	uint32	skillId = 0;
	uint32	weight = 0;
};

class ItemTable
{
public:
	// 실패하면 이유를 error에 남기고 false.
	static bool											Load(const string& dataDir, OUT string& error);
	static bool											Parse(const string& itemTypeCsv, const string& skillPoolsCsv, OUT string& error);

	static string										DefaultDataDir();

	static const ItemTypeDef*							Find(uint32 typeId);
	static const vector<SkillPoolEntry>*				FindPool(const string& poolId);
	static const unordered_map<uint32, ItemTypeDef>&	All() { return s_types; }
	static size_t										PoolCount() { return s_pools.size(); }

	static bool											CanEquip(uint32 typeId, Protocol::EquipSlot slot);

private:
	static unordered_map<uint32, ItemTypeDef>				s_types;
	static unordered_map<string, vector<SkillPoolEntry>>	s_pools;
};

