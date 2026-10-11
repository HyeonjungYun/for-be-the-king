#include "pch.h"
#include "GameSession.h"
#include "GameSessionManager.h"
#include "ServerPacketHandler.h"
#include "Player.h"
#include "Room.h"
#include "AccountManager.h"
#include "PartyManager.h"

void GameSession::OnConnected()
{
	GSessionManager.Add(static_pointer_cast<GameSession>(shared_from_this()));
}

void GameSession::OnDisconnected()
{
	LOG_LINE << "[DISCONNECT] enter\n";

	GSessionManager.Remove(static_pointer_cast<GameSession>(shared_from_this()));

	// PVE 협동 파티 (ADR-0014) - 베이스캠프에서 끊기면 파티에서 빠지고, 던전에서 끊기면 "접속 끊김" 으로 남는다.
	// accountId 를 비우기 전에 캐릭터를 읽는다 (characters 는 accountId 보다 먼저 쓰였다)
	const uint64 characterId = (accountId.load() != 0 && characters.empty() == false) ? characters[0].object_info().object_id() : 0;
	GParties.OnDisconnected(characterId, place.load());

	GAccountManager.LogOut(accountId.exchange(0));

	PlayerRef leavingPlayer = player.load();

	if (leavingPlayer == nullptr)
	{
		LOG_LINE << "[DISCONNECT] player == nullptr - skip room leave\n";
		return;
	}

	LOG_LINE << "[DISCONNECT] player id=" << leavingPlayer->objectInfo->object_id() << '\n';

	if (RoomRef room = leavingPlayer->room.load().lock())
	{
		LOG_LINE << "[DISCONNECT] room ok - DoAsync queued\n";
		room->DoAsync(&Room::HandleDisconnect, leavingPlayer);
	}
	else
	{
		LOG_LINE << "[DISCONNECT] room == nullptr - leave failed\n";
	}

	player.store(nullptr);
}

void GameSession::OnRecvPacket(BYTE* buffer, int32 len)
{
	PacketSessionRef session = GetPacketSessionRef();
	PacketHeader* header = reinterpret_cast<PacketHeader*>(buffer);

	// TODO : packetId 대역 체크
	ServerPacketHandler::HandlePacket(session, buffer, len);
}

void GameSession::OnSend(int32 len)
{
}

void GameSession::SendItemResult(const string& requestId, Protocol::ItemResult result, const vector<Protocol::ItemInstance>& changed)
{
	Protocol::S_ITEM_RESULT pkt;
	pkt.set_request_id(requestId);
	pkt.set_result(result);

	for (const Protocol::ItemInstance& c : changed)
		pkt.add_changed()->CopyFrom(c);

	SEND_PACKET_DECLARATION(pkt);
	Send(sendBuffer);
}
