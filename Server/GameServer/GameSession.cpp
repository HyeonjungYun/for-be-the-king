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
	wcout << "[DISCONNECT] OnDisconnected 진입" << endl;

	GSessionManager.Remove(static_pointer_cast<GameSession>(shared_from_this()));

	GAccountManager.LogOut(accountId.exchange(0));

	PlayerRef leavingPlayer = player.load();

	if (leavingPlayer == nullptr)
	{
		wcout << "[DISCONNECT] player == nullptr — 방 이탈 생략" << endl;
		return;
	}

	cout << "[DISCONNECT] player id=" << leavingPlayer->objectInfo->object_id() << endl;

	if (RoomRef room = leavingPlayer->room.load().lock())
	{
		wcout << "[DISCONNECT] room 유효 — DoAsync 등록" << endl;
		room->DoAsync(&Room::HandleLeavePlayer, leavingPlayer);
	}
	else
	{
		wcout << "[DISCONNECT] room == nullptr — 방 이탈 실패" << endl;
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
