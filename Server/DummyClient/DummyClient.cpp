#include "pch.h"
#include <iostream>
#include "ThreadManager.h"
#include "Service.h"
#include "Session.h"
#include "ClientPacketHandler.h"
#include "BotSession.h"

mutex GBotsLock;
vector<BotSessionRef> GBots;

namespace
{
	constexpr float BOT_MOVE_SPEED = 340.f; 
	constexpr float WANDER_RADIUS = 1000.f; 
	constexpr float ARRIVE_EPSILON = 20.f;  
	constexpr int   MOVE_INTERVAL_MS = 33;
}

static void TickBots(float deltaTime)
{
	vector<BotSessionRef> snapshot;
	{
		lock_guard<mutex> guard(GBotsLock);
		snapshot = GBots;
	}

	for (BotSessionRef& bot : snapshot)
	{
		if (bot->inGame.load() == false)
			continue;

		const float dx = bot->destX - bot->x;
		const float dy = bot->destY - bot->y;
		const float dist = ::sqrtf(dx * dx + dy * dy);

		if (dist < ARRIVE_EPSILON)
		{
			bot->destX = bot->x + Utils::GetRandom(-WANDER_RADIUS, WANDER_RADIUS);
			bot->destY = bot->y + Utils::GetRandom(-WANDER_RADIUS, WANDER_RADIUS);
		}
		else
		{
			const float step = BOT_MOVE_SPEED * deltaTime;
			const float ratio = (step < dist) ? (step / dist) : 1.f;

			bot->x += dx * ratio;
			bot->y += dy * ratio;
		}

		Protocol::C_MOVE movePkt;
		{
			Protocol::PosInfo* info = movePkt.mutable_info();
			info->set_object_id(bot->objectId.load());
			info->set_x(bot->x);
			info->set_y(bot->y);
			info->set_z(bot->z);
			info->set_yaw(0.f);
			info->set_state(Protocol::MOVE_STATE_RUN);
		}

		bot->Send(ClientPacketHandler::MakeSendBuffer(movePkt));
	}
}

int main(int argc, char* argv[])
{
	int32 botCount = 1;

	for (int i = 1; i < argc - 1; i++)
	{
		if (::strcmp(argv[i], "-bots") == 0)
			botCount = ::atoi(argv[i + 1]);
	}
	if (botCount < 1)
		botCount = 1;

	cout << "[BOT] launching " << botCount << " bot(s)" << endl;

	ClientPacketHandler::Init();

	ClientServiceRef service = make_shared<ClientService>(
		NetAddress(L"127.0.0.1", 7777),
		make_shared<IocpCore>(),
		[]() {return make_shared<BotSession>(); },
		botCount
	);

	ASSERT_CRASH(service->Start());

	for (int32 i = 0; i < 2; i++)
	{
		GThreadManager->Launch([service]()
			{
				while (true)
					service->GetIocpCore()->Dispatch();
			});
	}

	uint64 lastUs = Utils::NowMicroseconds();

	while (true)
	{
		this_thread::sleep_for(chrono::milliseconds(MOVE_INTERVAL_MS));

		const uint64 nowUs = Utils::NowMicroseconds();
		const float deltaTime = static_cast<float>(nowUs - lastUs) / 1000000.f;
		lastUs = nowUs;

		TickBots(deltaTime);
	}

	GThreadManager->Join();
}