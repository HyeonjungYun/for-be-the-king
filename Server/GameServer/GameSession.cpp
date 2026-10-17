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

	// 모르는 id 는 Handle_INVALID 가 끊는다. 표는 uint16 전 범위라 id 대역 검사가 따로 필요 없다 (2026-10-17)
	ServerPacketHandler::HandlePacket(session, buffer, len);
}

void GameSession::OnSend(int32 len)
{
}

void GameSession::SendItemResult(const string& requestId, Protocol::ItemResult result, const vector<Protocol::ItemInstance>& changed, int64 gold)
{
	Protocol::S_ITEM_RESULT pkt;
	// 받은 id 를 그대로 돌려준다 - 길면 잘라서. 64 KB 가까운 id 가 응답 크기(uint16)를 넘겼다 (보안 점검 C2). 정상 id 는 32 자다
	pkt.set_request_id(requestId.size() > MAX_REQUEST_ID ? requestId.substr(0, MAX_REQUEST_ID) : requestId);
	pkt.set_result(result);
	if (gold >= 0)
		pkt.set_gold(static_cast<uint64>(gold));

	for (const Protocol::ItemInstance& c : changed)
		pkt.add_changed()->CopyFrom(c);

	SEND_PACKET_DECLARATION(pkt);
	Send(sendBuffer);
}
