#include "pch.h"
#include "GameSession.h"
#include "GameSessionManager.h"
#include "ServerPacketHandler.h"
#include "Player.h"
#include "Room.h"
#include "AccountManager.h"

void GameSession::OnConnected()
{
	GSessionManager.Add(static_pointer_cast<GameSession>(shared_from_this()));
}

void GameSession::OnDisconnected()
{
	LOG_LINE << "[DISCONNECT] enter\n";

	GSessionManager.Remove(static_pointer_cast<GameSession>(shared_from_this()));

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
