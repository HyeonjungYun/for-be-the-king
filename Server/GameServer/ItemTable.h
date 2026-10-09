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

// item_type_stats.csv 의 stat 열 - item-equipment-system.md § 스탯 합산 규칙 표의 행
enum class ItemStat : uint8
{
	AttackPower = 0,
	MagicPower,
	MaxHp,
	Armor,
	MagicResist,
	CritChance,
	CritMultiplierBonus,
	PercentAsBonus,
	PercentMsBonus,
	Resistance,

	Count
};

// 최종 = flat_floor + base_stat × 등급 배율 (D2-b). MVP 는 레벨 0 이라 per_level_rate 항이 없다
struct ItemStatLine
{
	ItemStat	stat = ItemStat::AttackPower;
	float		baseStat = 0.f;
	float		flatFloor = 0.f;
};

struct ItemTypeDef
{
	uint32					typeId = 0;
	Protocol::EquipSlot		equipSlot = Protocol::SLOT_NONE;
	string					archetype;
	ItemDamage				damage = ItemDamage::None;
	string					skillPoolId;
	float					attackRangeCm = 0.f;	// 무기만. 0 = 공급하지 않음 (C3-b)
	float					attackSpeed = 1.f;		// 무기만 - 맨몸 공속(1.0/s)에 곱한다. 무기마다 평타가 다르다 (2026-10-10 사용자 결정)
	bool					rangedAttack = false;	// 활 · 스태프 - 평타가 투사체 (필중, 날아가는 중 벽에 막힘)

	vector<ItemStatLine>	stats;					// ParseStats 가 채운다
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

	// Parse 다음에 부른다 - 종류 id 를 Parse 가 실은 표로 검사한다. 실패하면 스탯 · 배율만 비운다.
	static bool											ParseStats(const string& itemTypeStatsCsv, const string& itemGradesCsv, OUT string& error);

	static string										DefaultDataDir();

	// item_grades.csv - 일반 · 희귀 · 영웅 · 전설. 표에 없는 등급(NONE 포함)은 1.0
	static float										GradeMultiplier(Protocol::ItemGrade grade);
	static size_t										StatLineCount();

	static const ItemTypeDef*							Find(uint32 typeId);
	static const vector<SkillPoolEntry>*				FindPool(const string& poolId);
	static const unordered_map<uint32, ItemTypeDef>&	All() { return s_types; }
	static size_t										PoolCount() { return s_pools.size(); }

	static bool											CanEquip(uint32 typeId, Protocol::EquipSlot slot);

private:
	static unordered_map<uint32, ItemTypeDef>				s_types;
	static unordered_map<string, vector<SkillPoolEntry>>	s_pools;
	static float											s_gradeMultipliers[Protocol::ITEM_GRADE_LEGENDARY + 1];
};

