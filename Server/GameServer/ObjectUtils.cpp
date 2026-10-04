#include "pch.h"
#include "ObjectUtils.h"
#include "Player.h"
#include "Bag.h"
#include "GameSession.h"
#include "Room.h"
#include "ItemTable.h"
#include <thread>

atomic<int64> ObjectUtils::s_idGenerator = RUNTIME_ID_BASE;
atomic<uint64> GNextInstanceId = 1;
atomic<uint64>GNextBagId = BAG_ID_BASE;
atomic<bool> GFailDeathCommit = false;
atomic<uint32> GDeathCommitDelayMs = 0;
atomic<uint32> GBaseCampDelayMs = 0;
atomic<int32> GFailEscapeCommits = 0;
DeathCommitTracker GDeathCommits;
atomic<uint64> GDisconnectGraceUs = 180'000'000;
DetachedBodies GDetachedBodies;

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

namespace
{
	constexpr uint32 ESCAPE_RETRY_FIRST_MS = 100;
	constexpr uint32 ESCAPE_RETRY_MAX_MS = 5000;

	string JoinIds(const vector<uint64>& ids)
	{
		string s;
		for (size_t i = 0; i < ids.size(); i++)
		{
			if (i > 0)
				s += ',';
			s += to_string(ids[i]);
		}

		return s;
	}

	// 이번 런에 생긴 아이템(isNew)은 DB 행이 없다 - 커밋이 새 행으로 넣는다 (상자 S-4).
	constexpr const char* INSERT_ITEMS =
		"INSERT INTO item_instances (instance_id, item_type_id, grade, level, state, slot, "
		"owner_character_id, bag_id, skill_id_primary, skill_id_secondary) VALUES ";

	// INSERT_ITEMS 뒤에 붙일 한 행. 주인과 가방은 둘 중 하나만 있다 (chk_owner_xor_bag) - 없는 쪽은 0 을 넘긴다.
	void AppendItemValues(OUT string& values, const ItemEntry& e, Protocol::ItemState state, int32 slot, uint32 level, uint64 ownerId, uint64 bagId)
	{
		if (values.empty() == false)
			values += ',';

		values += "(" + to_string(e.instanceId) + "," + to_string(e.itemTypeId) + "," + to_string(static_cast<int32>(e.grade))
			+ "," + to_string(level) + "," + to_string(static_cast<int32>(state)) + "," + to_string(slot)
			+ "," + ((ownerId != 0) ? to_string(ownerId) : string("NULL"))
			+ "," + ((bagId != 0) ? to_string(bagId) : string("NULL"))
			+ "," + to_string(e.skillIdPrimary) + "," + to_string(e.skillIdSecondary) + ")";
	}

	bool TryCommitEscape(DBConnection* conn, const EscapeSnapshot& snap)
	{
		const uint64 characterId = snap.characterId;
		char query[256];

		if (conn->BeginTransaction() == false)
			return false;

		::snprintf(query, sizeof(query),
			"SELECT character_id FROM characters WHERE character_id = %llu FOR UPDATE", characterId);

		MYSQL_RES* lockRes = conn->Query(query);
		bool ok = (lockRes != nullptr);
		if (lockRes != nullptr)
			conn->FreeResult(lockRes);

		// DB 에 이미 있는 것은 소유 · 상태만 바꾸고, 이번 런에 생긴 것은 새 행으로 넣는다.
		vector<uint64> carriedIds;
		string newValues;

		for (const ItemEntry& e : snap.carried)
		{
			if (e.isNew)
				AppendItemValues(OUT newValues, e, Protocol::ITEM_STATE_CARRIED, 0, e.level, characterId, 0);
			else
				carriedIds.push_back(e.instanceId);
		}

		if (ok && carriedIds.empty() == false)
		{
			const string update =
				"UPDATE item_instances SET owner_character_id = " + to_string(characterId)
				+ ", bag_id = NULL, state = " + to_string(static_cast<int32>(Protocol::ITEM_STATE_CARRIED))
				+ ", slot = 0 WHERE instance_id IN (" + JoinIds(carriedIds) + ")";

			ok = conn->Excute(update.c_str());
		}

		uint64 expected = snap.carried.size();
		for (int32 i = 0; ok && i < SKILL_SLOT_COUNT; i++)
		{
			const ItemEntry& e = snap.equipped[i];
			if (e.instanceId == 0)
				continue;

			expected++;

			if (e.isNew)
			{
				AppendItemValues(OUT newValues, e, Protocol::ITEM_STATE_EQUIPPED, i + 1, e.level, characterId, 0);
				continue;
			}

			::snprintf(query, sizeof(query),
				"UPDATE item_instances SET owner_character_id = %llu, bag_id = NULL, state = %d, slot = %d WHERE instance_id = %llu",
				characterId, static_cast<int32>(Protocol::ITEM_STATE_EQUIPPED), i + 1, e.instanceId);

			ok = conn->Excute(query);
		}

		if (ok && newValues.empty() == false)
		{
			const string insert = INSERT_ITEMS + newValues;
			ok = conn->Excute(insert.c_str());
		}

		if (ok && snap.droppedIds.empty() == false)
		{
			const string del = "DELETE FROM item_instances WHERE instance_id IN (" + JoinIds(snap.droppedIds) + ")";
			ok = conn->Excute(del.c_str());
		 }

		if (ok)
		{
			::snprintf(query, sizeof(query),
				"SELECT COUNT(*) FROM item_instances WHERE owner_character_id = %llu AND state IN (%d, %d)",
				characterId, static_cast<int32>(Protocol::ITEM_STATE_CARRIED), static_cast<int32>(Protocol::ITEM_STATE_EQUIPPED));

			if (MYSQL_RES* countRes = conn->Query(query))
			{
				uint64 actual = 0;
				if (MYSQL_ROW row = ::mysql_fetch_row(countRes))
					actual = ::strtoull(row[0], nullptr, 10);
				conn->FreeResult(countRes);

				if (actual != expected)
					LOG_LINE << "[ESCAPE] owned count mismatch - expected=" << expected << " actual=" << actual << " char=" << characterId << '\n';
			}
		}

		// 검증 전용 - 쓰기를 다 한 뒤에 실패시켜 롤백과 재시도를 본다
		if (ok && GFailEscapeCommits.fetch_sub(1) > 0)
		{
			LOG_LINE << "[ESCAPE] injected failure - char=" << characterId << '\n';
			ok = false;
		}

		if (ok && conn->Commit())
			return true;

		conn->Rollback();
		return false;
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

	const uint64 removeBags = conn->GetAffectedRows();

	if (conn->Excute("DELETE FROM item_instances WHERE bag_id IS NOT NULL AND bag_id NOT IN (SELECT bag_id FROM bags)") == false)
	{
		LOG_LINE << "[BOOT] purge orphan items failed: " << conn->GetError() << '\n';
		return false;
	}

	LOG_LINE << "[BOOT] expired bags purged - bags=" << removeBags
		<< " items=" << removedItems << " orphan_items=" << conn->GetAffectedRows() << '\n';

	return true;
}

bool ValidateItemRows(DBConnection* conn)
{
	// 조합별로 묶어 읽는다 - 행이 늘어도 결과는 (종류 × 상태 × 슬롯 × 스킬) 조합 수다.
	MYSQL_RES* res = conn->Query(
		"SELECT item_type_id, state, slot, skill_id_primary, skill_id_secondary, COUNT(*), MIN(instance_id) "
		"FROM item_instances GROUP BY item_type_id, state, slot, skill_id_primary, skill_id_secondary");

	if (res == nullptr)
	{
		LOG_LINE << "[BOOT] item row query failed: " << conn->GetError() << '\n';
		return false;
	}

	bool valid = true;
	uint64 warnings = 0;

	while (MYSQL_ROW row = ::mysql_fetch_row(res))
	{
		const uint32 typeId = static_cast<uint32>(::strtoul(row[0], nullptr, 10));
		const int32 state = ::atoi(row[1]);
		const int32 slot = ::atoi(row[2]);
		const uint32 skillIds[2] = { static_cast<uint32>(::strtoul(row[3], nullptr, 10)), static_cast<uint32>(::strtoul(row[4], nullptr, 10)) };
		const char* rows = row[5];
		const char* sample = row[6];

		// 표에 없는 종류 - 클라가 그릴 수도, 서버가 착용을 판정할 수도 없다.
		const ItemTypeDef* def = ItemTable::Find(typeId);
		if (def == nullptr)
		{
			LOG_LINE << "[BOOT] item rows with unknown type - type=" << typeId << " rows=" << rows << " e.g. instance=" << sample << '\n';
			valid = false;
			continue;
		}

		// 착용 슬롯이 종류의 부위와 다르다 - 착용 검사 없이 들어간 행이다.
		if (state == Protocol::ITEM_STATE_EQUIPPED && slot != def->equipSlot)
		{
			LOG_LINE << "[BOOT] equipped rows in wrong slot - type=" << typeId << " slot=" << slot
				<< " expected=" << def->equipSlot << " rows=" << rows << " e.g. instance=" << sample << '\n';
			valid = false;
			continue;
		}

		// 스킬이 그 종류의 풀에 없다 - 시드 픽스처(102 · 502)가 일부러 그렇다. 경고만 남긴다.
		const vector<SkillPoolEntry>* pool = ItemTable::FindPool(def->skillPoolId);

		for (uint32 skillId : skillIds)
		{
			if (skillId == 0)
				continue;

			bool inPool = false;
			if (pool != nullptr)
			{
				for (const SkillPoolEntry& e : *pool)
				{
					if (e.skillId == skillId)
						inPool = true;
				}
			}

			if (inPool == false)
			{
				LOG_LINE << "[BOOT] warning - skill " << skillId << " is not in pool '" << def->skillPoolId
					<< "' type=" << typeId << " rows=" << rows << " e.g. instance=" << sample << '\n';
				warnings++;
			}
		}
	}

	conn->FreeResult(res);

	LOG_LINE << "[BOOT] item rows checked - " << (valid ? "ok" : "INVALID") << " warnings=" << warnings << '\n';
	return valid;
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

bool DeleteExpiredBags(DBConnection* conn, const vector<uint64>& bagIds, const vector<uint64>& instanceIds)
{
	auto join = [](const vector<uint64>& v)
		{
			string s;
			for (size_t i = 0; i < v.size(); i++)
			{
				if (i > 0)
					s += ',';
				s += to_string(v[i]);
			}
			return s;
		};

	if (conn->BeginTransaction() == false)
	{
		LOG_LINE << "[BAG] expire delete begin failed - err=" << conn->GetLastErrorNo() << '\n';
		return false;
	}

	bool ok = true;

	// 빈 가방만 만료되면 아이템 목록이 비어 있다.
	if (instanceIds.empty() == false)
	{
		const string delItems = "DELETE FROM item_instances WHERE instance_id IN (" + join(instanceIds) + ")";
		ok = conn->Excute(delItems.c_str());
	}

	if (ok)
	{
		const string delBags = "DELETE FROM bags WHERE  bag_id IN (" + join(bagIds) + ")";
		ok = conn->Excute(delBags.c_str());
	}

	if (ok && conn->Commit())
		return true;

	conn->Rollback();
	LOG_LINE << "[BAG] expire delete failed - bags=" << bagIds.size() << " err=" << conn->GetLastErrorNo() << " (boot purge will clean up)\n";
	return false;
}

bool CommitDeath(DBConnection* conn, const DeathSnapshot& snap)
{
	// DB 에 이미 있는 것은 가방으로 옮기고(UPDATE), 이번 런에 생긴 것은 가방 소속 새 행으로 넣는다(INSERT).
	vector<uint64> existingIds;
	string newValues;

	for (const ItemEntry& e : snap.items)
	{
		if (e.isNew)
			AppendItemValues(OUT newValues, e, Protocol::ITEM_STATE_IN_BAG, 0, 0, 0, snap.bagId);
		else
			existingIds.push_back(e.instanceId);
	}

	const string ids = JoinIds(existingIds);

	string dropped;
	for (size_t i = 0; i < snap.droppedIds.size(); i++)
	{
		if (i > 0)
			dropped += ',';
		dropped += to_string(snap.droppedIds[i]);
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

		if (ok && existingIds.empty() == false)
		{
			const string update =
				"UPDATE item_instances SET owner_character_id = NULL, bag_id = " + to_string(snap.bagId)
				+ ", state = " + to_string(static_cast<int32>(Protocol::ITEM_STATE_IN_BAG))
				+ ", slot = 0, level = 0 WHERE instance_id IN (" + ids + ")";

			ok = conn->Excute(update.c_str());

			// DB 에 있어야 할 아이템(isNew 아님)은 전부 있어야 한다. 모자라면 어딘가 어긋났다 - 커밋하지 않는다.
			if (ok && conn->GetAffectedRows() != existingIds.size())
			{
				LOG_LINE << "[DEATH] item count mismatch - expected=" << existingIds.size() << " affected=" << conn->GetAffectedRows() << " char=" << snap.characterId << '\n';
				ok = false;
			}
		}

		// 같은 트랜잭션 - 가방 행 · 소유 이전과 함께 확정되거나 함께 롤백된다 (AC-CH-07)
		if (ok && newValues.empty() == false)
		{
			const string insert = INSERT_ITEMS + newValues;
			ok = conn->Excute(insert.c_str());
		}

		//  이번 런에서 버린 아이템을 파기한다. 이미 없으면 목적은 달성된 것이라 개수가 달라도 실패로 보지않는다.
		if (ok && snap.droppedIds.empty() == false)
		{
			const string del = "DELETE FROM item_instances WHERE instance_id IN (" + dropped + ")";
			ok = conn->Excute(del.c_str());
			if (ok && conn->GetAffectedRows() != snap.droppedIds.size())
				LOG_LINE << "[DEATH] dropped rows already gone - expected=" << snap.droppedIds.size() << " affected=" << conn->GetAffectedRows() << " char=" << snap.characterId << '\n';
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

void CommitEscape(DBConnection* conn, const EscapeSnapshot& snap)
{
	uint32 backoffMs = ESCAPE_RETRY_FIRST_MS;

	// 탈출만 무한 재시도 - 다른 경로는 실패해도 되돌릴 대상이 있지만, 탈출은 얻은 것을 확정하는 유일한 지점
	for (int32 attempt = 1; ; attempt++)
	{
		if (TryCommitEscape(conn, snap))
			return;

		LOG_LINE << "[ESCAPE] attempt " << attempt << " failed - char=" << snap.characterId
			<< " retry in " << backoffMs << "ms\n";

		this_thread::sleep_for(chrono::milliseconds(backoffMs));
		backoffMs = (backoffMs * 2 < ESCAPE_RETRY_MAX_MS) ? backoffMs * 2 : ESCAPE_RETRY_MAX_MS;
	}
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

void DetachedBodies::Add(uint64 characterId, PlayerRef body)
{
	WRITE_LOCK;
	_bodies[characterId] = body;
}

PlayerRef DetachedBodies::TryTake(uint64 characterId)
{
	WRITE_LOCK;
	auto findIt = _bodies.find(characterId);
	if (findIt == _bodies.end())
		return nullptr;

	PlayerRef body = findIt->second;
	_bodies.erase(findIt);
	return body;
}

bool DetachedBodies::Contains(uint64 characterId)
{
	WRITE_LOCK;
	return _bodies.find(characterId) != _bodies.end();
}
