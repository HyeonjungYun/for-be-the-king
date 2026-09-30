#include "pch.h"
#include "ServerPacketHandler.h"
#include "BufferReader.h"
#include "BufferWriter.h"
#include "ObjectUtils.h"
#include "Room.h"
#include "Player.h"
#include "AccountManager.h"
#include <thread>

PacketHandlerFunc GPacketHandler[UINT16_MAX];

namespace
{
	// 지급 결과를 세션에 돌려준다. DB 스레드에서 호출
	void SendGrantResult(GameSessionRef session, const string& requestId, Protocol::GrantResult result, uint64 goldAfter)
	{
		Protocol::S_GRANT_REWARD pkt;
		pkt.set_request_id(requestId);
		pkt.set_result(result);
		pkt.set_gold_after(goldAfter);

		SEND_PACKET_DECLARATION(pkt);
		session->Send(sendBuffer);
	}

	// 캐릭터의 아이템을 전부 읽어 Player에 싣는다.
	// 이후 던전 내에서는 DB를 치지 않는다.
	bool LoadPlayerItems(DBConnection* conn, PlayerRef player)
	{
		const uint64 characterId = player->objectInfo->object_id();

		char query[512];

		if (conn->BeginTransaction() == false)
		{
			LOG_LINE << "[ENTER] begin failed: " << conn->GetError() << '\n';
			return false;
		}

		::snprintf(query, sizeof(query),
			"SELECT character_id FROM characters WHERE character_id = %llu FOR UPDATE", characterId);

		MYSQL_RES* lockRes = conn->Query(query);
		if (lockRes == nullptr)
		{
			LOG_LINE << "[ENTER] lock failed: " << conn->GetError() << '\n';
			conn->Rollback();
			return false;
		}
		conn->FreeResult(lockRes);

		::snprintf(query, sizeof(query),
			"SELECT instance_id, item_type_id, grade, level, state, slot, "
			"skill_id_primary, skill_id_secondary FROM item_instances "
			"WHERE owner_character_id = %llu",
			characterId);

		MYSQL_RES* res = conn->Query(query);
		if (res == nullptr)
		{
			LOG_LINE << "[ENTER] item query failed: " << conn->GetError() << '\n';
			conn->Rollback();
			return false;
		}

		enum { COL_ID = 0, COL_TYPE, COL_GRADE, COL_LEVEL, COL_STATE, COL_SLOT, COL_SKILL1, COL_SKILL2, COL_COUNT };

		if (::mysql_num_fields(res) < COL_COUNT)
		{
			LOG_LINE << "[ENTER] item column mismatch" << '\n';
			conn->FreeResult(res);
			conn->Rollback();
			return false;
		}

		while (MYSQL_ROW row = ::mysql_fetch_row(res))
		{
			ItemEntry e;
			e.instanceId = ::strtoull(row[COL_ID], nullptr, 10);
			e.itemTypeId = static_cast<uint32>(::atoi(row[COL_TYPE]));
			e.grade = static_cast<Protocol::ItemGrade>(::atoi(row[COL_GRADE]));
			e.level = static_cast<uint32>(::atoi(row[COL_LEVEL]));
			e.skillIdPrimary = static_cast<uint32>(::atoi(row[COL_SKILL1]));
			e.skillIdSecondary = static_cast<uint32>(::atoi(row[COL_SKILL2]));

			const int32 state = ::atoi(row[COL_STATE]);
			const int32 slot = ::atoi(row[COL_SLOT]);

			if (state == Protocol::ITEM_STATE_EQUIPPED && slot >= Protocol::SLOT_WEAPON_PRIMARY && slot <= Protocol::SLOT_TRINKET)
			{
				player->equipped[slot - 1] = e;
			}
			else if (state == Protocol::ITEM_STATE_CARRIED)
			{
				if (player->inventory.size() >= Player::INVENTORY_SLOT_COUNT)
					LOG_LINE << "[ENTER] inventory overflow - char=" << player->objectInfo->object_id() << '\n';

				player->inventory.push_back(e);
			}
		}

		conn->FreeResult(res);
		conn->Commit();
		return true;
	}

	enum class BaseCampOp : uint8 { ToVault, ToCarried, Destroy, Equip, Unequip };

	const int32 VAULT_SLOT_COUNT = 25;

	const char* BaseCampOpName(BaseCampOp op)
	{
		switch (op)
		{
		case BaseCampOp::ToVault: return "vault";
		case BaseCampOp::ToCarried: return "unvault";
		case BaseCampOp::Equip: return "equip";
		case BaseCampOp::Unequip: return "unequip";
		default: return "destroy";
		}
	}

	// 아이템 한 행을 잠그며 읽는다. 조건에 맞는 행이 없으면 found = false. 쿼리가 실패하면 false
	bool LockItemRow(DBConnection* conn, const string& where, OUT ItemEntry& e, OUT int32& state, OUT int32& slot, OUT bool& found)
	{
		const string query =
			"SELECT instance_id, item_type_id, grade, level, state, slot, skill_id_primary, skill_id_secondary "
			"FROM item_instances WHERE " + where + " FOR UPDATE";

		MYSQL_RES* res = conn->Query(query.c_str());
		if (res == nullptr)
			return false;

		found = false;
		if (MYSQL_ROW row = ::mysql_fetch_row(res))
		{
			e.instanceId = ::strtoull(row[0], nullptr, 10);
			e.itemTypeId = static_cast<uint32>(::atoi(row[1]));
			e.grade = static_cast<Protocol::ItemGrade>(::atoi(row[2]));
			e.level = static_cast<uint32>(::atoi(row[3]));
			state = ::atoi(row[4]);
			slot = ::atoi(row[5]);
			e.skillIdPrimary = static_cast<uint32>(::atoi(row[6]));
			e.skillIdSecondary = static_cast<uint32>(::atoi(row[7]));
			found = true;
		}
		conn->FreeResult(res);
		return true;
	}

	// 캐릭터가 가진 state 아이템 개수 - 용량 검사용. 쿼리가 실패하면 false
	bool CountItems(DBConnection* conn, uint64 ownerId, Protocol::ItemState state, OUT uint64& count)
	{
		char query[256];
		::snprintf(query, sizeof(query),
			"SELECT COUNT(*) FROM item_instances WHERE owner_character_id = %llu AND state = %d",
			ownerId, static_cast<int32>(state));

		MYSQL_RES* res = conn->Query(query);
		if (res == nullptr)
			return false;

		count = 0;
		if (MYSQL_ROW row = ::mysql_fetch_row(res))
			count = ::strtoull(row[0], nullptr, 10);
		conn->FreeResult(res);
		return true;
	}

	Protocol::ItemResult RunBaseCampItemOp(DBConnection* conn, GameSessionRef session, uint64 instanceId, BaseCampOp op, Protocol::EquipSlot equipSlot, OUT vector<Protocol::ItemInstance>& changed)
	{
		char query[512];

		// 주인 - 클라는 instance_id만 보낸다. 주인은 서버가 자기 데이터에서 찾는다.
		uint64 ownerId = 0;
		::snprintf(query, sizeof(query),
			"SELECT owner_character_id FROM item_instances WHERE instance_id = %llu", instanceId);

		MYSQL_RES* ownerRes = conn->Query(query);
		if (ownerRes == nullptr)
			return Protocol::ITEM_DB_ERROR;

		if (MYSQL_ROW row = ::mysql_fetch_row(ownerRes))
			ownerId = (row[0] != nullptr) ? ::strtoull(row[0], nullptr, 10) : 0;
		conn->FreeResult(ownerRes);

		// 남의 아이템, 가방 속 아이템은 없는 것과 같이 답한다 - 존재 여부를 흘리지 않는다.
		bool mine = false;
		for (const Protocol::CharacterInfo& c : session->characters)
			if (ownerId != 0 && c.object_info().object_id() == ownerId)
				mine = true;

		if (mine == false)
			return Protocol::ITEM_NOT_FOUND;

		if (conn->BeginTransaction() == false)
			return Protocol::ITEM_DB_ERROR;

		// 캐릭터 잠금
		::snprintf(query, sizeof(query),
			"SELECT character_id FROM characters WHERE character_id = %llu FOR UPDATE", ownerId);

		MYSQL_RES* lockRes = conn->Query(query);
		if (lockRes == nullptr)
		{
			conn->Rollback();
			return Protocol::ITEM_DB_ERROR;
		}
		conn->FreeResult(lockRes);

		// 잠근 뒤에 다시 본다 - 그 사이 입장을 시작했거나(Entering) 사망 커밋이 진행 중이면 손대지 않는다.
		if (session->place.load() != SessionPlace::BaseCamp || GDeathCommits.IsPending(ownerId))
		{
			conn->Rollback();
			return Protocol::ITEM_INVALID_STATE;
		}

		// 검증 전용 - 잠금은 쥔 채 늦춰, 입장 적재가 이 커밋을 기다리는지 본다.
		const uint32 delayMs = GBaseCampDelayMs.load();
		if (delayMs > 0)
			this_thread::sleep_for(chrono::milliseconds(delayMs));

		// 아이템 현재 상태 (행 잠금)
		ItemEntry e;
		int32 state = -1;
		int32 slot = 0;
		bool found = false;

		if (LockItemRow(conn, "instance_id = " + to_string(instanceId) + " AND owner_character_id = " + to_string(ownerId), OUT e, OUT state, OUT slot, OUT found) == false)
		{
			conn->Rollback();
			return Protocol::ITEM_DB_ERROR;
		}

		// 주인 확인과 잠금 사이에 주인이 바뀌었다.
		if (found == false)
		{
			conn->Rollback();
			return Protocol::ITEM_NOT_FOUND;
		}

		// 규칙 - 이 아이템의 목표 상태, 슬롯을 정하고, 착용 교체면 밀려나는 아이템을 찾는다.
		Protocol::ItemResult result = Protocol::ITEM_OK;
		Protocol::ItemState target = Protocol::ITEM_STATE_NONE;
		int32 targetSlot = 0;
		ItemEntry outgoing;			// 착용 교체로 소지품이 되는 아이템. 없으면 instanceId 0
		
		switch (op)
		{
		case BaseCampOp::Destroy:
			// 착용품은 해제부터. Carried, Vaulted만 곧바로 파기
			if (state != Protocol::ITEM_STATE_CARRIED && state != Protocol::ITEM_STATE_VAULTED)
				result = Protocol::ITEM_INVALID_STATE;
			break;
		case BaseCampOp::ToVault:
		case BaseCampOp::ToCarried:
		{
			const Protocol::ItemState from = (op == BaseCampOp::ToVault) ? Protocol::ITEM_STATE_CARRIED : Protocol::ITEM_STATE_VAULTED;
			target = (op == BaseCampOp::ToVault) ? Protocol::ITEM_STATE_VAULTED : Protocol::ITEM_STATE_CARRIED;
			const uint64 capacity = (target == Protocol::ITEM_STATE_VAULTED) ? VAULT_SLOT_COUNT : Player::INVENTORY_SLOT_COUNT;

			uint64 count = 0;
			if (state == target)
				result = Protocol::ITEM_ALREADY;		// 재접속 뒤 재전송
			else if (state != from)
				result = Protocol::ITEM_INVALID_STATE;	// 착용품은 해제부터
			else if (CountItems(conn, ownerId, target, OUT count) == false)
				result = Protocol::ITEM_DB_ERROR;
			else if (count >= capacity)
				result = Protocol::ITEM_INVENTORY_FULL;		// 창고 25, 소지 25
			break;
		}

		case BaseCampOp::Equip:
		{
			target = Protocol::ITEM_STATE_EQUIPPED;
			targetSlot = equipSlot;
			
			if (state == Protocol::ITEM_STATE_EQUIPPED && slot == targetSlot)
			{
				result = Protocol::ITEM_ALREADY;
				break;
			}

			// 착용 대상은 소지품뿐이다. - 창고품은 먼저 반입, 다른 슬롯 착용품은 먼저 해제
			if (state != Protocol::ITEM_STATE_CARRIED)
			{
				result = Protocol::ITEM_INVALID_STATE;
				break;
			}

			// 슬롯에 이미 있으면 자동 교체 - 밀려난 것은 이 아이템이 비운 소지 칸으로 간다.
			// 소지 개수가 그대로라 25/25에서도 성공한다.
			int32 outState = 0;
			int32 outSlot = 0;
			bool occupied = false;
			if (LockItemRow(conn,
				"owner_character_id = " + to_string(ownerId)
				+ " AND state = " + to_string(static_cast<int32>(Protocol::ITEM_STATE_EQUIPPED))
				+ " AND slot = " + to_string(targetSlot),
				OUT outgoing, OUT outState, OUT outSlot, OUT occupied) == false)
			{
				result = Protocol::ITEM_DB_ERROR;
			}
			break;
		}

		case BaseCampOp::Unequip:
		{
			target = Protocol::ITEM_STATE_CARRIED;
			
			uint64 count = 0;
			if (state == Protocol::ITEM_STATE_CARRIED)
				result = Protocol::ITEM_ALREADY;
			else if (state != Protocol::ITEM_STATE_EQUIPPED)
				result = Protocol::ITEM_INVALID_STATE;
			else if (CountItems(conn, ownerId, Protocol::ITEM_STATE_CARRIED, OUT count) == false)
				result = Protocol::ITEM_DB_ERROR;
			else if (count >= static_cast<uint64>(Player::INVENTORY_SLOT_COUNT))
				result = Protocol::ITEM_INVENTORY_FULL;		
			break;
		}
		}

		if (result != Protocol::ITEM_OK)
		{
			conn->Rollback();
			return result;
		}

		bool ok = true;

		if (outgoing.instanceId != 0)
		{
			::snprintf(query, sizeof(query), "UPDATE item_instances SET state = %d, slot = 0 WHERE instance_id = %llu",
				static_cast<int32>(Protocol::ITEM_STATE_CARRIED), outgoing.instanceId);
			ok = conn->Excute(query) && conn->GetAffectedRows() == 1;
		}

		if (ok)
		{
			if (op == BaseCampOp::Destroy)
				::snprintf(query, sizeof(query), "DELETE FROM item_instances WHERE instance_id = %llu", instanceId);
			else
				::snprintf(query, sizeof(query), "UPDATE item_instances SET state = %d, slot = %d WHERE instance_id = %llu",
					static_cast<int32>(target), targetSlot, instanceId);
			
			ok = conn->Excute(query) && conn->GetAffectedRows() == 1;
		}

		if (ok == false || conn->Commit() == false)
		{
			conn->Rollback();
			return Protocol::ITEM_DB_ERROR;
		}

		changed.push_back(e.ToProto(target, static_cast<Protocol::EquipSlot>(targetSlot)));
		if (outgoing.instanceId != 0)
			changed.push_back(outgoing.ToProto(Protocol::ITEM_STATE_CARRIED));
		return Protocol::ITEM_OK;

	}

	void PushBaseCampItemOp(GameSessionRef session, string requestId, uint64 instanceId, BaseCampOp op, Protocol::EquipSlot slot = Protocol::SLOT_NONE)
	{
		GDBQueue.Push([session, requestId, instanceId, op, slot](DBConnection* conn)
			{
				vector<Protocol::ItemInstance> changed;
				const Protocol::ItemResult result = RunBaseCampItemOp(conn, session, instanceId, op, slot, OUT changed);

				LOG_LINE << "[BASECAMP] " << BaseCampOpName(op) << " item=" << instanceId << " slot=" << slot << " result=" << result << " changed=" << changed.size() <<'\n';

				session->SendItemResult(requestId, result, changed);
			});
	}
}

bool Handle_INVALID(PacketSessionRef& session, BYTE* buffer, int32 len)
{
	PacketHeader* header = reinterpret_cast<PacketHeader*>(buffer);
	// TODO : Log
	return false;
}

bool Handle_C_LOGIN(PacketSessionRef& session, Protocol::C_LOGIN& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);

	const string token = pkt.token();
	if (token.empty())
	{
		Protocol::S_LOGIN loginPkt;
		loginPkt.set_success(false);

		SEND_PACKET_DECLARATION(loginPkt);
		gameSession->Send(sendBuffer);

		return true;
	}

	GDBQueue.Push([gameSession, token](DBConnection* conn)
		{
			Protocol::S_LOGIN loginPkt;
			loginPkt.set_success(false);

			const string safeToken = conn->Escape(token);

			char query[256];
			::snprintf(query, sizeof(query),
				"SELECT account_id FROM login_sessions "
				"WHERE token = '%s' AND expires_at > NOW()",
				safeToken.c_str());

			uint64 accountId = 0;

			if (MYSQL_RES* result = conn->Query(query))
			{
				if (MYSQL_ROW row = ::mysql_fetch_row(result))
					accountId = ::strtoull(row[0], nullptr, 10);

				conn->FreeResult(result);
			}
			if (accountId != 0 && GAccountManager.TryLogin(accountId) == false)
			{
				LOG_LINE << "[LOGIN] already online - account=" << accountId << '\n';
				accountId = 0;
			}

			if (accountId != 0)
			{
				::snprintf(query, sizeof(query),
					"SELECT character_id, name, hp, max_hp, floor_id, "
					"pos_x, pos_y, pos_z, yaw FROM characters "
					"WHERE account_id = %llu ORDER BY character_id",
					accountId);

				if (MYSQL_RES* result = conn->Query(query))
				{
					enum {COL_ID = 0, COL_NAME, COL_HP, COL_MAX_HP, COL_FLOOR, COL_X, COL_Y, COL_Z, COL_YAW, COL_COUNT };

					if (::mysql_num_fields(result) < COL_COUNT)
					{
						LOG_LINE << "[LOGIN] column count mismatch - expected " << COL_COUNT << " got " << ::mysql_num_fields(result) << '\n';
						conn->FreeResult(result);
						accountId = 0;
					}
					else
					{
						while (MYSQL_ROW row = ::mysql_fetch_row(result))
						{
							Protocol::CharacterInfo* info = loginPkt.add_characters();
							info->set_name(row[1] ? row[1] : "");
							info->set_hp(::atoi(row[2]));
							info->set_max_hp(::atoi(row[3]));
							info->set_floor_id(static_cast<uint32>(::atoi(row[4])));

							Protocol::ObjectInfo* obj = info->mutable_object_info();
							obj->set_object_id(::strtoull(row[0], nullptr, 10));
							obj->set_object_type(Protocol::OBJECT_TYPE_CREATURE);

							Protocol::PosInfo* pos = obj->mutable_pos_info();
							pos->set_x(static_cast<float>(::atof(row[5])));
							pos->set_y(static_cast<float>(::atof(row[6])));
							pos->set_z(static_cast<float>(::atof(row[7])));
							pos->set_yaw(static_cast<float>(::atof(row[8])));
						}

						conn->FreeResult(result);
					}
				}

				gameSession->characters.assign(loginPkt.characters().begin(), loginPkt.characters().end());
				gameSession->accountId.store(accountId);
				gameSession->place.store(SessionPlace::BaseCamp);

				loginPkt.set_success(true);
			}

			SEND_PACKET_DECLARATION(loginPkt);
			gameSession->Send(sendBuffer);
		});

	return true;
}

bool Handle_C_ENTER_GAME(PacketSessionRef& session, Protocol::C_ENTER_GAME& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);

	if (gameSession->accountId.load() == 0)
		return false;

	const uint64 index = pkt.playerindex();
	if (index >= gameSession->characters.size())
		return false;

	const Protocol::CharacterInfo& character = gameSession->characters[index];

	// 사망 커밋이 끝나기 전에는 재입장 시키지 않는다. (복제 방지)
	if (GDeathCommits.IsPending(character.object_info().object_id()))
	{
		LOG_LINE << "[ENTER] death commit pending - char=" << character.object_info().object_id() << '\n';

		Protocol::S_ENTER_GAME rejectPkt;
		rejectPkt.set_success(false);
		SEND_PACKET_DECLARATION(rejectPkt);
		gameSession->Send(sendBuffer);
		
		return true;
	}

	RoomRef room = GetRoomForFloor(pkt.floor_id());
	if (room == nullptr)
		return false;

	SessionPlace expected = SessionPlace::BaseCamp;
	gameSession->place.compare_exchange_strong(expected, SessionPlace::Entering);

	// 플레이어 생성
	PlayerRef player = ObjectUtils::CreatePlayer(gameSession, character.object_info().object_id());

	player->maxHp = character.max_hp();
	player->hp = character.hp();

	const Protocol::PosInfo& saved = character.object_info().pos_info();
	player->posInfo->set_x(saved.x());
	player->posInfo->set_y(saved.y());
	player->posInfo->set_z(saved.z());
	player->posInfo->set_yaw(saved.yaw());

	// TEST용 임시 규칙 : 죽은 채로 저장된 캐릭터를 입장 때 되살린다.
	if (player->hp <= 0)
	{
		if (player->maxHp <= 0)
			player->maxHp = BaseStats::MAX_HP;

		player->hp = player->maxHp;

		player->posInfo->set_x(Utils::GetRandom(-2000.f, 2000.f));
		player->posInfo->set_y(Utils::GetRandom(-2000.f, 2000.f));
		player->posInfo->set_z(100.f);

		LOG_LINE << "[ENTER] revive char=" << character.object_info().object_id()
			<< " hp=" << player->hp << '\n';
	}

	GDBQueue.Push([gameSession, player, room](DBConnection* conn)
		{
			if (LoadPlayerItems(conn, player) == false)
			{
				LOG_LINE << "[ENTER] item load failed - char=" << player->objectInfo->object_id() << '\n';

				gameSession->Disconnect(L"item load failed");
				return;
			}

			// 방에 입장
			room->DoAsync(&Room::HandleEnterPlayer, player);
		});

	return true;
}

bool Handle_C_LEAVE_GAME(PacketSessionRef& session, Protocol::C_LEAVE_GAME& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);

	PlayerRef player = gameSession->player.load();
	if (player == nullptr)
		return false;

	RoomRef room = player->room.load().lock();
	if (room == nullptr)
		return false;

	room->DoAsync(&Room::HandleLeavePlayer, player);

	return true;
}

bool Handle_C_MOVE(PacketSessionRef& session, Protocol::C_MOVE& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);

	PlayerRef player = gameSession->player.load();
	if (player == nullptr)
		return false;

	RoomRef room = player->room.load().lock();
	if (room == nullptr)
		return false;

	const uint64 realId = player->objectInfo->object_id();
	if (pkt.info().object_id() != realId)
	{
		// 클라가 S_ENTER_GAME 을 처리했다면 0 이 아닌 제 id 를 보낸다.
		LOG_LINE << "[ID MISMATCH] claimed=" << pkt.info().object_id()
			<< " actual=" << realId << '\n';
	}
	pkt.mutable_info()->set_object_id(realId);

	room->DoAsync(&Room::HandleMove, pkt);

	return true;
}

bool Handle_C_ATTACK(PacketSessionRef& session, Protocol::C_ATTACK& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);

	PlayerRef player = gameSession->player.load();
	if (player == nullptr)
		return false;

	RoomRef room = player->room.load().lock();
	if (room == nullptr)
		return false;

	// 공격자 id는 패킷에서 받지 않는다. 세션에서 가져온다.
	const uint64 attackerId = player->objectInfo->object_id();

	room->DoAsync(&Room::HandleAttack, attackerId, pkt.target_id());

	return true;
}

bool Handle_C_CHAT(PacketSessionRef& session, Protocol::C_CHAT& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);

	PlayerRef player = gameSession->player.load();
	if (player == nullptr)
		return false;

	RoomRef room = player->room.load().lock();
	if (room == nullptr)
		return false;

	// CC 디버그 전용
	const string& msg = pkt.msg();
	if (msg.empty() || msg[0] != '/')
		return true;

	// 가방 디버그 전용
	if (msg.rfind("/bag", 0) == 0)
	{
		room->DoAsync(&Room::DebugDropBag, player->objectInfo->object_id());
		return true;
	}

	if (msg.rfind("/unbag", 0) == 0)
	{
		uint64 bagId = 0;
		if (::sscanf_s(msg.c_str(), "/unbag %llu", &bagId) == 1)
			room->DoAsync(&Room::RemoveBag, bagId);
		return true;
	}

	if (msg.rfind("/die", 0) == 0)
	{
		room->DoAsync(&Room::DebugKill, player->objectInfo->object_id());
		return true;
	}

	char command[32] = {};
	uint64 targetId = 0;
	uint32 durationMs = 0;
	float magnitude = 0.f;

	const int32 parsed = ::sscanf_s(msg.c_str(), "/%31s %llu %u %f",
		command, static_cast<uint32>(sizeof(command)), &targetId, &durationMs, &magnitude);

	if (parsed < 3)
		return true;

	Protocol::CcType type = Protocol::CC_TYPE_NONE;

	if (::strcmp(command, "stun") == 0)
		type = Protocol::CC_TYPE_STUN;
	else if (::strcmp(command, "root") == 0)
		type = Protocol::CC_TYPE_ROOT;
	else if (::strcmp(command, "silence") == 0)
		type = Protocol::CC_TYPE_SILENCE;
	else if (::strcmp(command, "slow") == 0)
		type = Protocol::CC_TYPE_SLOW;
	else return true;

	const uint64 instigatorId = player->objectInfo->object_id();

	LOG_LINE << "[DEBUG CC] " << command << " target=" << targetId
		<< " ms=" << durationMs << " mag=" << magnitude << '\n';

	room->DoAsync(&Room::ApplyCc, targetId, instigatorId, type, durationMs, magnitude);

	return true;
}

bool Handle_C_SKILL(PacketSessionRef& session, Protocol::C_SKILL& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);

	PlayerRef player = gameSession->player.load();
	if (player == nullptr)
		return false;

	RoomRef room = player->room.load().lock();
	if (room == nullptr)
		return false;

	const uint64 casterId = player->objectInfo->object_id();

	room->DoAsync(&Room::HandleSkill, casterId, pkt);

	return true;
}

bool Handle_C_SKILL_CANCEL(PacketSessionRef& session, Protocol::C_SKILL_CANCEL& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);

	PlayerRef player = gameSession->player.load();
	if (player == nullptr)
		return false;

	RoomRef room = player->room.load().lock();
	if (room == nullptr)
		return false;

	room->DoAsync(&Room::HandleSkillCancel, player->objectInfo->object_id());

	return true;
}

bool Handle_C_GRANT_REWARD(PacketSessionRef& session, Protocol::C_GRANT_REWARD& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);

	const string requestId = pkt.request_id();
	const uint64 characterId = pkt.character_id();
	const uint64 gold = pkt.gold();
	const string reason = pkt.reason();

	if (requestId.empty() || requestId.size() > 36 || characterId == 0 || gold == 0)
	{
		SendGrantResult(gameSession, requestId, Protocol::GRANT_BAD_REQUEST, 0);
		return true;
	}

	GDBQueue.Push([gameSession, requestId, characterId, gold, reason](DBConnection* conn)
		{
			const string safeRequestId = conn->Escape(requestId);
			const string safeReason = conn->Escape(reason);

			Protocol::GrantResult result = Protocol::GRANT_DB_ERROR;
			uint64 goldAfter = 0;

			char query[512];

			if (conn->BeginTransaction() == false)
			{
				SendGrantResult(gameSession, requestId, Protocol::GRANT_DB_ERROR, 0);
				return;
			}

			::snprintf(query, sizeof(query),
				"SELECT gold FROM characters WHERE character_id = %llu FOR UPDATE",
				characterId);

			bool targetExists = false;

			if (MYSQL_RES* res = conn->Query(query))
			{
				targetExists = (::mysql_fetch_row(res) != nullptr);
				conn->FreeResult(res);
			}
			else
			{
				LOG_LINE << "[GRANT] lock failed err=" << conn->GetLastErrorNo()
					<< " " << conn->GetError() << '\n';

				conn->Rollback();
				SendGrantResult(gameSession, requestId, Protocol::GRANT_DB_ERROR, 0);
				return;
			}

			// 잠금 단계에서 대상 유무가 판명된다. FK 나 affected_rows 보다 앞선다
			if (targetExists == false)
			{
				conn->Rollback();
				SendGrantResult(gameSession, requestId, Protocol::GRANT_NO_TARGET, 0);
				return;
			}

			::snprintf(query, sizeof(query),
				"INSERT INTO reward_grants (request_id, character_id, gold, reason) "
				"VALUES ('%s', %llu, %llu, '%s')",
				safeRequestId.c_str(), characterId, gold, safeReason.c_str());

			if (conn->Excute(query) == false)
			{
				const uint32 errorNo = conn->GetLastErrorNo();
				conn->Rollback();

				// 1062 = ER_DUP_ENTRY. 이미 처리된 요청.
				if (errorNo == 1062)
				{
					// 이미 지급됐으므로 현재 잔액을 읽어 돌려준다.
					::snprintf(query, sizeof(query),
						"SELECT gold FROM characters WHERE character_id = %llu", characterId);

					if (MYSQL_RES* res = conn->Query(query))
					{
						if (MYSQL_ROW row = ::mysql_fetch_row(res))
							goldAfter = ::strtoull(row[0], nullptr, 10);

						conn->FreeResult(res);
					}

					result = Protocol::GRANT_ALREADY;
				}
				else
				{
					LOG_LINE << "[GRANT] insert failed err=" << errorNo << " " << conn->GetError() << '\n';
					result = Protocol::GRANT_DB_ERROR;
				}

				SendGrantResult(gameSession, requestId, result, goldAfter);
				return;
			}

			// 잔액을 올린다.
			::snprintf(query, sizeof(query), "UPDATE characters SET gold = gold + %llu WHERE character_id = %llu", gold, characterId);

			if (conn->Excute(query) == false)
			{
				LOG_LINE << "[GRANT] update failed err=" << conn->GetLastErrorNo() << " " << conn->GetError() << '\n';
				conn->Rollback();
				SendGrantResult(gameSession, requestId, Protocol::GRANT_DB_ERROR, 0);
				return;
			}

			// 0행이면 캐릭터가 없다는 뜻, FK가 있어 Insert단계에서 걸러지지만, FK가 없더라도 한 번 더 걸러냄
			if (conn->GetAffectedRows() == 0)
			{
				conn->Rollback();
				SendGrantResult(gameSession, requestId, Protocol::GRANT_NO_TARGET, 0);
				return;
			}

			::snprintf(query, sizeof(query), "SELECT gold FROM characters WHERE character_Id = %llu", characterId);

			if (MYSQL_RES* res = conn->Query(query))
			{
				if (MYSQL_ROW row = ::mysql_fetch_row(res))
					goldAfter = ::strtoull(row[0], nullptr, 10);

				conn->FreeResult(res);
			}

			if (conn->Commit() == false)
			{
				LOG_LINE << "[GRANT] commit failed " << conn->GetError() << '\n';
				conn->Rollback();
				SendGrantResult(gameSession, requestId, Protocol::GRANT_DB_ERROR, 0);
				return;
			}

			SendGrantResult(gameSession, requestId, Protocol::GRANT_OK, goldAfter);
		});

	return true;
}

bool Handle_C_EQUIP(PacketSessionRef& session, Protocol::C_EQUIP& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);

	if (gameSession->accountId.load() == 0)
		return false;

	if (gameSession->TryBeginRequest(pkt.request_id()) == false)
	{
		gameSession->SendItemResult(pkt.request_id(), Protocol::ITEM_ALREADY);
		return true;
	}

	// 베이스캠프 - DB 직행. 슬롯 규칙은 Room::HandleEquip과 같다.
	if (gameSession->place.load() == SessionPlace::BaseCamp)
	{
		if (pkt.slot() < Protocol::SLOT_WEAPON_PRIMARY || pkt.slot() > Protocol::SLOT_TRINKET)
			gameSession->SendItemResult(pkt.request_id(), Protocol::ITEM_BAD_REQUEST);
		else if (pkt.slot() == Protocol::SLOT_WEAPON_SECONDARY)
			gameSession->SendItemResult(pkt.request_id(), Protocol::ITEM_INVALID_STATE);
		else
			PushBaseCampItemOp(gameSession, pkt.request_id(), pkt.instance_id(), BaseCampOp::Equip, pkt.slot());
		return true;
	}

	// 던전 - Room 메모리 우선
	PlayerRef player = gameSession->player.load();
	RoomRef room = (player != nullptr) ? player->room.load().lock() : nullptr;
	if (room == nullptr)
	{
		gameSession->SendItemResult(pkt.request_id(), Protocol::ITEM_INVALID_STATE);
		return true;
	}

	room->DoAsync(&Room::HandleEquip, gameSession, pkt.request_id(), pkt.instance_id(), pkt.slot());

	return true;
}

bool Handle_C_UNEQUIP(PacketSessionRef& session, Protocol::C_UNEQUIP& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);

	if (gameSession->accountId.load() == 0)
		return false;

	if (gameSession->TryBeginRequest(pkt.request_id()) == false)
	{
		gameSession->SendItemResult(pkt.request_id(), Protocol::ITEM_ALREADY);
		return true;
	}

	if (gameSession->place.load() == SessionPlace::BaseCamp)
	{
		PushBaseCampItemOp(gameSession, pkt.request_id(), pkt.instance_id(), BaseCampOp::Unequip);
		return true;
	}

	PlayerRef player = gameSession->player.load();
	RoomRef room = (player != nullptr) ? player->room.load().lock() : nullptr;
	if (room == nullptr)
	{
		gameSession->SendItemResult(pkt.request_id(), Protocol::ITEM_INVALID_STATE);
		return true;
	}

	room->DoAsync(&Room::HandleUnequip, gameSession, pkt.request_id(), pkt.instance_id());

	return true;
}

bool Handle_C_LOOT(PacketSessionRef& session, Protocol::C_LOOT& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);
	
	PlayerRef player = gameSession->player.load();
	if (player == nullptr)
		return false;

	RoomRef room = player->room.load().lock();
	if (room == nullptr)
		return false;

	if (gameSession->TryBeginRequest(pkt.request_id()) == false)
	{
		gameSession->SendItemResult(pkt.request_id(), Protocol::ITEM_ALREADY);
		return true;
	}

	room->DoAsync(&Room::HandleLoot, gameSession, pkt.request_id(), pkt.bag_id(), pkt.instance_id());
	
	return true;
}

bool Handle_C_DROP(PacketSessionRef& session, Protocol::C_DROP& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);

	if (gameSession->accountId.load() == 0)
		return false;

	if (gameSession->TryBeginRequest(pkt.request_id()) == false)
	{
		gameSession->SendItemResult(pkt.request_id(), Protocol::ITEM_ALREADY);
		return true;
	}

	if (gameSession->place.load() == SessionPlace::BaseCamp)
	{
		PushBaseCampItemOp(gameSession, pkt.request_id(), pkt.instance_id(), BaseCampOp::Destroy);
		return true;
	}

	PlayerRef player = gameSession->player.load();
	RoomRef room = (player != nullptr) ? player->room.load().lock() : nullptr;
	if (room == nullptr)
	{
		gameSession->SendItemResult(pkt.request_id(), Protocol::ITEM_INVALID_STATE);
		return true;
	}
	room->DoAsync(&Room::HandleDrop, gameSession, pkt.request_id(), pkt.instance_id());

	return true;
}

bool Handle_C_STASH(PacketSessionRef& session, Protocol::C_STASH& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);

	if (gameSession->accountId.load() == 0)
		return false;

	if (gameSession->TryBeginRequest(pkt.request_id()) == false)
	{
		gameSession->SendItemResult(pkt.request_id(), Protocol::ITEM_ALREADY);
		return true;
	}

	// 던전 안에서는 창고에 접근할 수 없다. 입장 중(Entering)도 막는다.
	if (gameSession->place.load() != SessionPlace::BaseCamp)
	{
		gameSession->SendItemResult(pkt.request_id(), Protocol::ITEM_INVALID_STATE);
		return true;
	}

	PushBaseCampItemOp(gameSession, pkt.request_id(), pkt.instance_id(),
		pkt.to_vault() ? BaseCampOp::ToVault : BaseCampOp::ToCarried);

	return true;
}

bool Handle_C_BAG_OPEN(PacketSessionRef& session, Protocol::C_BAG_OPEN& pkt)
{
	auto gameSession = static_pointer_cast<GameSession>(session);

	PlayerRef player = gameSession->player.load();
	if (player == nullptr)
		return false;

	RoomRef room = player->room.load().lock();
	if (room == nullptr)
		return false;

	room->DoAsync(&Room::HandleBagOpen, gameSession, pkt.bag_id());
	return true;
}
