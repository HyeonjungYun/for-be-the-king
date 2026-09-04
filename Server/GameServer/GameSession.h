#pragma once
#include "Struct.pb.h"
#include "Session.h"

class Player;

class GameSession : public PacketSession
{
public:
	~GameSession()
	{
		cout << "~GameSession" << endl;
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
	vector<Protocol::CharacterInfo> characters;


	void SendItemResult(const string& requestId, Protocol::ItemResult result, const vector<Protocol::ItemInstance>& changed = {});

private:
	USE_LOCK;
	unordered_set<string> _handleRequests;
};
