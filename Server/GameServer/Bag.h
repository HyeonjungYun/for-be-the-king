#pragma once
#include "Object.h"
#include "Player.h"

constexpr uint64 BAG_LIFETIME_US = 600'000'000;

struct DeathSnapshot
{
	uint64				characterId = 0;
	uint64				bagId = 0;
	uint32				floorId = 0;
	float				x = 0.f;
	float				y = 0.f;
	float				z = 0.f;
	uint64				expiresAtUs = 0;
	vector<ItemEntry>	items;
	vector<uint64>		droppedIds;
};

struct EscapeSnapshot
{
	uint64				characterId = 0;
	vector<ItemEntry>	carried;
	ItemEntry			equipped[SKILL_SLOT_COUNT];
	vector<uint64>		droppedIds;
};

class Bag : public Object
{
public:
	Bag();
	virtual ~Bag();

public:
	uint64					ownerCharacterId = 0;
	uint32					floorId;
	uint64					expiresAtUs = 0;
	vector<ItemEntry>		items;
	unordered_set<uint64>	viewers;
};

