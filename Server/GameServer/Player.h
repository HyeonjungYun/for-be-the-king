#pragma once
#include "Creature.h"

class GameSession;
class Room;

struct ItemEntry
{
	uint64				instanceId = 0;
	uint32				itemTypeId = 0;
	Protocol::ItemGrade	grade = Protocol::ITEM_GRADE_NONE;
	uint32				level = 0;

	uint32				skillIdPrimary = 0;
	uint32				skillIdSecondary = 0;

	Protocol::ItemInstance ToProto(Protocol::ItemState state, Protocol::EquipSlot slot = Protocol::SLOT_NONE) const;
};

class Player : public Creature
{
public:
				Player();
	virtual		~Player();

public:
	float		GetEffectiveMoveSpeed() const;
	float		GetSpeedCeiling(uint64 nowUs) const;

public:
	void	RebuildSkillSlotsFromEquipment();
	ItemEntry* FindInInventory(uint64 instanceId);
	bool IsInventoryFull() const { return inventory.size() >= INVENTORY_SLOT_COUNT; }

public:
	weak_ptr<GameSession>	session;
	
public:
	uint64					lastMoveUs = 0;
	double					moveBudget = 0.0;
	float					moveExceptionSpeed = 0.f;
	uint64					moveExceptionEndUs = 0;

	uint64 lastSaveUs = 0;

public:

	static constexpr int32	INVENTORY_SLOT_COUNT = 25;

	vector<ItemEntry>		inventory;
	ItemEntry				equipped[SKILL_SLOT_COUNT];
};
