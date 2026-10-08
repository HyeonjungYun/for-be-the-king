#include "pch.h"
#include "ObjectUtils.h"
#include "Player.h"
#include "Bag.h"
#include "GameSession.h"
#include "GameSessionManager.h"
#include "Room.h"
#include "ItemTable.h"
#include "ChestSpawnTable.h"
#include "StarterKitTable.h"
#include "DBError.h"
#include <thread>

atomic<int64> ObjectUtils::s_idGenerator = RUNTIME_ID_BASE;
atomic<uint64> GNextInstanceId = 1;
atomic<uint64>GNextBagId = BAG_ID_BASE;
atomic<bool> GFailDeathCommit = false;
atomic<uint32> GDeathCommitDelayMs = 0;
atomic<uint32> GBaseCampDelayMs = 0;
atomic<bool> GBotRegear = false;
atomic<int32> GFakeLoginFail = 0;
atomic<uint32> GLoginDelayMs = 0;
atomic<bool> GEntryFromDb = false;
atomic<int32> GFailEscapeCommits = 0;
DeathCommitTracker GDeathCommits;
atomic<uint64> GDisconnectGraceUs = 180'000'000;
DetachedBodies GDetachedBodies;
atomic<bool> GDebugGold = false;
atomic<bool> GDebugCommands = false;

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

	// 베이스캠프 동기화가 사망 커밋을 기다리는 한도 - 100 ms × 100 = 10 초
	constexpr int32 BASECAMP_SYNC_WAIT_TRIES = 100;
	constexpr uint32 BASECAMP_SYNC_WAIT_MS = 100;
}

namespace
{
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

	// INSERT_ITEMS 의 관대 판 - 이미 있는 id(중복 키) · 제약 위반 행은 건너뛰고 나머지를 넣는다
	string InsertItemsSql(CommitMode mode)
	{
		return (mode == CommitMode::Lenient) ? string("INSERT IGNORE") + (INSERT_ITEMS + 6) : string(INSERT_ITEMS);
	}

	// 이 DB 스레드의 마지막 저장 실패 오류 번호 - Rollback 이 mysql_errno 를 덮어쓰므로 여기 남긴다 (RunCommit 이 로그 · 재연결에 쓴다)
	thread_local uint32 t_commitErrno = 0;

	// 실패한 문장의 오류 번호를 Rollback 이 덮어쓰기 전에 읽어 분류한다 (CommitRetry - 다시 하면 되나)
	CommitOutcome RollbackAndClassify(DBConnection* conn)
	{
		const uint32 err = conn->GetLastErrorNo();
		t_commitErrno = err;
		conn->Rollback();
		return CommitRetry::FromMysqlError(err);
	}

	enum class MarkResult : uint8 { Marked, AlreadyApplied, Failed };

	// 모든 런 끝 저장은 트랜잭션 맨 앞에서 적용 표시를 남긴다 (CommitJournal.h). 기록 id 는 영문 · 숫자 · '-' 뿐.
	// 🔴 2026-10-08 리뷰: 예전엔 파일로 남긴 저장(재시도에 들어간 것)만 표시했다 - 첫 시도의 COMMIT 이 DB 에선 됐는데 응답을 잃으면
	//    (연결 끊김 → 일시 실패) 표시 없이 재시도가 돌아 탈출 골드 · 만료 골드가 두 번 들어갔다. 이제 id 는 큐에 넣을 때 정하고 늘 표시한다.
	//    맨 앞인 이유: 앞선 시도가 커밋됐으면 상태가 이미 바뀌어 엄격 단계의 쓰기가 "어긋남" 으로 영구 실패하기 전에 알아야 한다
	MarkResult MarkApplied(DBConnection* conn, const string& journalId, const char* kind, uint64 characterId)
	{
		const string insert = "INSERT INTO applied_commits (journal_id, character_id, kind) VALUES ('" + conn->Escape(journalId) + "', "
			+ to_string(characterId) + ", '" + kind + "')";

		if (conn->Excute(insert.c_str()))
			return MarkResult::Marked;

		// 이미 있다 = 이 기록은 전에 커밋됐다 (커밋 뒤 파일을 지우기 전에 서버가 꺼졌다). 지금 트랜잭션은 버리고 성공으로 친다 - 두 번 적용하지 않는다
		if (conn->GetLastErrorNo() == 1062)
			return MarkResult::AlreadyApplied;

		return MarkResult::Failed;
	}

	// TryCommit* 의 트랜잭션 맨 앞 공통 - journalId 가 비었으면 아무것도 안 한다(시험 · 옛 호출). false 면 이미 롤백했다:
	// result = Ok 는 "전에 적용됐다", 그 밖은 실패 분류
	bool MarkOrFinish(DBConnection* conn, const string& journalId, const char* kind, uint64 characterId, OUT CommitOutcome& result)
	{
		if (journalId.empty())
			return true;

		switch (MarkApplied(conn, journalId, kind, characterId))
		{
		case MarkResult::Marked:
			return true;
		case MarkResult::AlreadyApplied:
			LOG_LINE << "[JOURNAL] " << journalId << " already applied - this attempt rolled back, counted as done\n";
			conn->Rollback();
			result = CommitOutcome::Ok;
			return false;
		case MarkResult::Failed:
		default:
			result = RollbackAndClassify(conn);
			return false;
		}
	}

	// 저장이 끝났다 - 표시는 그 저장이 진행 중인 동안만 필요하다. 지우지 못해도 부팅이 비운다
	void ClearAppliedMark(DBConnection* conn, const string& journalId)
	{
		if (journalId.empty())
			return;

		const string del = "DELETE FROM applied_commits WHERE journal_id = '" + conn->Escape(journalId) + "'";
		if (conn->Excute(del.c_str()) == false)
			LOG_LINE << "[JOURNAL] cannot clear applied mark " << journalId << " err=" << conn->GetLastErrorNo() << " (boot will clear it)\n";
	}

	// 검증 전용 (FBTK_FAKE_ACK_LOSS=N) - 처음 N 번의 커밋은 DB 에선 됐는데 응답을 잃은 것처럼 연결 끊김으로 돌려준다.
	// 재시도가 표시를 보고 두 번 적용하지 않는지 본다 (2026-10-08 리뷰 ①)
	bool FakeAckLoss(const char* tag, uint64 characterId)
	{
		if (GFakeCommitAckLoss.load() <= 0 || GFakeCommitAckLoss.fetch_sub(1) <= 0)
			return false;

		LOG_LINE << "[" << tag << "] injected ack loss - committed but reporting 2013 (TEST) char=" << characterId << '\n';
		t_commitErrno = 2013;
		return true;
	}

	// 이미 적용된 사망 저장 - 그때 실제로 가방에 들어간 것만 메모리 가방에 담는다. 관대 단계였으면 일부만 들어갔을 수 있다
	// (그 시도의 걸러 내기는 응답과 함께 잃었다). 엄격 단계였으면 전부 들어갔다 - 걸러도 그대로
	void KeepItemsActuallyInBag(DBConnection* conn, DeathSnapshot& snap)
	{
		const string select = "SELECT instance_id FROM item_instances WHERE bag_id = " + to_string(snap.bagId);
		MYSQL_RES* res = conn->Query(select.c_str());
		if (res == nullptr)
		{
			LOG_LINE << "[DEATH] 🔴 already applied but cannot read bag " << snap.bagId << " err=" << conn->GetLastErrorNo() << " - keeping the snapshot\n";
			return;
		}

		unordered_set<uint64> inBag;
		while (MYSQL_ROW row = ::mysql_fetch_row(res))
			inBag.insert(::strtoull(row[0], nullptr, 10));
		conn->FreeResult(res);

		const size_t before = snap.items.size();
		snap.items.erase(remove_if(snap.items.begin(), snap.items.end(),
			[&inBag](const ItemEntry& e) { return inBag.count(e.instanceId) == 0; }), snap.items.end());

		if (snap.items.size() != before)
			LOG_LINE << "[DEATH] already applied - bag " << snap.bagId << " holds " << snap.items.size() << " of " << before << " char=" << snap.characterId << '\n';
	}
}

string GCommitJournalDir;
atomic<int32> GFakeCommitAckLoss = 0;

CommitOutcome TryCommitEscape(DBConnection* conn, const EscapeSnapshot& snap, CommitMode mode, const string& journalId)
{
	const uint64 characterId = snap.characterId;
	const bool lenient = (mode == CommitMode::Lenient);
	char query[256];

	// 남의 것은 가져오지 않는다 - 주인 없는(가방 · 상자에서 주운 - DB 에선 아직 그 가방 소속) 것과 내 것만. 두 단계 모두.
	// 회귀: 예전엔 id 만 봤다 - DB 에서 다른 캐릭터 창고로 간 행도 내 소지로 끌어왔다 (2026-10-08 사망 경로에서 발견, 탈출도 같은 구멍)
	const string mineOrUnowned = " AND (owner_character_id IS NULL OR owner_character_id = " + to_string(characterId) + ")";

	if (conn->BeginTransaction() == false)
		return RollbackAndClassify(conn);

	// 적용 표시를 맨 먼저 - 이미 있으면 전에 커밋됐다(응답만 잃었다). 쓰기를 하나도 하지 않고 성공으로 친다
	if (CommitOutcome marked; MarkOrFinish(conn, journalId, "ESCAPE", characterId, OUT marked) == false)
		return marked;

	::snprintf(query, sizeof(query),
		"SELECT character_id FROM characters WHERE character_id = %llu FOR UPDATE", characterId);

	MYSQL_RES* lockRes = conn->Query(query);
	if (lockRes == nullptr)
		return RollbackAndClassify(conn);
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

	// 엄격: 메모리가 말한 DB 행(isNew 제외)이 전부 있고 전부 내 것 또는 주인 없는 것이어야 한다 - 아니면 데이터가 어긋났다 → 관대 단계로.
	// 바뀐 행 수로는 셀 수 없어(값이 그대로인 행은 0) 미리 잠그고 센다
	if (lenient == false)
	{
		vector<uint64> existingIds = carriedIds;
		for (const ItemEntry& e : snap.equipped)
		{
			if (e.instanceId != 0 && e.isNew == false)
				existingIds.push_back(e.instanceId);
		}

		if (existingIds.empty() == false)
		{
			const string check = "SELECT COUNT(*) FROM item_instances WHERE instance_id IN (" + JoinIds(existingIds) + ")" + mineOrUnowned + " FOR UPDATE";
			MYSQL_RES* res = conn->Query(check.c_str());
			if (res == nullptr)
				return RollbackAndClassify(conn);

			uint64 found = 0;
			if (MYSQL_ROW row = ::mysql_fetch_row(res))
				found = ::strtoull(row[0], nullptr, 10);
			conn->FreeResult(res);

			if (found != existingIds.size())
			{
				LOG_LINE << "[ESCAPE] item rows mismatch - expected=" << existingIds.size() << " found=" << found << " char=" << characterId << '\n';
				conn->Rollback();
				return CommitOutcome::Permanent;
			}
		}
	}

	if (carriedIds.empty() == false)
	{
		const string update =
			string(lenient ? "UPDATE IGNORE" : "UPDATE") + " item_instances SET owner_character_id = " + to_string(characterId)
			+ ", bag_id = NULL, state = " + to_string(static_cast<int32>(Protocol::ITEM_STATE_CARRIED))
			+ ", slot = 0 WHERE instance_id IN (" + JoinIds(carriedIds) + ")" + mineOrUnowned;

		// 바뀐 행 수로는 셀 수 없다 - 원래 내 소지품(값이 그대로인 행)은 MySQL 이 0 으로 센다. 어긋남은 아래 소유 개수 확인이 본다
		if (conn->Excute(update.c_str()) == false)
			return RollbackAndClassify(conn);
	}

	uint64 expected = snap.carried.size();
	for (int32 i = 0; i < SKILL_SLOT_COUNT; i++)
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

		const string update =
			string(lenient ? "UPDATE IGNORE" : "UPDATE") + " item_instances SET owner_character_id = " + to_string(characterId)
			+ ", bag_id = NULL, state = " + to_string(static_cast<int32>(Protocol::ITEM_STATE_EQUIPPED)) + ", slot = " + to_string(i + 1)
			+ " WHERE instance_id = " + to_string(e.instanceId) + mineOrUnowned;

		if (conn->Excute(update.c_str()) == false)
			return RollbackAndClassify(conn);
	}

	if (newValues.empty() == false)
	{
		const string insert = InsertItemsSql(mode) + newValues;
		if (conn->Excute(insert.c_str()) == false)
			return RollbackAndClassify(conn);
	}

	if (snap.droppedIds.empty() == false)
	{
		const string del = "DELETE FROM item_instances WHERE instance_id IN (" + JoinIds(snap.droppedIds) + ")";
		if (conn->Excute(del.c_str()) == false)
			return RollbackAndClassify(conn);
	}

	// 런 골드 전액 (escape-timer.md Core Rule 18) - 정상 플레이에서는 0 이라 문장을 보내지 않는다
	if (snap.gold > 0)
	{
		::snprintf(query, sizeof(query), "%s characters SET gold = gold + %llu WHERE character_id = %llu",
			lenient ? "UPDATE IGNORE" : "UPDATE", snap.gold, characterId);
		if (conn->Excute(query) == false)
			return RollbackAndClassify(conn);
	}

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
			LOG_LINE << "[ESCAPE] " << (lenient ? "🔴 lenient - " : "") << "owned count mismatch - expected=" << expected << " actual=" << actual << " char=" << characterId << '\n';
	}

	// 검증 전용 - 쓰기를 다 한 뒤에 실패시켜 롤백과 재시도를 본다. 일시적 실패를 흉내 낸다 (AC-ET-24)
	if (GFailEscapeCommits.fetch_sub(1) > 0)
	{
		LOG_LINE << "[ESCAPE] injected failure - char=" << characterId << '\n';
		conn->Rollback();
		return CommitOutcome::Transient;
	}

	if (conn->Commit() == false)
		return RollbackAndClassify(conn);

	if (FakeAckLoss("ESCAPE", characterId))
		return CommitOutcome::Transient;

	return CommitOutcome::Ok;
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

BagRef ObjectUtils::CreateChest(const ChestSpawn& spawn)
{
	// 만료 없음(expiresAtUs 0 → S_BAG_CONTENTS.remaining_ms 0 → 클라가 타이머를 숨기고 "상자"로 본다) · 주인 없음
	BagRef chest = CreateBag(CHEST_ID_BASE + spawn.chestId, spawn.floorId, spawn.x, spawn.y, spawn.z, 0, 0);
	chest->posInfo->set_yaw(spawn.yaw);

	chest->chestId = spawn.chestId;
	chest->tier = spawn.tier;
	chest->respawnUs = static_cast<uint64>(spawn.respawnSeconds) * 1'000'000;

	return chest;
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

CommitOutcome TryCommitDeath(DBConnection* conn, DeathSnapshot& snap, CommitMode mode, const string& journalId)
{
	const bool lenient = (mode == CommitMode::Lenient);

	// 관대 단계는 새 가방 id 로 - 엄격 단계가 막힌 이유가 가방 id 중복일 수도 있다 (롤백돼 옛 id 는 어디에도 없다).
	// 관대 단계 안의 재시도는 같은 id 를 다시 쓴다 - 커밋은 됐는데 응답을 잃은 뒤 재시도가 새 id 를 받으면, 표시를 보고 성공으로
	// 치더라도 메모리 가방은 DB 에 없는 id 가 되고 DB 의 진짜 가방은 재시작 때 따로 되살아난다 (2026-10-08 리뷰 ④)
	if (lenient)
	{
		if (snap.lenientBagId == 0)
			snap.lenientBagId = GNextBagId.fetch_add(1);
		snap.bagId = snap.lenientBagId;
	}

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

	if (conn->BeginTransaction() == false)
		return RollbackAndClassify(conn);

	// 적용 표시를 맨 먼저 (TryCommitEscape 와 같다). 이미 적용됐으면 그때 가방에 실제로 들어간 것만 남긴다
	if (CommitOutcome marked; MarkOrFinish(conn, journalId, "DEATH", snap.characterId, OUT marked) == false)
	{
		if (marked == CommitOutcome::Ok)
			KeepItemsActuallyInBag(conn, snap);
		return marked;
	}

	char insert[256];
	::snprintf(insert, sizeof(insert),
		"INSERT INTO bags (bag_id, floor_id, x, y, z, expires_at) "
		"VALUES (%llu, %u, %f, %f, %f, NOW() + INTERVAL %llu SECOND)",
		snap.bagId, snap.floorId, snap.x, snap.y, snap.z, BAG_LIFETIME_US / 1'000'000);

	if (conn->Excute(insert) == false)
		return RollbackAndClassify(conn);

	if (existingIds.empty() == false)
	{
		// 내 것 또는 주인 없는 것(가방 · 상자에서 주운 것 - DB 에선 아직 그 가방 소속)만 옮긴다. 남의 것은 두 단계 모두 건드리지 않는다.
		// 회귀: 예전엔 id 만 봤다 - DB 에서 다른 캐릭터 창고로 간 행도 이 가방으로 끌어왔다 (2026-10-08 실서버 시험에서 발견)
		const string mineOrUnowned = " AND (owner_character_id IS NULL OR owner_character_id = " + to_string(snap.characterId) + ")";

		const string update =
			"UPDATE item_instances SET owner_character_id = NULL, bag_id = " + to_string(snap.bagId)
			+ ", state = " + to_string(static_cast<int32>(Protocol::ITEM_STATE_IN_BAG))
			+ ", slot = 0, level = 0 WHERE instance_id IN (" + JoinIds(existingIds) + ")" + mineOrUnowned;

		if (conn->Excute(update.c_str()) == false)
			return RollbackAndClassify(conn);

		if (conn->GetAffectedRows() != existingIds.size())
		{
			// 엄격: DB 에 있어야 할 아이템(isNew 아님)은 전부 있어야 한다. 모자라면 어딘가 어긋났다 - 커밋하지 않는다 → 관대 단계로
			if (lenient == false)
			{
				LOG_LINE << "[DEATH] item count mismatch - expected=" << existingIds.size() << " affected=" << conn->GetAffectedRows() << " char=" << snap.characterId << '\n';
				conn->Rollback();
				return CommitOutcome::Permanent;
			}

			LOG_LINE << "[DEATH] 🔴 lenient - moved " << conn->GetAffectedRows() << " of " << existingIds.size() << " to bag " << snap.bagId
				<< " (the rest were gone or someone else's) char=" << snap.characterId << '\n';
		}
	}

	// 같은 트랜잭션 - 가방 행 · 소유 이전과 함께 확정되거나 함께 롤백된다 (AC-CH-07)
	if (newValues.empty() == false)
	{
		const string insertItems = InsertItemsSql(mode) + newValues;
		if (conn->Excute(insertItems.c_str()) == false)
			return RollbackAndClassify(conn);
	}

	//  이번 런에서 버린 아이템을 파기한다. 이미 없으면 목적은 달성된 것이라 개수가 달라도 실패로 보지않는다.
	if (snap.droppedIds.empty() == false)
	{
		const string del = "DELETE FROM item_instances WHERE instance_id IN (" + JoinIds(snap.droppedIds) + ")";
		if (conn->Excute(del.c_str()) == false)
			return RollbackAndClassify(conn);

		if (conn->GetAffectedRows() != snap.droppedIds.size())
			LOG_LINE << "[DEATH] dropped rows already gone - expected=" << snap.droppedIds.size() << " affected=" << conn->GetAffectedRows() << " char=" << snap.characterId << '\n';
	}

	// 관대 단계: 가방에 실제로 들어간 것만 메모리 가방에 담는다 - 안 그러면 DB 에 없는 아이템을 주워 갈 수 있다
	unordered_set<uint64> inBag;
	if (lenient)
	{
		const string select = "SELECT instance_id FROM item_instances WHERE bag_id = " + to_string(snap.bagId);
		MYSQL_RES* res = conn->Query(select.c_str());
		if (res == nullptr)
			return RollbackAndClassify(conn);

		while (MYSQL_ROW row = ::mysql_fetch_row(res))
			inBag.insert(::strtoull(row[0], nullptr, 10));
		conn->FreeResult(res);
	}

	// 검증 전용 - 커밋을 늦춰 재입장 가드(DeathCommitTracker)가 실제로 발동하는지 본다.
	if (const uint32 delayMs = GDeathCommitDelayMs.load(); delayMs > 0)
		this_thread::sleep_for(chrono::milliseconds(delayMs));

	// 검증 전용 - 쓰기를 전부 성공시킨 뒤에 실패시킨다(가방 INSERT · 소유 이전 · 런 중 생성 INSERT · 파기).
	// 롤백이 그것들을 모두 되돌리는지가 원자성 검증이다 (AC-CH-07). 데이터 어긋남과 같은 영구 실패로 다룬다 -
	// 엄격 → 관대 → 포기 로 끝나고, 결과는 예전과 같다(가방 없음 · 아이템은 주인에게)
	if (GFailDeathCommit.load())
	{
		LOG_LINE << "[DEATH] injected failure after all writes - bag=" << snap.bagId << '\n';
		conn->Rollback();
		return CommitOutcome::Permanent;
	}

	if (conn->Commit() == false)
		return RollbackAndClassify(conn);

	// 응답을 잃은 척 - 아래 관대 단계 걸러 내기도 같이 잃는다. 재시도의 KeepItemsActuallyInBag 이 DB 에서 다시 본다
	if (FakeAckLoss("DEATH", snap.characterId))
		return CommitOutcome::Transient;

	if (lenient)
	{
		const size_t before = snap.items.size();
		snap.items.erase(remove_if(snap.items.begin(), snap.items.end(),
			[&inBag](const ItemEntry& e) { return inBag.count(e.instanceId) == 0; }), snap.items.end());

		if (snap.items.size() != before)
			LOG_LINE << "[DEATH] 🔴 lenient - bag " << snap.bagId << " holds " << snap.items.size() << " of " << before << " char=" << snap.characterId << '\n';
	}

	return CommitOutcome::Ok;
}

namespace
{
	void SyncBaseCamp(DBConnection* conn, GameSessionRef session, uint64 characterId, int32 waitsLeft, uint64 firstUs);
}

void ResendBaseCampIfRelogged(DBConnection* conn, const GameSessionRef& original, uint64 characterId)
{
	if (original != nullptr && original->IsConnected())
		return;

	GameSessionRef relogged = GSessionManager.FindInBaseCamp(characterId);
	if (relogged == nullptr || relogged == original)
		return;

	LOG_LINE << "[BASECAMP] run-end result to the relogged session - char=" << characterId << '\n';
	SendBaseCampInventory(conn, relogged, characterId);
}

void SendBaseCampInventory(DBConnection* conn, GameSessionRef session, uint64 characterId)
{
	if (session == nullptr || characterId == 0)
		return;

	SyncBaseCamp(conn, session, characterId, BASECAMP_SYNC_WAIT_TRIES, Utils::NowMicroseconds());
}

namespace
{
void SyncBaseCamp(DBConnection* conn, GameSessionRef session, uint64 characterId, int32 waitsLeft, uint64 firstUs)
{
	// 기다리는 사이에 끊겼다 - 보낼 곳이 없다
	if (session->IsConnected() == false)
		return;

	// 사망 커밋이 도는 중이면 DB 는 아직 사망 전 상태다. 커밋은 보통 수 ms 라 잠깐 기다린다 - 자지 않고 다시 넣는다(PushAfter).
	// 예전엔 이 워커가 최대 10 초 잤다 - 저장이 DB 장애로 길어지면 재로그인마다 워커 하나가 10 초씩 빠졌다 (ADR-0007 R-3)
	if (GDeathCommits.IsPending(characterId) && waitsLeft > 0)
	{
		if (waitsLeft == BASECAMP_SYNC_WAIT_TRIES)
			LOG_LINE << "[BASECAMP] sync deferred - death commit pending. char=" << characterId << '\n';

		GDBQueue.PushAfter(BASECAMP_SYNC_WAIT_MS, [session, characterId, waitsLeft, firstUs](DBConnection* next)
			{
				SyncBaseCamp(next, session, characterId, waitsLeft - 1, firstUs);
			});
		return;
	}

	// 끝내 안 끝나면(실패 주입 · DB 장애) 지금 확정된 상태를 보낸다 - 커밋이 포기되면 그게 맞는 상태이고,
	// 커밋되면 OnBagCreated 가 다시 보낸다. 그동안 베이스캠프 조작과 입장은 GDeathCommits 검사가 막는다.
	if (GDeathCommits.IsPending(characterId))
		LOG_LINE << "[BASECAMP] sync while death commit pending - char=" << characterId << " (sending committed state)\n";

	char query[320];
	::snprintf(query, sizeof(query),
		"SELECT instance_id, item_type_id, grade, level, state, slot, skill_id_primary, skill_id_secondary "
		"FROM item_instances WHERE owner_character_id = %llu AND state IN (%d, %d, %d)",
		characterId,
		static_cast<int32>(Protocol::ITEM_STATE_VAULTED),
		static_cast<int32>(Protocol::ITEM_STATE_CARRIED),
		static_cast<int32>(Protocol::ITEM_STATE_EQUIPPED));

	MYSQL_RES* res = conn->Query(query);
	if (res == nullptr)
	{
		LOG_LINE << "[BASECAMP] sync query failed - char=" << characterId << " err=" << conn->GetError() << '\n';
		return;
	}

	Protocol::S_INVENTORY_SYNC pkt;
	int32 vaulted = 0;
	int32 carried = 0;
	int32 equipped = 0;

	while (MYSQL_ROW row = ::mysql_fetch_row(res))
	{
		ItemEntry e;
		e.instanceId = ::strtoull(row[0], nullptr, 10);
		e.itemTypeId = static_cast<uint32>(::atoi(row[1]));
		e.grade = static_cast<Protocol::ItemGrade>(::atoi(row[2]));
		e.level = static_cast<uint32>(::atoi(row[3]));
		e.skillIdPrimary = static_cast<uint32>(::atoi(row[6]));
		e.skillIdSecondary = static_cast<uint32>(::atoi(row[7]));

		const Protocol::ItemState state = static_cast<Protocol::ItemState>(::atoi(row[4]));
		const Protocol::EquipSlot slot = static_cast<Protocol::EquipSlot>(::atoi(row[5]));
		*pkt.add_items() = e.ToProto(state, slot);

		if (state == Protocol::ITEM_STATE_VAULTED)
			vaulted++;
		else if (state == Protocol::ITEM_STATE_CARRIED)
			carried++;
		else
			equipped++;
	}

	conn->FreeResult(res);

	SEND_PACKET_DECLARATION(pkt);
	session->Send(sendBuffer);

	// 사망 커밋을 기다렸으면 얼마나 - 0 이면 바로 보냈다
	const uint64 waitedMs = (waitsLeft < BASECAMP_SYNC_WAIT_TRIES) ? (Utils::NowMicroseconds() - firstUs) / 1000 : 0;
	LOG_LINE << "[BASECAMP] sync - char=" << characterId << " vaulted=" << vaulted << " carried=" << carried << " equipped=" << equipped
		<< " waited=" << waitedMs << "ms\n";
}
}

// 만료 커밋 한 번 (escape-timer.md Core Rule 17). 가방을 만들지 않는다 - bags 테이블은 건드리지 않는다 (AC-9).
// DELETE 는 원래 관대하다(어긋나도 실패로 보지 않는다) - 관대 단계는 골드 문장만 UPDATE IGNORE 로 바꾼다
CommitOutcome TryCommitExpiry(DBConnection* conn, const ExpirySnapshot& snap, CommitMode mode, const string& journalId)
{
	const bool lenient = (mode == CommitMode::Lenient);
	char query[256];

	if (conn->BeginTransaction() == false)
		return RollbackAndClassify(conn);

	// 적용 표시를 맨 먼저 (TryCommitEscape 와 같다) - 만료 골드도 두 번 들어가지 않게
	if (CommitOutcome marked; MarkOrFinish(conn, journalId, "EXPIRE", snap.characterId, OUT marked) == false)
		return marked;

	::snprintf(query, sizeof(query), "SELECT character_id FROM characters WHERE character_id = %llu FOR UPDATE", snap.characterId);
	MYSQL_RES* lockRes = conn->Query(query);
	if (lockRes == nullptr)
		return RollbackAndClassify(conn);
	conn->FreeResult(lockRes);

	// 소유자가 아니라 id 로 - 주운 사망 가방 아이템은 DB 에서 아직 그 가방 소속(IN_BAG)이다. 지우지 않으면 재시작 때
	// LoadLiveBags 가 가방을 되살려 복제가 된다. 창고(VAULTED)는 상태 조건으로 어떤 경우에도 못 지운다 (AC-ET-25)
	if (snap.rowIds.empty() == false)
	{
		const string del = "DELETE FROM item_instances WHERE instance_id IN (" + JoinIds(snap.rowIds) + ") AND state IN ("
			+ to_string(static_cast<int32>(Protocol::ITEM_STATE_CARRIED)) + ","
			+ to_string(static_cast<int32>(Protocol::ITEM_STATE_EQUIPPED)) + ","
			+ to_string(static_cast<int32>(Protocol::ITEM_STATE_IN_BAG)) + ")";

		if (conn->Excute(del.c_str()) == false)
			return RollbackAndClassify(conn);

		if (conn->GetAffectedRows() != snap.rowIds.size())
			LOG_LINE << "[EXPIRE] row mismatch - expected=" << snap.rowIds.size() << " affected=" << conn->GetAffectedRows() << " char=" << snap.characterId << '\n';
	}

	if (snap.goldKept > 0)
	{
		::snprintf(query, sizeof(query), "%s characters SET gold = gold + %llu WHERE character_id = %llu",
			lenient ? "UPDATE IGNORE" : "UPDATE", snap.goldKept, snap.characterId);
		if (conn->Excute(query) == false)
			return RollbackAndClassify(conn);
	}

	// 검증 전용 - 탈출 커밋 실패 주입과 같은 자리 (AC-ET-24 롤백). 일시적 실패를 흉내 낸다
	if (GFailEscapeCommits.fetch_sub(1) > 0)
	{
		LOG_LINE << "[EXPIRE] injected failure - char=" << snap.characterId << '\n';
		conn->Rollback();
		return CommitOutcome::Transient;
	}

	if (conn->Commit() == false)
		return RollbackAndClassify(conn);

	if (FakeAckLoss("EXPIRE", snap.characterId))
		return CommitOutcome::Transient;

	return CommitOutcome::Ok;
}

namespace
{
	// 저장 하나의 재시도 상태. 작업이 자기 자신을 PushAfter 로 다시 넣는다 - DB 스레드를 잡고 자지 않는다
	struct CommitTask
	{
		string			tag;					// DEATH · ESCAPE · EXPIRE - 로그 머리
		uint64			characterId = 0;
		uint64			queuedUs = 0;
		CommitMode		mode = CommitMode::Strict;
		uint32			transientRetries = 0;

		JournalEntry	journal;				// 첫 시도 때의 스냅샷 - 실패하면 파일로 (CommitJournal.h)
		string			journalId;				// 큐에 넣을 때 정한다 - 모든 시도가 이 id 로 applied_commits 표시 (2026-10-08 리뷰 ①)
		bool			journalWritten = false;	// 파일로 남겼나 - 첫 실패 때만 (사용자 결정 ①)

		function<CommitOutcome(DBConnection*, CommitMode, const string&)>	attempt;
		function<void(DBConnection*, bool, CommitMode)>						finish;
	};

	// 재시도로 들어가는 순간 한 번 - 평소 저장(첫 시도 성공)은 디스크를 건드리지 않는다 (사용자 결정 2026-10-08 ①)
	void WriteJournalOnce(const shared_ptr<CommitTask>& task)
	{
		if (task->journalWritten || GCommitJournalDir.empty())
			return;

		string error;
		if (CommitJournal::Write(GCommitJournalDir, task->journal, OUT error) == false)
		{
			// 기록 없이도 재시도는 계속한다 - 그동안 서버가 꺼지면 예전처럼 이 저장은 사라진다
			LOG_LINE << "[JOURNAL] 🔴 cannot write " << task->journal.id << " - " << error << '\n';
			return;
		}

		task->journalWritten = true;
		LOG_LINE << "[JOURNAL] wrote " << task->journalId << " (a restart will finish this commit)\n";
	}

	void RunCommit(const shared_ptr<CommitTask>& task, DBConnection* conn)
	{
		t_commitErrno = 0;
		const CommitOutcome outcome = task->attempt(conn, task->mode, task->journalId);
		const uint32 err = t_commitErrno;		// 실패한 문장의 번호 (0 = DB 오류가 아닌 실패 - 데이터 어긋남 · 시험 주입)
		const CommitStep step = CommitRetry::Next(task->mode, outcome, task->transientRetries);
		const uint64 elapsedMs = (Utils::NowMicroseconds() - task->queuedUs) / 1000;
		const char* modeName = (task->mode == CommitMode::Lenient) ? "lenient" : "strict";

		switch (step.kind)
		{
		case CommitStep::Kind::Done:
			if (task->transientRetries > 0 || task->mode == CommitMode::Lenient)
				LOG_LINE << "[" << task->tag << "] committed after " << task->transientRetries << " transient retries · mode=" << modeName
					<< " · " << elapsedMs << "ms char=" << task->characterId << '\n';

			// 파일을 먼저 지우고 표시를 지운다 - 지우기 전에 꺼져도 표시가 있어 부팅이 건너뛴다.
			// 파일을 못 지웠으면(백신 · 색인기가 쥐고 있는 등) 표시를 남긴다 - 다음 부팅이 "이미 적용" 으로 파일만 치운다 (리뷰 ②)
			if (task->journalWritten == false || CommitJournal::Remove(GCommitJournalDir, task->journalId))
				ClearAppliedMark(conn, task->journalId);
			else
				LOG_LINE << "[JOURNAL] 🔴 cannot remove " << task->journalId << " - keeping the applied mark (the next boot only removes the file)\n";

			task->finish(conn, true, task->mode);
			return;

		case CommitStep::Kind::Abandon:
			// 관대 단계에서도 같은 문장으로는 안 된다 - 데이터가 깨졌다. DB 는 저장 전 그대로 (예전 사망 3회 실패와 같은 결과)
			LOG_LINE << "[" << task->tag << "] 🔴 ABANDONED - permanent failure even in lenient mode, DB unchanged. err=" << err
				<< " char=" << task->characterId << " after " << elapsedMs << "ms\n";

			// 다시 해도 같다 - 부팅이 또 붙잡지 않게 치우되, 원인을 볼 수 있게 남긴다. 표시는 없다(이 시도는 롤백됐다)
			if (task->journalWritten)
				CommitJournal::MoveToAbandoned(GCommitJournalDir, task->journalId);

			task->finish(conn, false, task->mode);
			return;

		case CommitStep::Kind::Retry:
		default:
			break;
		}

		if (step.mode != task->mode)
		{
			LOG_LINE << "[" << task->tag << "] permanent failure in strict mode (err=" << err << ") - retrying leniently char=" << task->characterId << '\n';
			task->mode = step.mode;
		}
		else
		{
			task->transientRetries++;
			const bool stuck = (task->transientRetries % CommitRetry::WARN_EVERY) == 0;
			LOG_LINE << "[" << task->tag << "] " << (stuck ? "🔴 STILL FAILING - " : "") << "attempt " << task->transientRetries
				<< " failed err=" << err << " (" << modeName << ") - retry in " << step.delayMs << "ms · waiting " << elapsedMs
				<< "ms char=" << task->characterId << '\n';
		}

		// 기다리게 됐다 - 그동안 서버가 꺼져도 다음 부팅이 끝내도록 디스크에 남긴다
		WriteJournalOnce(task);

		// 끊긴 연결로 다시 해 봐야 같다 - 이 워커의 커넥션을 지금 다시 붙인다 (다음 시도는 다른 워커일 수 있다 - 그 워커도 제 것을 고친다)
		if (DBError::IsConnectionLost(err) || conn->IsConnected() == false)
			conn->Reconnect();

		GDBQueue.PushAfter(step.delayMs, [task](DBConnection* next) { RunCommit(task, next); });
	}

	CommitOutcome AttemptJournal(DBConnection* conn, JournalEntry& entry, CommitMode mode)
	{
		switch (entry.kind)
		{
		case JournalKind::Death:	return TryCommitDeath(conn, entry.death, mode, entry.id);
		case JournalKind::Escape:	return TryCommitEscape(conn, entry.escape, mode, entry.id);
		case JournalKind::Expiry:	return TryCommitExpiry(conn, entry.expiry, mode, entry.id);
		default:					return CommitOutcome::Permanent;
		}
	}

	// 부팅 재처리의 일시 실패 한도 - 100 ms → ×2 → 5 s 로 10 번이면 약 25 s. 그래도 안 되면 서버를 띄우지 않는다
	constexpr uint32 BOOT_TRANSIENT_LIMIT = 10;
}

void QueueCommit(JournalEntry journal,
	function<CommitOutcome(DBConnection*, CommitMode, const string& journalId)> attempt,
	function<void(DBConnection*, bool committed, CommitMode mode)> finish)
{
	auto task = make_shared<CommitTask>();
	task->tag = CommitJournal::KindName(journal.kind);
	task->characterId = journal.characterId;
	task->queuedUs = Utils::NowMicroseconds();
	task->journal = move(journal);
	task->journal.id = CommitJournal::NewId(task->journal.kind, task->characterId);
	task->journalId = task->journal.id;
	task->attempt = move(attempt);
	task->finish = move(finish);

	GDBQueue.Push([task](DBConnection* conn) { RunCommit(task, conn); });
}

bool EnsureCommitJournalTable(DBConnection* conn)
{
	// Server/Tools/schema_commit_journal.sql 과 같은 정의 - 표가 있으면 아무 일도 하지 않는다
	if (conn->Excute(
		"CREATE TABLE IF NOT EXISTS applied_commits ("
		" journal_id   VARCHAR(64)     NOT NULL,"
		" character_id BIGINT UNSIGNED NOT NULL,"
		" kind         VARCHAR(16)     NOT NULL,"
		" applied_at   DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,"
		" PRIMARY KEY (journal_id)"
		") ENGINE = InnoDB DEFAULT CHARSET = utf8mb4") == false)
	{
		LOG_LINE << "[BOOT] cannot create applied_commits: " << conn->GetError() << '\n';
		return false;
	}

	return true;
}

bool ReplayCommitJournal(DBConnection* conn, const string& dir)
{
	if (dir.empty())
		return true;

	const vector<string> ids = CommitJournal::ListIds(dir);

	// 읽을 수 없는 기록은 적용할 수 없다 - 일부만 적용하면 잘못된 결과가 확정된다. 원인을 볼 수 있게 abandoned/ 로
	vector<JournalEntry> entries;
	uint64 maxInstance = 0;
	uint64 maxBag = 0;
	for (const string& id : ids)
	{
		JournalEntry entry;
		string error;
		if (CommitJournal::Read(dir, id, OUT entry, OUT error) == false || entry.id != id)
		{
			LOG_LINE << "[JOURNAL] 🔴 unreadable " << id << " - " << (error.empty() ? "id mismatch" : error) << " → abandoned/\n";
			CommitJournal::MoveToAbandoned(dir, id);
			continue;
		}

		CommitJournal::MaxIds(entry, OUT maxInstance, OUT maxBag);
		entries.push_back(move(entry));
	}

	// 런 중 생성 아이템 · 가방 id 는 DB 에 없어 RestoreIdCounters 가 모른다 - 재처리 · 이후 발급이 기록 속 id 와 겹치지 않게 그 위로
	if (maxInstance >= GNextInstanceId.load())
		GNextInstanceId.store(maxInstance + 1);
	if (maxBag >= GNextBagId.load())
		GNextBagId.store(maxBag + 1);

	int32 applied = 0;
	int32 skipped = 0;
	int32 abandoned = 0;

	// 파일을 못 지운 기록 - 표시를 남긴다. 표시까지 지우면 다음 부팅이 그 파일을 또 적용한다 (2026-10-08 리뷰 ②)
	vector<string> keepMarks;
	auto removeOrKeep = [&dir, &keepMarks](const string& id)
		{
			if (CommitJournal::Remove(dir, id))
				return;
			LOG_LINE << "[JOURNAL] 🔴 cannot remove " << id << " - keeping its applied mark so the next boot skips it\n";
			keepMarks.push_back(id);
		};

	for (JournalEntry& entry : entries)
	{
		// 커밋은 됐는데 파일을 지우기 전에 꺼졌다 - 두 번 적용하지 않는다
		uint64 marked = 0;
		const string check = "SELECT COUNT(*) FROM applied_commits WHERE journal_id = '" + conn->Escape(entry.id) + "'";
		if (QueryScalar(conn, check.c_str(), OUT marked) == false)
			return false;

		if (marked > 0)
		{
			LOG_LINE << "[JOURNAL] " << entry.id << " already applied - removing the record\n";
			removeOrKeep(entry.id);
			skipped++;
			continue;
		}

		CommitMode mode = CommitMode::Strict;
		uint32 transient = 0;

		while (true)
		{
			t_commitErrno = 0;
			const CommitOutcome outcome = AttemptJournal(conn, entry, mode);
			const uint32 err = t_commitErrno;
			const CommitStep step = CommitRetry::Next(mode, outcome, transient);

			if (step.kind == CommitStep::Kind::Done)
			{
				LOG_LINE << "[JOURNAL] replayed " << entry.id << " (" << CommitJournal::KindName(entry.kind) << " char=" << entry.characterId
					<< " mode=" << (mode == CommitMode::Lenient ? "lenient" : "strict") << ")\n";
				removeOrKeep(entry.id);
				applied++;
				break;
			}

			if (step.kind == CommitStep::Kind::Abandon)
			{
				LOG_LINE << "[JOURNAL] 🔴 ABANDONED " << entry.id << " - permanent failure even in lenient mode, DB unchanged. err=" << err << " → abandoned/\n";
				CommitJournal::MoveToAbandoned(dir, entry.id);
				abandoned++;
				break;
			}

			if (step.mode != mode)
			{
				LOG_LINE << "[JOURNAL] " << entry.id << " permanent failure in strict mode (err=" << err << ") - retrying leniently\n";
				mode = step.mode;
				continue;
			}

			if (++transient > BOOT_TRANSIENT_LIMIT)
			{
				LOG_LINE << "[JOURNAL] 🔴 " << entry.id << " still failing after " << BOOT_TRANSIENT_LIMIT << " retries (err=" << err
					<< ") - refusing to start until it can be applied\n";
				return false;
			}

			LOG_LINE << "[JOURNAL] " << entry.id << " attempt " << transient << " failed err=" << err << " - retry in " << step.delayMs << "ms\n";
			if (DBError::IsConnectionLost(err) || conn->IsConnected() == false)
				conn->Reconnect();
			this_thread::sleep_for(chrono::milliseconds(step.delayMs));
		}
	}

	// 기록이 모두 정리됐다 - 표시는 진행 중인 저장 · 파일이 남은 저장을 위해서만 있다. 런 중에 지우지 못한 것까지 비우되,
	// 파일을 못 지운 기록의 표시는 남긴다
	string clear = "DELETE FROM applied_commits";
	if (keepMarks.empty() == false)
	{
		clear += " WHERE journal_id NOT IN (";
		for (size_t i = 0; i < keepMarks.size(); i++)
			clear += (i > 0 ? ",'" : "'") + conn->Escape(keepMarks[i]) + "'";
		clear += ")";
	}

	if (conn->Excute(clear.c_str()) == false)
	{
		LOG_LINE << "[BOOT] cannot clear applied_commits: " << conn->GetError() << '\n';
		return false;
	}

	LOG_LINE << "[JOURNAL] boot replay - records=" << ids.size() << " applied=" << applied << " already=" << skipped
		<< " abandoned=" << abandoned << " unreadable=" << (ids.size() - entries.size()) << " kept_marks=" << keepMarks.size() << " dir=" << dir << '\n';
	return true;
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

/*-------------
	RegearBot
--------------*/

int32 RegearBot(Player& player, const string& characterName)
{
	if (characterName.rfind("bot_", 0) != 0)
		return 0;

	// 기본 장비 표(starter_kit.csv)와 같다 - reset_bots.sql 이 처음에 입히는 3점이기도 해서 재지급 전후로 봇의 전투가 달라지지 않는다
	return EquipStarterKit(player);
}

int32 EquipStarterKit(Player& player)
{
	return StarterKitTable::EquipInto(player.equipped, SKILL_SLOT_COUNT, [] { return GNextInstanceId.fetch_add(1); });
}
