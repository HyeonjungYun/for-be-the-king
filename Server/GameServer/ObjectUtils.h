#pragma once

struct DeathSnapshot;

class ObjectUtils
{
public:
	static PlayerRef CreatePlayer(GameSessionRef session, uint64 characterId);
	static BagRef CreateBag(uint64 bagId, uint32 floorId, float x, float y, float z, uint64 expiresAtUs, uint64 ownerCharacterId);

private:
	static atomic<int64> s_idGenerator;
};

constexpr uint64 RUNTIME_ID_BASE = 1'000'000'000;
constexpr uint64 BAG_ID_BASE = 2'000'000'000;

extern atomic<uint64> GNextInstanceId;
extern atomic<uint64> GNextBagId;

bool RestoreIdCounters(DBConnection* conn);

bool PurgeExpiredBags(DBConnection* conn);

bool LoadLiveBags(DBConnection* conn);

bool DeleteExpiredBags(DBConnection* conn, const vector<uint64>& bagIds, const vector<uint64>& instanceIds);

bool CommitDeath(DBConnection* conn, const DeathSnapshot& snap);

extern atomic<bool> GFailDeathCommit;

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
