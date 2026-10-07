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

	const EquipStatTotals totals = EquipStats::Compute(worn, nakedMaxHp);

	const int32 oldMaxHp = maxHp;
	const int32 oldHp = hp;

	maxHp			= totals.maxHp;
	attackPower		= totals.attackPower;
	magicPower		= totals.magicPower;
	armor			= totals.armor;
	magicResist		= totals.magicResist;
	critChance		= totals.critChance;
	critMultiplier	= totals.critMultiplier;
	percentAsBonus	= totals.percentAsBonus;
	percentMsBonus	= totals.percentMsBonus;
	resistance		= totals.resistance;
	attackRange		= totals.attackRange;
	autoAttackType	= totals.autoAttackType;

	hp = refill ? maxHp : EquipStats::RescaleHp(hp, oldMaxHp, maxHp);

	return hp != oldHp;
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
