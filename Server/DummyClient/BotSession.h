#pragma once
#include "Session.h"

/*-----------------
	 BotSession
-----------------*/

class BotSession : public PacketSession
{
public:
	virtual void OnConnected() override;
	virtual void OnRecvPacket(BYTE* buffer, int32 len) override;
	virtual void OnDisconnected() override;

public:
	atomic<bool> inGame = false;
	atomic<uint64> objectId = 0;

	float x = 0.f;
	float y = 0.f;
	float z = 100.f;
	float destX = 0.f;
	float destY = 0.f;
};

extern mutex GBotsLock;
extern vector<BotSessionRef> GBots;
