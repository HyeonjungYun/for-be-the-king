#pragma once

class ObjectUtils
{
public:
	static PlayerRef CreatePlayer(GameSessionRef session, uint64 characterId);

private:
	static atomic<int64> s_idGenerator;
};

constexpr uint64 RUNTIME_ID_BASE = 1'000'000'000;
constexpr uint64 BAG_ID_BASE = 2'000'000'000;

extern atomic<uint64> GNextInstanceId;
extern atomic<uint64> GNextBagId;

bool RestoreIdCounters(DBConnection* conn);

bool PurgeExpiredBags(DBConnection* conn);

