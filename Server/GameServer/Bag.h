#pragma once
#include "Object.h"
#include "Player.h"
#include "ItemRoller.h"

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
	uint64				gold = 0;		// 런 골드 전액이 계정으로 (escape-timer.md Core Rule 18)
};

class Bag : public Object
{
public:
	Bag();
	virtual ~Bag();

	// ── 상자 (quick-spec § 8 · § 9) ──
	// 사망 가방과 같은 열기 · 루팅 · 열람자 경로를 쓴다. chestId 가 0 이 아니면 상자다.
	//   Sealed ──(첫 획득)──▶ Looted ──(첫 획득 + respawn)──▶ Sealed (내용물 폐기, rolled = false)
	bool		IsChest() const { return chestId != 0; }

	// 처음 열 때 롤한다(지연 롤). 이미 롤했거나 상자가 아니면 아무것도 하지 않고 false
	bool		RollIfNeeded(const ChestPool& pool, mt19937& rng, const InstanceIdIssuer& nextId);

	// 첫 "획득"만 Looted 로 바꾼다 - 열어 보기만 해서는 "털렸다" 신호가 생기지 않는다. 바뀌었으면 true
	bool		MarkTaken(uint64 nowUs);

	// 첫 획득 + 리스폰 시간이 지났는가
	bool		IsDueForReset(uint64 nowUs) const;

	// Sealed 로 되돌린다. 남은 내용물은 버린다(롤 아이템이라 DB 행이 없다). 버린 개수를 돌려준다
	size_t		ResetChest();

public:
	uint64					ownerCharacterId = 0;
	uint32					floorId;
	uint64					expiresAtUs = 0;		// 상자는 0 - 만료가 없다
	vector<ItemEntry>		items;
	unordered_set<uint64>	viewers;

	uint32					chestId = 0;
	ChestTier				tier = ChestTier::Open;
	uint64					respawnUs = 0;
	bool					rolled = false;
	uint64					lootedAtUs = 0;			// 0 = Sealed
};

