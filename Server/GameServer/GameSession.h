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

		WRITE_LOCK;
		if (_handleRequests.size() >= MAX_TRACKED_REQUESTS)
			_handleRequests.clear();		// 재전송 창 = 최근 4096 개. 재전송은 몇 초 안에 온다
		return _handleRequests.insert(requestId).second;
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

	// gold >= 0 이면 바뀐 창고 골드를 싣는다 (매입 · 강화 - 2026-10-17 경제)
	void SendItemResult(const string& requestId, Protocol::ItemResult result, const vector<Protocol::ItemInstance>& changed = {}, int64 gold = -1);

private:
	USE_LOCK;
	unordered_set<string> _handleRequests;
};
