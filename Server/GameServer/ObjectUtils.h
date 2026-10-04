#pragma once

struct DeathSnapshot;
struct EscapeSnapshot;
struct ChestSpawn;

class ObjectUtils
{
public:
	static PlayerRef CreatePlayer(GameSessionRef session, uint64 characterId);
	static BagRef CreateBag(uint64 bagId, uint32 floorId, float x, float y, float z, uint64 expiresAtUs, uint64 ownerCharacterId);

	// 상자 = 만료 없고 주인 없는 가방. 처음 열 때 롤한다 (Room::HandleBagOpen)
	static BagRef CreateChest(const ChestSpawn& spawn);

private:
	static atomic<int64> s_idGenerator;
};

constexpr uint64 RUNTIME_ID_BASE = 1'000'000'000;
constexpr uint64 BAG_ID_BASE = 2'000'000'000;
constexpr uint64 CHEST_ID_BASE = 3'000'000'000;	// + chest_id 로 고정 - 로그에서 몇 번 상자인지 바로 보인다. 재시작해도 같다

extern atomic<uint64> GNextInstanceId;
extern atomic<uint64> GNextBagId;

bool RestoreIdCounters(DBConnection* conn);

bool PurgeExpiredBags(DBConnection* conn);

bool ValidateItemRows(DBConnection* conn);

bool LoadLiveBags(DBConnection* conn);

bool DeleteExpiredBags(DBConnection* conn, const vector<uint64>& bagIds, const vector<uint64>& instanceIds);

bool CommitDeath(DBConnection* conn, const DeathSnapshot& snap);

void CommitEscape(DBConnection* conn, const EscapeSnapshot& snap);

extern atomic<bool> GFailDeathCommit;
extern atomic<int32> GFailEscapeCommits;

/*-----------------------
	DeathCommitTracker
------------------------*/

class DeathCommitTracker
{
public:
	bool TryBegin(uint64 characterId);
	void End(uint64 characterId);
	bool IsPending(uint64 characterId);

private:
	USE_LOCK;
	unordered_set<uint64> _pending;
};

extern DeathCommitTracker GDeathCommits;
extern atomic<uint32> GDeathCommitDelayMs;
extern atomic<uint32> GBaseCampDelayMs;

extern atomic<uint64> GDisconnectGraceUs;

/*------------------
	DetachedBodies
-------------------*/

// 접속이 끊겨 던전에 남은 몸. 재접속과 유예 만료가 같은 몸을 두고 경쟁하므로
// "표에서 먼저 꺼낸 쪽만 이긴다."로 판정한다.
class DetachedBodies
{
public:
	void		Add(uint64 characterId, PlayerRef body);
	PlayerRef	TryTake(uint64 characterId);
	bool		Contains(uint64 characterId);

private:
	USE_LOCK;
	unordered_map<uint64, PlayerRef> _bodies;
};

extern DetachedBodies GDetachedBodies;
