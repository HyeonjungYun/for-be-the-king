#include "pch.h"
#include "Player.h"
#include "EquipStats.h"

namespace
{
	constexpr float BASE_MOVE_SPEED = 340.F;
	constexpr float MS_MIN_RATIO = 0.5;
	constexpr float MS_MAX_RATIO = 1.5;
}

Player::Player()
{
	_isPlayer = true;
	objectInfo->set_creature_type(Protocol::CREATE_TYPE_PLAYER);
}

Player::~Player()
{
}

float Player::GetEffectiveMoveSpeed() const
{
	return ComputeMoveSpeed(activeSlow);
}

float Player::GetSpeedCeiling(uint64 nowUs) const
{
	if (moveExceptionEndUs != 0 && nowUs < moveExceptionEndUs)
		return moveExceptionSpeed;

	if (nowUs < speedGraceUntilUs)
		return ComputeMoveSpeed(min(preGraceSlow, activeSlow));

	return GetEffectiveMoveSpeed();
}

float Player::ComputeMoveSpeed(float slow) const
{
	const float flatBonus = 0.f;

	// 이동속도 보너스는 가산 후 1회 곱 - 곱연산 누적 금지 (entities.yaml effective_move_speed)
	const float speed = (BASE_MOVE_SPEED + flatBonus) * (1.f + percentMsBonus) * (1.f - slow);

	return std::clamp(speed, BASE_MOVE_SPEED * MS_MIN_RATIO, BASE_MOVE_SPEED * MS_MAX_RATIO);
}

bool Player::ApplyEquipmentStats(bool refill)
{
	vector<WornItem> worn;
	for (const ItemEntry& e : equipped)
	{
		if (e.instanceId != 0)
			worn.push_back({ e.itemTypeId, e.grade });
	}

	equipTotals = EquipStats::Compute(worn, nakedMaxHp);

	const int32 oldMaxHp = maxHp;
	const int32 oldHp = hp;

	maxHp = equipTotals.maxHp;
	RebuildCombatStats();

	hp = refill ? maxHp : EquipStats::RescaleHp(hp, oldMaxHp, maxHp);

	// 최대치만 바뀌고 체력은 반올림으로 그대로일 수 있다 (체력 1 등) - 그래도 알려야 남의 체력 바 비율이 맞는다
	return hp != oldHp || maxHp != oldMaxHp;
}

void Player::RebuildCombatStats()
{
	const EquipStatTotals& t = equipTotals;
	auto pct = [&](SkillBuffStat stat) { return status.BuffSum(stat); };
	auto scaled = [](int32 base, float bonus) { return static_cast<int32>(::lroundf(base * (1.f + bonus))); };

	attackPower		= scaled(t.attackPower, pct(SkillBuffStat::AttackPower));
	magicPower		= scaled(t.magicPower, pct(SkillBuffStat::MagicPower));
	armor			= scaled(t.armor, pct(SkillBuffStat::Armor));
	magicResist		= scaled(t.magicResist, pct(SkillBuffStat::MagicResist));
	critChance		= min(EquipStats::CRIT_CHANCE_MAX, t.critChance + pct(SkillBuffStat::CritChance));
	critMultiplier	= t.critMultiplier;
	percentAsBonus	= t.percentAsBonus + pct(SkillBuffStat::AttackSpeed);
	// 모래주머니 (1174) - 장착 중 내내 이속 -10 % (passive_stat). 버프와 같은 합에 들어간다 (2026-10-11 3차 로스터)
	percentMsBonus	= t.percentMsBonus + pct(SkillBuffStat::MoveSpeed) + PassiveStatSum(SkillBuffStat::MoveSpeed);
	resistance		= t.resistance;
	attackRange		= t.attackRange;
	attackSpeed		= t.attackSpeed;
	rangedAutoAttack = t.rangedAttack;
	autoAttackType	= t.autoAttackType;
	outgoingDamagePct = pct(SkillBuffStat::DamagePct);

	// 평타 모드 (2026-10-11) - 근접(푸리아 · 은장도)은 투사체 대신 그 사거리로 벤다. 그다음 사거리 버프(명경지수 · 통아 · 육량전)를 더한다
	if (const float melee = status.BuffMax(SkillBuffStat::BasicMelee); melee > 0.f)
	{
		attackRange = melee;
		rangedAutoAttack = false;
	}
	attackRange = max(MIN_ATTACK_RANGE_CM, attackRange + pct(SkillBuffStat::AttackRange));
}

uint32 Player::ItemLevelOfSlot(Protocol::EquipSlot slot) const
{
	int32 index = static_cast<int32>(slot) - 1;
	if (slot == Protocol::SLOT_WEAPON_SECONDARY)
		index = Protocol::SLOT_WEAPON_PRIMARY - 1;

	if (index < 0 || index >= SKILL_SLOT_COUNT || equipped[index].instanceId == 0)
		return 0;

	return equipped[index].level;
}

void Player::RebuildSkillSlotsFromEquipment()
{
	for (int32 i = 0; i < SKILL_SLOT_COUNT; i++)
		skillSlots[i].skillId = 0;

	for (int32 i = 0; i < SKILL_SLOT_COUNT; i++)
	{
		const ItemEntry& item = equipped[i];
		if (item.instanceId == 0)
			continue;

		skillSlots[i].skillId = item.skillIdPrimary;
	}

	const ItemEntry& weapon = equipped[Protocol::SLOT_WEAPON_PRIMARY - 1];
	if (weapon.instanceId != 0)
		skillSlots[Protocol::SLOT_WEAPON_SECONDARY - 1].skillId = weapon.skillIdSecondary;

	// 스폰 스냅샷의 무기 - 보는 사람이 자세 · 휘두르기 클립을 고른다 (anim, 2026-10-12). 0 = 맨손
	objectInfo->set_weapon_type_id(weapon.instanceId != 0 ? weapon.itemTypeId : 0);

	RefreshBindStates();
}

ItemEntry* Player::FindInInventory(uint64 instanceId)
{
	for (ItemEntry& e : inventory)
	{
		if (e.instanceId == instanceId)
			return &e;
	}

	return nullptr;
}

Protocol::ItemInstance ItemEntry::ToProto(Protocol::ItemState state, Protocol::EquipSlot slot) const
{
	Protocol::ItemInstance item;
	item.set_instance_id(instanceId);
	item.set_item_type_id(itemTypeId);
	item.set_grade(grade);
	item.set_level(level);
	item.set_state(state);
	item.set_slot(slot);

	if (skillIdPrimary != 0)
		item.add_skills()->set_skill_id(skillIdPrimary);

	if (skillIdSecondary != 0)
		item.add_skills()->set_skill_id(skillIdSecondary);

	return item;
}
