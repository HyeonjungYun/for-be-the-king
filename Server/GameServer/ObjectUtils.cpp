#include "pch.h"
#include "ObjectUtils.h"
#include "Player.h"
#include "GameSession.h"

atomic<int64> ObjectUtils::s_idGenerator = RUNTIME_ID_BASE;

atomic<uint64> GNextInstanceId = 1;
atomic<uint64>GNextBagId = BAG_ID_BASE;

namespace
{
	bool QueryScalar(DBConnection* conn, const char* query, OUT uint64& out)
	{
		MYSQL_RES* res = conn->Query(query);
		if (res == nullptr)
		{
			cout << "[BOOT] query failed: " << conn->GetError() << endl;
			return false;
		}

		bool ok = false;

		if (MYSQL_ROW row = ::mysql_fetch_row(res))
		{
			// COALESCE를 썻으므로 NULL이 올 수 없지만, row[0] 검사는 남긴다.
			out = (row[0] != nullptr) ? ::strtoull(row[0], nullptr, 10) : 0;
			ok = true;
		}

		conn->FreeResult(res);
		return ok;
	}
}

PlayerRef ObjectUtils::CreatePlayer(GameSessionRef session, uint64 characterId)
{
	PlayerRef player = make_shared<Player>();
	player->objectInfo->set_object_id(characterId);
	player->posInfo->set_object_id(characterId);

	player->session = session;
	session->player.store(player);

	return player;
}

bool RestoreIdCounters(DBConnection* conn)
{
	uint64 maxInstance = 0;
	uint64 maxBag = 0;

	if (QueryScalar(conn, "SELECT COALESCE(MAX(instance_id), 0) FROM item_instances", OUT maxInstance) == false)
		return false;

	if (QueryScalar(conn, "SELECT COALESCE(MAX(bag_id), 0) FROM bags", OUT maxBag) == false)
		return false;

	GNextInstanceId.store(maxInstance + 1);

	GNextBagId.store((maxBag == 0) ? BAG_ID_BASE : maxBag + 1);

	cout << "[BOOT] id counters restored - instance=" << GNextInstanceId.load() << " bag=" << GNextBagId.load() << endl;

	return true;
}

bool PurgeExpiredBags(DBConnection* conn)
{
	if (conn->Excute(
		"DELETE FROM item_instances WHERE bag_id IN"
		"(SELECT bag_id FROM bags WHERE expires_at <= NOW())") == false)
	{
		cout << "[BOOT] purge items failed: " << conn->GetError() << endl;
		return false;
	}

	const uint64 removedItems = conn->GetAffectedRows();
	if (conn->Excute("DELETE FROM bags WHERE expires_at <= NOW()") == false)
	{
		cout << "[BOOT] purge bags failed: " << conn->GetError() << endl;
		return false;
	}

	cout << "[BOOT] expired bags purged - bags=" << conn->GetAffectedRows()
		<< " items=" << removedItems << endl;

	return true;
}
