#pragma once
#include "Struct.pb.h"
#include "Session.h"

class Player;

enum class SessionPlace : uint8
{
	None = 0,
	BaseCamp,
	Dungeon,
	Entering,
	Escaping,
};

class GameSession : public PacketSession
{
public:
	~GameSession()
	{
		LOG_LINE << "~GameSession\n";		// endl 은 매번 플러시한다 - 봇 부하에서 세션마다
	}

	virtual void OnConnected() override;
	virtual void OnDisconnected() override;
	virtual void OnRecvPacket(BYTE* buffer, int32 len) override;
	virtual void OnSend(int32 len) override;

public:
	// 같은 request_id 는 한 번만. 예전엔 길이 제한도 상한도 없어 연결 하나가 메모리를 끝없이 늘릴 수 있었고, 64 KB 를 넘는 id 는
	// 응답(S_ITEM_RESULT)에 실려 uint16 패킷 크기를 넘겼다 (2026-10-17 리뷰). 클라 id 는 32 자 GUID 다
	static constexpr size_t MAX_REQUEST_ID = 64;
	static constexpr size_t MAX_TRACKED_REQUESTS = 4096;

	bool TryBeginRequest(const string& requestId)
	{
		if (requestId.empty() || requestId.size() > MAX_REQUEST_ID)
			return false;

		lock_guard<mutex> guard(_requestLock);
		if (_handleRequests.insert(requestId).second == false)
			return false;

		// 재전송 창 = 최근 4096 개, 가장 오래된 것부터 하나씩 뺀다. 예전엔 꽉 차면 통째로 비워서 그 순간 바로 전의 id 를
		// 다시 보내면 받아 줬다 (안티치트 감사 7, 2026-10-19). 진짜 멱등은 DB 상태가 맡는다 - 이건 재전송 방지다
		_requestOrder.push_back(requestId);
		if (_requestOrder.size() > MAX_TRACKED_REQUESTS)
		{
			_handleRequests.erase(_requestOrder.front());
			_requestOrder.pop_front();
		}
		return true;
	}

public:
	atomic<shared_ptr<Player>> player;

public:
	atomic<uint64> accountId = 0;
	atomic<bool> loginStarted = false;		// C_LOGIN 을 받았다 - 실패하면 되돌린다. 연결 하나에 로그인 하나 (ADR-0011)
	static constexpr int32 MAX_LOGIN_ATTEMPTS = 10;
	atomic<int32> loginAttempts = 0;		// 이 연결이 보낸 C_LOGIN 수 - 넘으면 끊는다 (토큰 대입, 보안 점검 M3)
	vector<Protocol::CharacterInfo> characters;
	atomic<SessionPlace> place = SessionPlace::None;

	// 고독한 길 (2026-10-17) - 지금 착용한 '고독한' 옵션 아이템 수. 0 보다 크면 파티를 맺을 수 없다 (PartyManager).
	// 몸이 있으면 Player::ApplyEquipmentStats 가, 베이스캠프 DB 조작 뒤엔 SyncBaseCamp 가 맞춘다
	atomic<int32> lonePieces = 0;

	// 마지막 베이스캠프 방 요청 시각 (µs) - 연타 거절 (EnterBaseCamp)
	atomic<uint64> lastCampEnterUs = 0;

	// [ID MISMATCH] 는 세션당 한 번만 남긴다 - 잘못된 C_MOVE 마다 찍혔다
	atomic<bool> idMismatchLogged = false;

	// ── 연결당 속도 제한 (2026-10-17 사용자 결정 · 보안 점검 H2) ──
	// 전체 패킷 초당 120 · 순간 240 - 넘으면 끊는다 (정상 클라는 이동 30 Hz + 스킬 · 평타).
	// DB 를 치는 요청 초당 10 · 순간 20 - 넘는 요청은 버리고, 50 번 버리면 끊는다 (클릭 한 번에 하나라 정상 손으로는 닿지 않는다).
	// 로그인 전 연결은 10 초 (메인 루프가 10 초마다 보니 실제로는 10 ~ 20 초) - GameSessionManager::DropUnauthenticated
	static constexpr double	PACKET_RATE_PER_SEC = 120.0;
	static constexpr double	PACKET_BURST = 240.0;
	static constexpr double	DB_RATE_PER_SEC = 10.0;
	static constexpr double	DB_BURST = 20.0;
	static constexpr int32	DB_DROPS_BEFORE_DISCONNECT = 50;
	static constexpr uint64	LOGIN_TIMEOUT_US = 10'000'000;

	atomic<uint64> connectedAtUs = 0;

	// 패킷 하나를 받아도 되는가 - false 면 이미 끊었거나 버릴 패킷이다. 수신은 세션마다 한 스레드씩이라 락이 없다
	bool AdmitPacket(uint16 packetId);

	// 로그인 전 연결이 보낼 수 있는 패킷 - 나머지는 해석도 하지 않고 버린다 (2026-10-21). 예전엔 핸들러 9 개가 각자 accountId 를 검사했고
	// 빠뜨리면 컴파일러가 잡아 주지 않았다. 버린 수가 PRE_LOGIN_DROPS_BEFORE_DISCONNECT 를 넘으면 끊는다
	static bool AllowedBeforeLogin(uint16 packetId);
	static constexpr int32	PRE_LOGIN_DROPS_BEFORE_DISCONNECT = 20;

	// 마지막으로 패킷을 받은 시각 (µs) - 로그인한 연결의 유휴 끊기 (GameSessionManager::DropIdle, 클라 하트비트가 채운다)
	atomic<uint64> lastRecvUs = 0;


	// gold >= 0 이면 바뀐 창고 골드를 싣는다 (매입 · 강화 - 2026-10-17 경제)
	void SendItemResult(const string& requestId, Protocol::ItemResult result, const vector<Protocol::ItemInstance>& changed = {}, int64 gold = -1);

private:
	// 요청 id 집합 전용 - 예전엔 USE_LOCK 을 다시 선언해 Session::_locks(송신 큐 락)를 같은 이름으로 가렸다 (2026-10-21)
	mutex _requestLock;
	unordered_set<string> _handleRequests;
	deque<string>		_requestOrder;		// _handleRequests 에 넣은 순서 - 오래된 것부터 뺀다

	struct TokenBucket
	{
		double	tokens = -1.0;		// 처음 쓸 때 가득 채운다
		uint64	lastUs = 0;
		bool	Take(double ratePerSec, double burst, uint64 nowUs);
	};
	TokenBucket	_packetBucket;
	TokenBucket	_dbBucket;
	int32		_dbDrops = 0;
	int32		_preLoginDrops = 0;
};
