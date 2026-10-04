#include "pch.h"
#include "Bag.h"

Bag::Bag()
{
	objectInfo->set_object_type(Protocol::OBJECT_TYPE_BAG);
}

Bag::~Bag()
{
}

bool Bag::RollIfNeeded(const ChestPool& pool, mt19937& rng, const InstanceIdIssuer& nextId)
{
	if (IsChest() == false || rolled)
		return false;

	items = ItemRoller::RollChest(pool, rng, nextId);
	rolled = true;
	return true;
}

bool Bag::MarkTaken(uint64 nowUs)
{
	if (IsChest() == false || lootedAtUs != 0)
		return false;

	lootedAtUs = nowUs;
	return true;
}

bool Bag::IsDueForReset(uint64 nowUs) const
{
	return IsChest() && lootedAtUs != 0 && nowUs >= lootedAtUs + respawnUs;
}

size_t Bag::ResetChest()
{
	const size_t discarded = items.size();

	items.clear();
	viewers.clear();
	rolled = false;
	lootedAtUs = 0;

	return discarded;
}
