#include "pch.h"
#include "Player.h"

namespace
{
	constexpr float BASE_MOVE_SPEED = 340.F;
	constexpr float MS_MIN_RATIO = 0.5;
	constexpr float MS_MAX_RATIO = 1.5;
}

Player::Player()
{
	_isPlayer = true;
}

Player::~Player()
{
}

float Player::GetEffectiveMoveSpeed() const
{
	const float flatBonus = 0.f;
	const float percentBonus = 0.f;

	const float speed = (BASE_MOVE_SPEED + flatBonus) * (1.f + percentBonus) * (1.f - activeSlow);

	return std::clamp(speed, BASE_MOVE_SPEED * MS_MIN_RATIO, BASE_MOVE_SPEED * MS_MAX_RATIO);
}

float Player::GetSpeedCeiling(uint64 nowUs) const
{
	if (moveExceptionEndUs != 0 && nowUs < moveExceptionEndUs)
		return moveExceptionSpeed;

	return GetEffectiveMoveSpeed();
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
