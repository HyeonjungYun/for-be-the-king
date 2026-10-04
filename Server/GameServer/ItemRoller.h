#pragma once
#include "ItemTable.h"
#include <functional>
#include <random>

struct ItemEntry;

/*-------------------
	FloorItemPool
-------------------*/

// Server/Data/floor_item_pool.csv — 층 · 상자 등급별 가중치 (quick-spec § 5).
// ItemTable 다음에 읽는다 - 부위 · 물리/마법 행에 실제로 뽑을 종류가 있는지 ItemTable 로 검사한다.

enum class ChestTier : uint8
{
	Open = 0,		// 개활지 소형
	House			// 집 대형
};

template<typename T>
struct Weighted
{
	T		value{};
	uint32	weight = 0;
};

struct ChestPool
{
	vector<Weighted<uint32>>				counts;		// 상자 1회 롤의 아이템 수 (1 이상)
	vector<Weighted<Protocol::ItemGrade>>	grades;
	vector<Weighted<Protocol::EquipSlot>>	parts;
	vector<Weighted<ItemDamage>>			damages;	// 무기만 - 물리 : 마법
	uint32									maxCount = 0;
};

class FloorItemPool
{
public:
	// 실패하면 이유를 error 에 남기고 false. 표는 빈 채로 둔다.
	static bool				Load(const string& dataDir, OUT string& error);
	static bool				Parse(const string& csv, OUT string& error);

	static const ChestPool*	Find(uint32 floorId, ChestTier tier);
	static size_t			Count() { return s_pools.size(); }

	static bool				ParseTier(const string& s, OUT ChestTier& out);
	static const char*		TierName(ChestTier tier);

private:
	static map<pair<uint32, ChestTier>, ChestPool>	s_pools;
};

/*----------------
	ItemRoller
----------------*/

// 아이템은 서버에서만, 생성되는 순간 한 번 롤한다 (B3). 상자는 처음 열 때 롤한다 (quick-spec § 8 지연 롤).
// RNG 와 id 발급은 주입한다 - 테스트가 시드를 고정해 결정론적으로 돈다.
// 실제 서버는 id 를 GNextInstanceId.fetch_add(1) 로 준다 (ADR-0004).
using InstanceIdIssuer = function<uint64()>;

class ItemRoller
{
public:
	// 상자 1회분. 개수는 counts 에서 뽑는다 - 데이터 검사가 1 이상을 보장한다 (빈 상자 금지, § 5)
	static vector<ItemEntry>	RollChest(const ChestPool& pool, mt19937& rng, const InstanceIdIssuer& nextId);

	// 등급 → 부위 → (무기면 물리/마법) → 아키타입 균등 → 그 안에서 종류 균등 → 스킬. level 0
	static ItemEntry			RollItem(const ChestPool& pool, mt19937& rng, const InstanceIdIssuer& nextId);

	// § 4 — 무기는 Shift 를 뽑고 Q 를 뽑는다. 나머지 부위는 한 번. 풀이 비면 0 (오류 아님, EB-15)
	static void					RollSkills(const ItemTypeDef& type, mt19937& rng, OUT uint32& primary, OUT uint32& secondary);
};
