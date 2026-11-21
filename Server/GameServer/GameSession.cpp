#include "pch.h"
#include "GameSession.h"
#include "GameSessionManager.h"
#include "ServerPacketHandler.h"
#include "Player.h"
#include "Room.h"
#include "AccountManager.h"
#include "PartyManager.h"
#include "Cluster.h"

void GameSession::OnConnected()
{
	connectedAtUs.store(Utils::NowMicroseconds());

	// IP 당 연결 수 (2026-10-18) - 한 곳에서 연결을 쏟아부어 Accept 슬롯 · 세션 메모리(640 KB/개)를 다 쓰지 못하게
	string ip;
	for (wchar_t c : GetAddress().GetIpAddress())
		ip.push_back(static_cast<char>(c < 128 ? c : '?'));

	switch (GSessionManager.Add(static_pointer_cast<GameSession>(shared_from_this()), ip))
	{
	case GameSessionManager::AddResult::Ok:
		break;

	case GameSessionManager::AddResult::TooManyFromIp:
		LOG_LINE << "[RATE] too many connections from " << ip << " - refused\n";
		Disconnect(L"too many connections from this address");
		break;

	case GameSessionManager::AddResult::ServerFull:
	{
		// 동시 연결 상한 (FBTK_MAX_SESSIONS, 2026-11-21) - 이유를 알리고 끊는다. 클라는 "서버가 가득 찼습니다"
		static atomic<uint64> s_fullCount = 0;
		const uint64 n = ++s_fullCount;
		if (n <= 5 || n % 1000 == 0)
			LOG_LINE << "[CAPACITY] server full - refused " << ip << " (total refused " << n << ")\n";
		Protocol::S_KICK kickPkt;
		kickPkt.set_reason(Protocol::KICK_REASON_SERVER_FULL);
		SEND_PACKET_DECLARATION(kickPkt);
		Send(sendBuffer);
		Disconnect(L"server full");
		break;
	}
	}
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
	GSessionManager.Remove(static_pointer_cast<GameSession>(shared_from_this()));

	// PVE 협동 파티 (ADR-0014) - 베이스캠프에서 끊기면 파티에서 빠지고, 던전에서 끊기면 "접속 끊김" 으로 남는다.
	// accountId 를 비우기 전에 캐릭터를 읽는다 (characters 는 accountId 보다 먼저 쓰였다)
	const uint64 characterId = (accountId.load() != 0 && characters.empty() == false) ? characters[0].object_info().object_id() : 0;

	// 누가 끊겼는지를 첫 줄에 (2026-10-21 - 예전 "[DISCONNECT] enter" 는 id 가 없어 이벤트 로그가 플레이어에 붙이지 못했다)
	LOG_LINE << "[DISCONNECT] begin char=" << characterId << " account=" << accountId.load() << " place=" << static_cast<int32>(place.load()) << '\n';
	GParties.OnDisconnected(characterId, place.load());

	const uint64 leavingAccount = accountId.exchange(0);
	GAccountManager.LogOut(leavingAccount, this);		// 주인이 나일 때만 - 중복 로그인으로 밀려난 옛 연결이 새 연결의 계정을 풀지 않게
	Cluster::ReleaseAccount(leavingAccount);			// 서버 여러 대 - 접속 계정 표에서 (그 사이 다시 붙었으면 지우지 않는다, 2026-11-22)

	PlayerRef leavingPlayer = player.load();

	if (leavingPlayer == nullptr)
	{
		LOG_LINE << "[DISCONNECT] player == nullptr - skip room leave\n";
		return;
	}

	if (RoomRef room = leavingPlayer->room.load().lock())
	{
		LOG_LINE << "[DISCONNECT] leaving char=" << leavingPlayer->objectInfo->object_id() << " room=" << room->Tag() << '\n';
		room->DoAsync(&Room::HandleDisconnect, leavingPlayer);
	}
	else
	{
		LOG_LINE << "[DISCONNECT] room == nullptr - leave failed\n";
	}

	player.store(nullptr);
}

bool GameSession::AllowedBeforeLogin(uint16 packetId)
{
	switch (packetId)
	{
	case PKT_C_LOGIN:
	case PKT_C_HEARTBEAT:
		return true;
	default:
		return false;
	}
}

void GameSession::OnRecvPacket(BYTE* buffer, int32 len)
{
	const uint16 packetId = reinterpret_cast<PacketHeader*>(buffer)->id;

	// 속도 제한 - 넘으면 버리거나 끊는다 (AdmitPacket)
	if (AdmitPacket(packetId) == false)
		return;

	lastRecvUs.store(Utils::NowMicroseconds());

	// 중앙 인증 게이트 - 로그인 전에는 C_LOGIN 만. 핸들러의 accountId 검사는 2 중 방어로 남는다
	if (accountId.load() == 0 && AllowedBeforeLogin(packetId) == false)
	{
		if (++_preLoginDrops >= PRE_LOGIN_DROPS_BEFORE_DISCONNECT)
		{
			LOG_LINE << "[RATE] packets before login - disconnect last_id=" << packetId << '\n';
			Disconnect(L"packets before login");
		}
		return;
	}

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
