#include "pch.h"
#include "ServerPacketHandler.h"
#include "BufferReader.h"
#include "BufferWriter.h"
#include "ObjectUtils.h"
#include "Room.h"
#include "Player.h"

PacketHandlerFunc GPacketHandler[UINT16_MAX];

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

			if (accountId != 0)
			{
				::snprintf(query, sizeof(query),
					"SELECT character_id, name, hp, max_hp, floor_id FROM characters "
					"WHERE account_id = %llu ORDER BY character_id",
					accountId);

				if (MYSQL_RES* result = conn->Query(query))
				{
					while (MYSQL_ROW row = ::mysql_fetch_row(result))
					{
						Protocol::CharacterInfo* info = loginPkt.add_characters();
						info->set_character_id(::strtoull(row[0], nullptr, 10));
						info->set_name(row[1] ? row[1] : "");
						info->set_hp(atoi(row[3]));
						info->set_floor_id(static_cast<uint32>(::atoi(row[4])));
					}

					conn->FreeResult(result);
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
	PlayerRef player = ObjectUtils::CreatPlayer(gameSession);

	player->maxHp = character.max_hp();
	player->hp = character.hp();

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
