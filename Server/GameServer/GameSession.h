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
		LOG_LINE << "~GameSession" << endl;
	}

	virtual void OnConnected() override;
	virtual void OnDisconnected() override;
	virtual void OnRecvPacket(BYTE* buffer, int32 len) override;
	virtual void OnSend(int32 len) override;

public:
	bool TryBeginRequest(const string& requestId)
	{
		if (requestId.empty())
			return false;

		WRITE_LOCK;
		return _handleRequests.insert(requestId).second;
	}

public:
	atomic<shared_ptr<Player>> player;

public:
	atomic<uint64> accountId = 0;
	atomic<bool> loginStarted = false;		// C_LOGIN 을 받았다 - 실패하면 되돌린다. 연결 하나에 로그인 하나 (ADR-0011)
	vector<Protocol::CharacterInfo> characters;
	atomic<SessionPlace> place = SessionPlace::None;

	// 고독한 길 (2026-10-17) - 지금 착용한 '고독한' 옵션 아이템 수. 0 보다 크면 파티를 맺을 수 없다 (PartyManager).
	// 몸이 있으면 Player::ApplyEquipmentStats 가, 베이스캠프 DB 조작 뒤엔 SyncBaseCamp 가 맞춘다
	atomic<int32> lonePieces = 0;

	// gold >= 0 이면 바뀐 창고 골드를 싣는다 (매입 · 강화 - 2026-10-17 경제)
	void SendItemResult(const string& requestId, Protocol::ItemResult result, const vector<Protocol::ItemInstance>& changed = {}, int64 gold = -1);

private:
	USE_LOCK;
	unordered_set<string> _handleRequests;
};
