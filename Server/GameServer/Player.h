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

	// 이번 런에 서버가 만들었다(상자 · 롤) - DB 행이 아직 없다. 탈출 · 사망 커밋이 UPDATE 대신 INSERT 한다.
	// 커밋으로 행이 생기면 false 로 돌린다. 메모리 전용이라 클라로 보내지 않는다.
	bool				isNew = false;

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

private:
	float		ComputeMoveSpeed(float slow)const;

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
	vector<uint64>			droppedIds;
	uint64					detachedUntilUs = 0;

	// 진입 지점에서 시작한다 - Room::EnterRoom 이 그 둘레 빈자리로 옮기고 내린다 (entry-point.md AC-EP-04)
	bool					spreadOnEnter = false;
};
