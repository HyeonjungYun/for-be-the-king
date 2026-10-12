#pragma once
#include "Creature.h"
#include "ItemTable.h"

/*---------------
	EquipStats
---------------*/

// 착용 6칸 → 전투 스탯 한 벌. item-equipment-system.md § 스탯 합산 규칙 표 그대로 (2026-10-07)
// 순수 함수 - ItemTable 만 읽는다. Player 가 착용이 바뀔 때마다 부른다.

struct WornItem
{
	uint32				typeId = 0;
	Protocol::ItemGrade	grade = Protocol::ITEM_GRADE_NONE;
	uint32				level = 0;		// 강화 레벨 0..ItemLevel::LEVEL_MAX (item-enhance-2026-10-12 R1)
};

// 아이템 강화 레벨 (design/quick-specs/item-enhance-2026-10-12.md · item-equipment-system.md § D2, 2026-10-12)
namespace ItemLevel
{
	constexpr uint32	LEVEL_MAX = 16;
	// per_level_rate = base_stat × (GM_전설 2.2 − GM_일반 1.0) / 16 - "만렙 일반템 = 갓 나온 전설템"
	constexpr float		LEVEL_RATE_PER_BASE = 1.2f / 16.f;

	inline bool			CanEnhance(uint32 level) { return level < LEVEL_MAX; }
	inline float		Bonus(float baseStat, uint32 level) { return baseStat * LEVEL_RATE_PER_BASE * static_cast<float>(level < LEVEL_MAX ? level : LEVEL_MAX); }		// (std::min) 는 Windows 매크로와 충돌
}

struct EquipStatTotals
{
	int32					maxHp = BaseStats::MAX_HP;
	int32					attackPower = BaseStats::ATTACK_POWER;
	int32					magicPower = BaseStats::MAGIC_POWER;
	int32					armor = BaseStats::ARMOR;
	int32					magicResist = BaseStats::MAGIC_RESIST;
	float					critChance = BaseStats::CRIT_CHANCE;
	float					critMultiplier = BaseStats::CRIT_MULITPLIER;
	float					percentAsBonus = 0.f;
	float					percentMsBonus = 0.f;
	float					resistance = BaseStats::RESISTANCE;
	float					attackRange = BaseStats::ATTACK_RANGE;
	float					attackSpeed = BaseStats::ATTACK_SPEED;				// 무기 배율 × 맨몸 1.0 (2026-10-10 무기별 평타)
	bool					rangedAttack = false;								// 활 · 스태프 - 평타가 투사체
	Protocol::DamageType	autoAttackType = Protocol::DAMAGE_TYPE_PHYSICAL;	// 무기가 평타 타입을 정한다 (combat-system.md Core Rule 2)
};

namespace EquipStats
{
	// = 맨몸 attack_power. 마법 무기가 맨주먹을 대체한다는 뜻 - 따로 조절하면 물리 / 마법 동등성(AC-IE-04)이 깨진다
	constexpr float		MAGIC_WEAPON_OFFSET		= static_cast<float>(BaseStats::ATTACK_POWER);

	constexpr float		CRIT_CHANCE_MAX			= 1.f;
	constexpr float		CRIT_MULTIPLIER_MIN		= 1.f;
	constexpr float		CRIT_MULTIPLIER_MAX		= 3.f;
	constexpr float		RESISTANCE_MAX			= 1.f;		// entities.yaml resistance_max - 넘어도 완전 면역은 없다
	constexpr float		AS_BONUS_PER_ITEM_MAX	= 0.35f;	// 합계 상한은 Creature 의 AS_MAX_RATIO 2.5
	constexpr float		MS_BONUS_PER_ITEM_MAX	= 0.15f;	// 합계 상한은 Player 의 MS_MAX_RATIO 1.5

	// 표에 없는 종류는 건너뛴다 (스탯 0). nakedMaxHp = characters.max_hp - 맨몸 바닥
	EquipStatTotals		Compute(const vector<WornItem>& worn, int32 nakedMaxHp);

	// 최대 체력이 바뀔 때 현재 체력 - 비율 유지 · 내림. 벗고 다시 입어 체력을 버는 길을 막는다.
	// 죽은 몸(0)은 그대로, 산 몸은 1 아래로 내려가지 않는다 (장비를 벗어서 죽지 않는다)
	int32				RescaleHp(int32 hp, int32 oldMaxHp, int32 newMaxHp);
}
