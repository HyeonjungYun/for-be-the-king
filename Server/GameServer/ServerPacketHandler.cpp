#include "pch.h"
#include "ServerPacketHandler.h"
#include "BufferReader.h"
#include "BufferWriter.h"
#include "ObjectUtils.h"
#include "Room.h"
#include "Player.h"
#include "AccountManager.h"

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
				cout << "[LOGIN] already online - account=" << accountId << endl;
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
						cout << "[LOGIN] column count mismatch - expected " << COL_COUNT << " got " << ::mysql_num_fields(result) << endl;
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

	RoomRef room = GetRoomForFloor(pkt.floor_id());
	if (room == nullptr)
		return false;

	// 플레이어 생성
	PlayerRef player = ObjectUtils::CreatePlayer(gameSession, character.object_info().object_id());

	player->maxHp = character.max_hp();
	player->hp = character.hp();

	const Protocol::PosInfo& saved = character.object_info().pos_info();
	player->posInfo->set_x(saved.x());
	player->posInfo->set_y(saved.y());
	player->posInfo->set_z(saved.z());
	player->posInfo->set_yaw(saved.yaw());

	// 방에 입장
	room->DoAsync(&Room::HandleEnterPlayer, player);

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
		cout << "[ID MISMATCH] claimed=" << pkt.info().object_id()
			<< " actual=" << realId << endl;
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

	cout << "[DEBUG CC] " << command << " target=" << targetId
		<< " ms=" << durationMs << " mag=" << magnitude << endl;

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
				cout << "[GRANT] lock failed err=" << conn->GetLastErrorNo()
					<< " " << conn->GetError() << endl;

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
					cout << "[GRANT] insert failed err=" << errorNo << " " << conn->GetError() << endl;
					result = Protocol::GRANT_DB_ERROR;
				}

				SendGrantResult(gameSession, requestId, result, goldAfter);
				return;
			}

			// 잔액을 올린다.
			::snprintf(query, sizeof(query), "UPDATE characters SET gold = gold + %llu WHERE character_id = %llu", gold, characterId);

			if (conn->Excute(query) == false)
			{
				cout << "[GRANT] update failed err=" << conn->GetLastErrorNo() << " " << conn->GetError() << endl;
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
				cout << "[GRANT] commit failed " << conn->GetError() << endl;
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
	return true;
}

bool Handle_C_UNEQUIP(PacketSessionRef& session, Protocol::C_UNEQUIP& pkt)
{
	return true;
}

bool Handle_C_LOOT(PacketSessionRef& session, Protocol::C_LOOT& pkt)
{
	return true;
}

bool Handle_C_DROP(PacketSessionRef& session, Protocol::C_DROP& pkt)
{
	return true;
}

bool Handle_C_STASH(PacketSessionRef& session, Protocol::C_STASH& pkt)
{
	return true;
}
