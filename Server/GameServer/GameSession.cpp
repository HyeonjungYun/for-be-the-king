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
	connectedAtUs.store(Utils::NowMicroseconds());
	GSessionManager.Add(static_pointer_cast<GameSession>(shared_from_this()));
}

bool GameSession::TokenBucket::Take(double ratePerSec, double burst, uint64 nowUs)
{
	if (tokens < 0.0)
		tokens = burst;
	else if (nowUs > lastUs)
		tokens = (std::min)(burst, tokens + static_cast<double>(nowUs - lastUs) * 1e-6 * ratePerSec);
	lastUs = nowUs;

	if (tokens < 1.0)
		return false;
	tokens -= 1.0;
	return true;
}

namespace
{
	// DB 작업을 일으키는 요청 - 로그인 · 입장 적재 · 베이스캠프 아이템 조작(던전에선 방 작업이지만 같이 센다)
	bool IsDbBound(uint16 id)
	{
		switch (id)
		{
		case PKT_C_LOGIN:
		case PKT_C_ENTER_GAME:
		case PKT_C_STASH:
		case PKT_C_EQUIP:
		case PKT_C_UNEQUIP:
		case PKT_C_DROP:
		case PKT_C_ENHANCE:
		case PKT_C_SELL:
			return true;
		default:
			return false;		// C_GRANT_REWARD 는 시험 전용 끝점이라 뺀다 (DummyClient -grant 가 한 번에 수백 개를 보낸다)
		}
	}
}

bool GameSession::AdmitPacket(uint16 packetId)
{
	// 끊은 뒤에도 같은 수신 버퍼에 남은 패킷은 OnRecv 가 계속 넘긴다 - 처리하지 않는다
	if (IsConnected() == false)
		return false;

	const uint64 nowUs = Utils::NowMicroseconds();

	if (_packetBucket.Take(PACKET_RATE_PER_SEC, PACKET_BURST, nowUs) == false)
	{
		LOG_LINE << "[RATE] packet flood - disconnect account=" << accountId.load() << " last_id=" << packetId << '\n';
		Disconnect(L"packet rate");
		return false;
	}

	if (IsDbBound(packetId) && _dbBucket.Take(DB_RATE_PER_SEC, DB_BURST, nowUs) == false)
	{
		if (++_dbDrops >= DB_DROPS_BEFORE_DISCONNECT)
		{
			LOG_LINE << "[RATE] db request flood - disconnect account=" << accountId.load() << '\n';
			Disconnect(L"db request rate");
		}
		else if (_dbDrops == 1)
		{
			LOG_LINE << "[RATE] db request over limit - dropping account=" << accountId.load() << " id=" << packetId << '\n';
		}
		return false;
	}

	return true;
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
	// 속도 제한 - 넘으면 버리거나 끊는다 (AdmitPacket)
	if (AdmitPacket(reinterpret_cast<PacketHeader*>(buffer)->id) == false)
		return;

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
