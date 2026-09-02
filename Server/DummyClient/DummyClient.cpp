#include "pch.h"
#include <iostream>
#include <timeapi.h>
#include "ThreadManager.h"
#include "Service.h"
#include "Session.h"
#include "ClientPacketHandler.h"
#include "BotSession.h"

#pragma comment(lib, "winmm.lib")

mutex GBotsLock;
vector<BotSessionRef> GBots;
std::atomic<uint64> GSentThisWindow{ 0 };

atomic<uint64> GGrantTargetId = 0;
atomic<uint64> GGrantOk = 0;
atomic<uint64> GGrantAlready = 0;
atomic<uint64> GGrantNoTarget = 0;
atomic<uint64> GGrantError = 0;
atomic<uint64> GGrantBad = 0;
atomic<uint64> GGrantReplies = 0;

namespace
{
	constexpr float BOT_MOVE_SPEED = 340.f;
	constexpr float WANDER_RADIUS = 1000.f;
	constexpr float ARRIVE_EPSILON = 20.f;
	constexpr int   MOVE_INTERVAL_MS = 33;
	constexpr int   MOVE_CHAT_MS = 2000;

	constexpr float ATTACK_RANGE = 150.f;
	constexpr float ATTACK_RANGE_SQ = ATTACK_RANGE * ATTACK_RANGE;
	constexpr float BOT_STOP_DISTANCE = 120.f;
	constexpr uint64 ATTACK_INTERVAL_US = 1'000'000;
	constexpr uint64 REFILL_INTERVAL_US = 1'000'000;

	constexpr uint64 SKILL_TRY_INTERVAL_US = 3'000'000;
	constexpr uint64 BOT_CAST_HOLD_US = 400'000;
	constexpr float SKILL_RANGE = 400.f;
	constexpr float SKILL_RANGE_SQ = SKILL_RANGE * SKILL_RANGE;

	constexpr uint64 DASH_TRY_INTERVAL_US = 5'000'000;
	constexpr uint64 AREA_TRY_INTERVAL_US = 4'000'000;
	constexpr uint64 AREA_CAST_HOLD_US = 60'000;
	constexpr uint64 DASH_CAST_HOLD_US = 100'000;
}

namespace
{
	constexpr uint64 GRANT_GOLD = 100;

	void RunGrantBenchmark(int32 sessionCount, int32 uniqueCount)
	{
		cout << "[GRANT] waiting for " << sessionCount << " session(s) to log in" << endl;

		for (int32 spin = 0; spin < 300; spin++)
		{
			size_t  ready = 0;
			{
				lock_guard<mutex> guard(GBotsLock);
				for (BotSessionRef& bot : GBots)
					if (bot->characterId.load() != 0)
						ready++;
			}

			if (ready >= static_cast<size_t>(sessionCount))
				break;

			this_thread::sleep_for(chrono::milliseconds(100));
		}

		vector<BotSessionRef> snapshot;
		{
			lock_guard<mutex> guard(GBotsLock);
			snapshot = GBots;
		}

		const uint64 targetId = GGrantTargetId.load();

		if (snapshot.empty() || targetId == 0)
		{
			cout << "[GRANT] no session logged in — abort" << endl;
			return;
		}

		const uint64 expectedReplies = static_cast<uint64>(snapshot.size()) * uniqueCount;

		cout << "[GRANT] target character=" << targetId << " sessions=" << snapshot.size() << " unique=" << uniqueCount << " total=" << expectedReplies << endl;

		const uint64 startUs = Utils::NowMicroseconds();

		vector<thread> senders;

		for (BotSessionRef& bot : snapshot)
		{
			senders.emplace_back([bot, uniqueCount, targetId]()
				{
					for (int32 i = 0; i < uniqueCount; i++)
					{
						Protocol::C_GRANT_REWARD pkt;
						pkt.set_request_id("grant-" + to_string(i));
						pkt.set_character_id(targetId);
						pkt.set_gold(GRANT_GOLD);
						pkt.set_reason("bench");

						bot->Send(ClientPacketHandler::MakeSendBuffer(pkt));
					}
				});
		}

		for (thread& t : senders)
			t.join();

		const uint64 sentUs = Utils::NowMicroseconds();
		cout << "[GRANT] all sent in " << (sentUs - startUs) / 1000.0 << " ms — waiting for replies" << endl;

		// 응답을 다 받을 때까지 기다림. 30초를 넘기면 포기하고 현재까지를 보고
		for (int32 spin = 0; spin < 300; spin++)
		{
			if (GGrantReplies.load() >= expectedReplies)
				break;

			this_thread::sleep_for(chrono::milliseconds(100));
		}

		const uint64 endUs = Utils::NowMicroseconds();
		const double elapsedSec = (endUs - startUs) / 1000000.0;

		this_thread::sleep_for(chrono::milliseconds(500));

		const uint64 ok = GGrantOk.load();
		const uint64 already = GGrantAlready.load();
		const uint64 replies = GGrantReplies.load();

		cout << "\n===== GRANT BENCHMARK =====\n"
			<< " sessions      " << snapshot.size() << "\n"
			<< " unique ids    " << uniqueCount << "\n"
			<< " requests sent " << expectedReplies << "\n"
			<< " replies       " << replies
			<< (replies == expectedReplies ? "  (all)" : "  *** MISSING ***") << "\n"
			<< "\n"
			<< " GRANT_OK      " << ok
			<< (ok == static_cast<uint64>(uniqueCount) ? "  PASS" : "  *** FAIL ***") << "\n"
			<< " GRANT_ALREADY " << already << "\n"
			<< " NO_TARGET     " << GGrantNoTarget.load() << "\n"
			<< " BAD_REQUEST   " << GGrantBad.load() << "\n"
			<< " DB_ERROR      " << GGrantError.load() << "\n"
			<< "\n"
			<< " elapsed       " << elapsedSec << " s\n"
			<< " throughput    " << (replies / elapsedSec) << " req/s\n"
			<< " expected gold +" << (static_cast<uint64>(uniqueCount) * GRANT_GOLD) << "\n"
			<< "===========================\n" << endl;
	}
}

atomic<uint64> GAttackSentThisWindow = 0;
atomic<uint64> GSkillSentThisWindow = 0;

static void TickBots(float deltaTime)
{
	vector<BotSessionRef> snapshot;
	{
		lock_guard<mutex> guard(GBotsLock);
		snapshot = GBots;
	}

	const uint64 nowUs = Utils::NowMicroseconds();

	for (BotSessionRef& bot : snapshot)
	{
		if (bot->inGame.load() == false || bot->alive.load() == false)
			continue;

		if (nowUs < bot->hardCcUntilUs.load())
			continue;

		BotSessionRef target = nullptr;
		float bestDistSq = FLT_MAX;

		for (BotSessionRef& other : snapshot)
		{
			if (other == bot)
				continue;
			if (other->inGame.load() == false || other->alive.load() == false)
				continue;

			const float ox = other->x - bot->x;
			const float oy = other->y - bot->y;
			const float distSq = ox * ox + oy * oy;

			//if (distSq < bestDistSq)
			if (target == nullptr || other->objectId.load() < target->objectId.load())
			{
				bestDistSq = distSq;
				target = other;
			}
		}

		if (target != nullptr)
		{
			bot->destX = target->x;
			bot->destY = target->y;
		}

		const float dx = bot->destX - bot->x;
		const float dy = bot->destY - bot->y;
		const float dist = ::sqrtf(dx * dx + dy * dy);

		const bool casting = (nowUs < bot->castUnitlUs);
		const bool dashing = (nowUs < bot->dashUntilUs.load());

		if (dashing)
		{
			const float step = DASH_SPEED * deltaTime;
			bot->x += bot->dashDirX * step;
			bot->y += bot->dashDirY * step;
		}
		else if (casting)
		{

		}
		else if (target == nullptr)
		{
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
		}
		else if (dist > BOT_STOP_DISTANCE)
		{
			const float step = BOT_MOVE_SPEED * deltaTime;
			const float ratio = (step < dist) ? (step / dist) : 1.f;

			bot->x += dx * ratio;
			bot->y += dy * ratio;
		}

		{
			Protocol::C_MOVE pkt;
			{
				Protocol::PosInfo* info = pkt.mutable_info();
				info->set_object_id(bot->objectId.load());
				info->set_x(bot->x);
				info->set_y(bot->y);
				info->set_z(bot->z);
				info->set_yaw(0.f);
				info->set_state(Protocol::MOVE_STATE_RUN);
			}

			bot->Send(ClientPacketHandler::MakeSendBuffer(pkt));
			++GSentThisWindow;
		}

		if (casting == false && dashing == false && nowUs >= bot->nextDashAtUs)
		{
			float dirX = (target != nullptr) ? (target->x - bot->x) : dx;
			float dirY = (target != nullptr) ? (target->y - bot->y) : dy;

			const float len = ::sqrtf(dirX * dirX + dirY * dirY);
			if (len > 1.f)
			{
				dirX /= len;
				dirY /= len;

				Protocol::C_SKILL dashPkt;
				dashPkt.set_slot(Protocol::SLOT_BOOTS);
				dashPkt.set_target_id(0);
				dashPkt.set_aim_x(bot->x + dirX * DASH_DIST);
				dashPkt.set_aim_y(bot->y + dirY * DASH_DIST);

				bot->Send(ClientPacketHandler::MakeSendBuffer(dashPkt));

				bot->dashDirX = dirX;
				bot->dashDirY = dirY;
				bot->nextDashAtUs = nowUs + DASH_TRY_INTERVAL_US;
				bot->castUnitlUs = nowUs + DASH_CAST_HOLD_US;
				++GSkillSentThisWindow;
			}
		}
		else if (casting == false && dashing == false && nowUs >= bot->nextAreaAtUs)
		{
			Protocol::C_SKILL areaPkt;
			areaPkt.set_slot(Protocol::SLOT_ARMOR);
			areaPkt.set_target_id(0);
			areaPkt.set_aim_x(bot->x);
			areaPkt.set_aim_y(bot->y);

			bot->Send(ClientPacketHandler::MakeSendBuffer(areaPkt));

			bot->nextAreaAtUs = nowUs + AREA_TRY_INTERVAL_US;
			bot->dashUntilUs = nowUs + AREA_CAST_HOLD_US;
			++GSkillSentThisWindow;
		}
		else if (target != nullptr && bestDistSq <= SKILL_RANGE_SQ && nowUs >= bot->nextSkillAtUs)
		{
			Protocol::C_SKILL skillPkt;
			skillPkt.set_slot(Protocol::SLOT_WEAPON_PRIMARY);
			skillPkt.set_target_id(target->objectId.load());
			skillPkt.set_aim_x(target->x);
			skillPkt.set_aim_y(target->y);

			bot->Send(ClientPacketHandler::MakeSendBuffer(skillPkt));

			bot->nextSkillAtUs = nowUs + SKILL_TRY_INTERVAL_US;
			bot->castUnitlUs = nowUs + BOT_CAST_HOLD_US;
			++GSkillSentThisWindow;
		}
		else if (target != nullptr && bestDistSq <= ATTACK_RANGE_SQ && nowUs >= bot->nextAttackAtUs)
		{
			Protocol::C_ATTACK attackPkt;
			attackPkt.set_target_id(target->objectId.load());

			bot->Send(ClientPacketHandler::MakeSendBuffer(attackPkt));

			bot->nextAttackAtUs = nowUs + ATTACK_INTERVAL_US;
			++GAttackSentThisWindow;
		}
	}
}

int main(int argc, char* argv[])
{
	int32 botCount = 1;
	int32 grantUnique = 0;

	for (int i = 1; i < argc - 1; i++)
	{
		if (::strcmp(argv[i], "-bots") == 0)
			botCount = ::atoi(argv[i + 1]);
		else if (::strcmp(argv[i], "-grant") == 0)
			grantUnique = ::atoi(argv[i + 1]);
	}
	if (botCount < 1)
		botCount = 1;

	cout << "[BOT] launching " << botCount << " bot(s)" << endl;

	::timeBeginPeriod(1);

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

	if (grantUnique > 0)
	{
		RunGrantBenchmark(botCount, grantUnique);
		return 0;
	}

	uint64 lastUs = Utils::NowMicroseconds();
	uint64 lastReportUs = lastUs;
	uint64 lastRefillUs = lastUs;

	while (true)
	{
		this_thread::sleep_for(chrono::milliseconds(MOVE_INTERVAL_MS));

		const uint64 nowUs = Utils::NowMicroseconds();
		const float deltaTime = static_cast<float>(nowUs - lastUs) / 1000000.f;
		lastUs = nowUs;

		TickBots(deltaTime);

		// 죽어서 빠진 자리 채우기
		if (nowUs - lastRefillUs >= REFILL_INTERVAL_US)
		{
			lastRefillUs = nowUs;

			size_t liveCount = 0;
			{
				lock_guard<mutex> guard(GBotsLock);
				liveCount = GBots.size();
			}

			for (size_t i = liveCount; i < static_cast<size_t>(botCount); i++)
			{
				SessionRef session = service->CreateSession();
				session->Connect();
			}
		}

		if (nowUs - lastReportUs >= 10000000)
		{
			const double sec = static_cast<double>(nowUs - lastReportUs) / 1000000.0;
			const uint64 sent = GSentThisWindow.exchange(0);
			const uint64 attacks = GAttackSentThisWindow.exchange(0);
			const uint64 skills = GSkillSentThisWindow.exchange(0);

			size_t botCount = 0;
			{
				lock_guard<mutex> guard(GBotsLock);
				botCount = GBots.size();
			}

			cout << "[BOT] send rate = "
				<< (botCount > 0 ? sent / sec / botCount : 0.0)
				<< " Hz/bot  (target 30)"
				<< "   attacks = " << (attacks / sec) << "/s"
				<< "   skills = " << (skills / sec) << "/s" << endl;

			lastReportUs = nowUs;
		}
	}

	GThreadManager->Join();
}
