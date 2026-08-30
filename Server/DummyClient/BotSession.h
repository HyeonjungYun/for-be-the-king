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
	uint32 floorId = 0;
	atomic<uint64> objectId = 0;

	atomic<bool> alive = true;

	float x = 0.f;
	float y = 0.f;
	float z = 100.f;
	float destX = 0.f;
	float destY = 0.f;

	uint64 nextAttackAtUs = 0;

	uint64 nextSkillAtUs = 0;
	uint64 castUnitlUs = 0;

	atomic<uint64> hardCcUntilUs = 0;

	uint64 nextDashAtUs = 0;
	uint64 nextAreaAtUs = 0;
	atomic<uint64> dashUntilUs = 0;
	float dashDirX = 0.f;
	float dashDirY = 0.f;
};

constexpr float DASH_SPEED = 1200.f;
constexpr float DASH_DIST = 400.f;
constexpr uint64 DASH_DURATION_US = static_cast<uint64>(DASH_DIST / DASH_SPEED * 1'000'000.0);

using BotSessionRef = std::shared_ptr<BotSession>;

extern mutex GBotsLock;
extern vector<BotSessionRef> GBots;
