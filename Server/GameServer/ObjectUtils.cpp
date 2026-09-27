#include "pch.h"
#include "ObjectUtils.h"
#include "Player.h"
#include "Bag.h"
#include "GameSession.h"
#include "Room.h"
#include <thread>

atomic<int64> ObjectUtils::s_idGenerator = RUNTIME_ID_BASE;
atomic<uint64> GNextInstanceId = 1;
atomic<uint64>GNextBagId = BAG_ID_BASE;
atomic<bool> GFailDeathCommit = false;
atomic<uint32> GDeathCommitDelayMs = 0;
DeathCommitTracker GDeathCommits;

namespace
{
	bool QueryScalar(DBConnection* conn, const char* query, OUT uint64& out)
	{
		MYSQL_RES* res = conn->Query(query);
		if (res == nullptr)
		{
			LOG_LINE << "[BOOT] query failed: " << conn->GetError() << '\n';
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

	constexpr int32 DEATH_COMMIT_ATTEMPTS = 3;
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

BagRef ObjectUtils::CreateBag(uint64 bagId, uint32 floorId, float x, float y, float z, uint64 expiresAtUs, uint64 ownerCharacterId)
{
	BagRef bag = make_shared<Bag>();
	bag->objectInfo->set_object_id(bagId);
	bag->posInfo->set_object_id(bagId);
	bag->posInfo->set_x(x);
	bag->posInfo->set_y(y);
	bag->posInfo->set_z(z);

	bag->floorId = floorId;
	bag->expiresAtUs = expiresAtUs;
	bag->ownerCharacterId = ownerCharacterId;

	return bag;
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

	LOG_LINE << "[BOOT] id counters restored - instance=" << GNextInstanceId.load() << " bag=" << GNextBagId.load() << '\n';

	return true;
}

bool PurgeExpiredBags(DBConnection* conn)
{
	if (conn->Excute(
		"DELETE FROM item_instances WHERE bag_id IN"
		"(SELECT bag_id FROM bags WHERE expires_at <= NOW())") == false)
	{
		LOG_LINE << "[BOOT] purge items failed: " << conn->GetError() << '\n';
		return false;
	}

	const uint64 removedItems = conn->GetAffectedRows();
	if (conn->Excute("DELETE FROM bags WHERE expires_at <= NOW()") == false)
	{
		LOG_LINE << "[BOOT] purge bags failed: " << conn->GetError() << '\n';
		return false;
	}

	LOG_LINE << "[BOOT] expired bags purged - bags=" << conn->GetAffectedRows()
		<< " items=" << removedItems << '\n';

	return true;
}

bool LoadLiveBags(DBConnection* conn)
{
	MYSQL_RES* bagRes = conn->Query(
		"SELECT bag_id, floor_id, x, y, z, TIMESTAMPDIFF(MICROSECOND, NOW(), expires_at) "
		"FROM bags WHERE expires_at > NOW()");

	if (bagRes == nullptr)
	{
		LOG_LINE << "[BOOT] bag query failed: " << conn->GetError() << '\n';
		return false;
	}

	const uint64 nowUs = Utils::NowMicroseconds();
	unordered_map<uint64, BagRef> bags;
	bool valid = true;

	while (MYSQL_ROW row = ::mysql_fetch_row(bagRes))
	{
		const uint64 bagId = ::strtoull(row[0], nullptr, 10);
		const uint32 floorId = static_cast<uint32>(::strtoul(row[1], nullptr, 10));

		// 없는 층의 가방은 데이터 손상이다. 조용히 버리지 않고 기동을 거부
		if (floorId >= FLOOR_COUNT)
		{
			LOG_LINE << "[BOOT] bag on invalid floor - bag=" << bagId << " floor=" << floorId << '\n';
			valid = false;
			break;
		}

		const int64 remainingUs = ::strtoll(row[5], nullptr, 10);

		bags[bagId] = ObjectUtils::CreateBag(bagId, floorId,
			static_cast<float>(::atof(row[2])),
			static_cast<float>(::atof(row[3])),
			static_cast<float>(::atof(row[4])),
			nowUs + static_cast<uint64>(remainingUs > 0 ? remainingUs : 0),
			0);
	}

	conn->FreeResult(bagRes);

	if (valid == false)
		return false;

	MYSQL_RES* itemRes = conn->Query(
		"SELECT bag_id, instance_id, item_type_id, grade, level, skill_id_primary, skill_id_secondary "
		"FROM item_instances WHERE bag_id IS NOT NULL");

	if (itemRes == nullptr)
	{
		LOG_LINE << "[BOOT] bag item query failed: " << conn->GetError() << '\n';
		return false;
	}

	uint64 itemCount = 0;
	uint64 strayCount = 0;

	while (MYSQL_ROW row = ::mysql_fetch_row(itemRes))
	{
		auto findIt = bags.find(::strtoull(row[0], nullptr, 10));
		if (findIt == bags.end())
		{
			strayCount++;
			continue;
		}

		ItemEntry e;
		e.instanceId = ::strtoull(row[1], nullptr, 10);
		e.itemTypeId = static_cast<uint32>(::atoi(row[2]));
		e.grade = static_cast<Protocol::ItemGrade>(::atoi(row[3]));
		e.level = static_cast<uint32>(::atoi(row[4]));
		e.skillIdPrimary = static_cast<uint32>(::atoi(row[5]));
		e.skillIdSecondary = static_cast<uint32>(::atoi(row[6]));

		findIt->second->items.push_back(e);
		itemCount++;
	}

	conn->FreeResult(itemRes);

	// 층별 Room에 적재. 워커가 없으므로 DoAsync가 이 스레드에서 즉시 실행된다.
	for (auto& [bagId, bag] : bags)
		GRooms[bag->floorId]->DoAsync(&Room::AddBag, bag);

	LOG_LINE << "[BOOT] live bags loaded - bags=" << bags.size() << " items=" << itemCount << " stray_items=" << strayCount << '\n';

	return true;
}

bool CommitDeath(DBConnection* conn, const DeathSnapshot& snap)
{
	string ids;
	for (size_t i = 0; i < snap.items.size(); i++)
	{
		if (i > 0)
			ids += ',';
		ids += to_string(snap.items[i].instanceId);
	}

	for (int32 attempt = 1; attempt <= DEATH_COMMIT_ATTEMPTS; attempt++)
	{
		if (conn->BeginTransaction() == false)
		{
			LOG_LINE << "[DEATH] begin failed - char=" << snap.characterId << " err=" << conn->GetLastErrorNo() << '\n';
			continue;
		}

		char insert[256];
		::snprintf(insert, sizeof(insert),
			"INSERT INTO bags (bag_id, floor_id, x, y, z, expires_at) "
			"VALUES (%llu, %u, %f, %f, %f, NOW() + INTERVAL %llu SECOND)",
			snap.bagId, snap.floorId, snap.x, snap.y, snap.z, BAG_LIFETIME_US / 1'000'000);

		bool ok = conn->Excute(insert);

		// 가방 INSERT는 성공시킨 뒤에 실패시킨다 - 롤백이 그것까지 되돌리는지가 원자성 검증이다.
		if (ok && GFailDeathCommit.load())
		{
			LOG_LINE << "[DEATH] injected failure after bag insert - bag=" << snap.bagId << '\n';
			ok = false;
		}

		if (ok && snap.items.empty() == false)
		{
			const string update =
				"UPDATE item_instances SET owner_character_id = NULL, bag_id = " + to_string(snap.bagId)
				+ ", state = " + to_string(static_cast<int32>(Protocol::ITEM_STATE_IN_BAG))
				+ ", slot = 0, level = 0 WHERE instance_id IN (" + ids + ")";

			ok = conn->Excute(update.c_str());

			// 메모리에 있는 아이템은 전부 DB에 있어야 한다. 모자라면 어딘가 어긋났다 - 커밋하지 않는다.
			if (ok && conn->GetAffectedRows() != snap.items.size())
			{
				LOG_LINE << "[DEATH] item count mismatch - expected=" << snap.items.size() << " affected=" << conn->GetAffectedRows() << " char=" << snap.characterId << '\n';
				ok = false;
			}
		}

		// 검증 전용 - 커밋을 늦춰 재입장 가드(DeathCommitTracker)가 실제로 발동하는지 본다.
		const uint32 delayMs = GDeathCommitDelayMs.load();
		if (ok && delayMs > 0)
			this_thread::sleep_for(chrono::milliseconds(delayMs));

		if (ok && conn->Commit())
			return true;

		conn->Rollback();
		LOG_LINE << "[DEATH] attempt " << attempt << " failed - char=" << snap.characterId << " err=" << conn->GetLastErrorNo() << '\n';
	}

	return false;
}

bool DeathCommitTracker::TryBegin(uint64 characterId)
{
	WRITE_LOCK;
	return _pending.insert(characterId).second;
}

void DeathCommitTracker::End(uint64 characterId)
{
	WRITE_LOCK;
	_pending.erase(characterId);
}

bool DeathCommitTracker::IsPending(uint64 characterId)
{
	WRITE_LOCK;
	return _pending.find(characterId) != _pending.end();
}
