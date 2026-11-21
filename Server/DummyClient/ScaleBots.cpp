#include "pch.h"
#include "ScaleBots.h"
#include "BotSession.h"
#include "BotPath.h"
#include "ClientPacketHandler.h"
#include "Service.h"
#include "ThreadManager.h"

namespace ScaleBots
{
	atomic<bool> GOn = false;

	namespace
	{
		constexpr float		MOVE_SPEED = 340.f;			// BOT_MOVE_SPEED 와 같다 (서버 이동 검증 한도 안)
		// 부하 모양 (docs/server-scaling-10k.md § 1) - 사람처럼 대부분 걷고(봇당 평균 20 Hz 넘게 C_MOVE), 가까이 온 상대와만 싸운다.
		// 예전 하네스처럼 모두가 서로를 쫓으면 몇 초 안에 뭉쳐 서서 때리기만 해 이동 부하가 사라진다
		constexpr float		CHASE_CM = 1200.f;			// 싸우기 국면에서 이 안의 생물을 쫓는다 (시야 15 m 안쪽)
		constexpr float		STOP_CM = 120.f;
		constexpr float		ATTACK_CM = 150.f;
		constexpr float		SKILL_CM = 400.f;
		constexpr uint64	ATTACK_US = 1'000'000;
		constexpr uint64	SKILL_US = 10'000'000;
		// 스킬을 쓴 뒤 이만큼 서 있는다 - 서버 후딜(recovery, 표에서 최대 약 2 s) 동안 걸으면 [MOVE BLOCKED] · 안티치트 move_while_locked.
		// -ai 는 스킬 표로 정확히 맞추지만, 대량 봇은 넉넉히 한 값으로 (UE 클라도 후딜 동안 입력을 막는다)
		constexpr uint64	CAST_HOLD_US = 2'500'000;
		// 국면 - 돌아다니기(8~15 s, 걷기만) ↔ 싸우기(3~6 s, 가까운 상대를 쫓아 때린다). 평균 이동 비율 약 70 %
		constexpr uint64	ROAM_MIN_US = 8'000'000;
		constexpr uint64	ROAM_MAX_US = 15'000'000;
		constexpr uint64	FIGHT_MIN_US = 3'000'000;
		constexpr uint64	FIGHT_MAX_US = 6'000'000;
		constexpr uint64	LEAVE_AFTER_DEATH_US = 3'000'000;	// 죽은 몸을 잠깐 두고 나간다 (UE 클라의 사망 화면)
		constexpr uint64	REENTER_US = 2'000'000;				// 베이스캠프에 머무는 시간 흉내
		constexpr uint64	ENTER_RETRY_US = 5'000'000;			// 입장 응답이 없으면 다시
		constexpr uint64	REJECT_RETRY_US = 3'000'000;
		int32				GTickMs = 33;		// 틱 주기 = max(33 ms, 이동 송신 간격) - 5 Hz 로 보내면서 30 Hz 로 돌 까닭이 없다 (봇 1만 시도 2026-11-22)
		constexpr uint64	REPORT_US = 10'000'000;
		constexpr uint64	PENDING_TIMEOUT_US = 15'000'000;

		ServerStats::Histogram	GRtt;
		atomic<uint64>			GEnters = 0;
		atomic<uint64>			GEnterRejects = 0;
		atomic<uint64>			GDeaths = 0;
		atomic<uint64>			GLoginFails = 0;
		atomic<uint64>			GMoves = 0;
		atomic<uint64>			GAttacks = 0;
		atomic<uint64>			GSkills = 0;
		atomic<uint64>			GKicks[8] = {};
		atomic<uint64>			GTickMaxUs = 0;
		atomic<uint64>			GTickLateUs = 0;		// 33 ms 를 넘긴 틱의 초과 합 - 봇 프로세스가 밀렸나
		atomic<uint64>			GFullUntilUs = 0;		// 서버가 SERVER_FULL 로 끊었다 - 이 시각까지 새 접속을 걸지 않는다 (사람 클라의 재시도 간격 흉내)

		struct BotTarget
		{
			uint64	id = 0;
			float	x = 0.f;
			float	y = 0.f;
			float	distSq = 0.f;
		};

		void SendEnter(const BotSessionRef& bot, uint64 nowUs)
		{
			Protocol::C_ENTER_GAME enter;
			enter.set_playerindex(0);
			enter.set_floor_id(bot->floorId);
			enter.set_entry_point_id(0);
			enter.set_mode(Protocol::RUN_MODE_PVP);
			bot->scaleEnterSentUs = nowUs;
			bot->Send(ClientPacketHandler::MakeSendBuffer(enter));
		}

		BotTarget PickTarget(const BotSessionRef& bot)
		{
			BotTarget best;
			best.distSq = CHASE_CM * CHASE_CM;
			lock_guard<mutex> guard(bot->itemLock);
			for (const auto& [id, c] : bot->ai.creatures)
			{
				if (c.ally || c.hp <= 0)
					continue;
				const float dx = c.x - bot->x;
				const float dy = c.y - bot->y;
				const float d2 = dx * dx + dy * dy;
				if (d2 < best.distSq)
				{
					best.id = id;
					best.x = c.x;
					best.y = c.y;
					best.distSq = d2;
				}
			}
			if (best.id == 0)
				best.distSq = 0.f;
			return best;
		}

		// 이 걸음이 서버에서 통과하는가 - 벽이 있는 층(0층)만 BotPath 로 본다. 다른 층은 벽이 없고 입장 지점 둘레만 걷는다
		bool StepOk(const BotSessionRef& bot, float x1, float y1)
		{
			if (BotPath::IsLoaded() == false || bot->floorId != BotPath::LoadedFloor())
				return true;
			return BotPath::SegmentClear(bot->x, bot->y, x1, y1) && BotPath::Walkable(x1, y1);
		}

		float GWanderCm = 1500.f;

		// 배회 목표 - 벽이 있는 층(0층)이면 지금 자리에서 목표까지 직선이 트인 자리만 고른다 (2026-11-22).
		// 예전엔 걸음마다 벽 · 해안선을 검사하고 막히면 매 틱 다시 골라, 봇 1만에서 봇 한 명 틱이 600 µs 를 넘었다.
		// 다리 하나를 한 번 검사하면 그 위의 걸음은 검사할 필요가 없다
		void PickWanderGoal(const BotSessionRef& bot)
		{
			const bool checked = BotPath::IsLoaded() && bot->floorId == BotPath::LoadedFloor();
			for (int32 attempt = 0; attempt < 8; attempt++)
			{
				const float radius = (attempt < 5) ? GWanderCm : 500.f;		// 멀리가 안 되면 가까이
				bot->destX = bot->anchorX + Utils::GetRandom(-radius, radius);
				bot->destY = bot->anchorY + Utils::GetRandom(-radius, radius);
				if (attempt >= 5)
				{
					bot->destX = bot->x + Utils::GetRandom(-radius, radius);
					bot->destY = bot->y + Utils::GetRandom(-radius, radius);
				}
				if (checked == false || (BotPath::Walkable(bot->destX, bot->destY) && BotPath::SegmentClear(bot->x, bot->y, bot->destX, bot->destY)))
				{
					bot->scaleLegClear = true;
					return;
				}
			}
			bot->destX = bot->x;		// 트인 곳이 없다 - 이번엔 서 있는다 (다음 틱에 다시 고른다)
			bot->destY = bot->y;
			bot->scaleLegClear = true;
		}

		void TickBot(const BotSessionRef& bot, uint64 nowUs, float dt, const Options& o)
		{
			if (bot->IsConnected() == false || bot->loggedIn.load() == false)
				return;

			// 하트비트 - 베이스캠프에서도 (서버 유휴 끊기 30 s). 지난 왕복을 실어 서버가 지연 분포를 모은다
			if (nowUs >= bot->scaleNextHbUs)
			{
				bot->scaleNextHbUs = nowUs + static_cast<uint64>(o.heartbeatMs) * 1000;
				// 보내는 순간의 시각 - 틱 시작 시각(nowUs)을 쓰면 틱 안에서 뒤에 도는 봇일수록 그 틱 처리 시간만큼 왕복이 부풀었다
				const uint64 sendUs = Utils::NowMicroseconds();
				Protocol::C_HEARTBEAT hb;
				hb.set_client_ms(sendUs / 1000);
				hb.set_client_us(sendUs);
				hb.set_last_rtt_us(bot->lastRttUs.exchange(0));
				bot->Send(ClientPacketHandler::MakeSendBuffer(hb));
			}

			if (bot->inGame.load() == false)
			{
				const uint64 at = bot->reenterAtUs.load();
				if (at != 0 && nowUs >= at)
				{
					bot->reenterAtUs.store(0);
					SendEnter(bot, nowUs);
				}
				else if (at == 0 && bot->scaleEnterSentUs != 0 && nowUs - bot->scaleEnterSentUs > ENTER_RETRY_US)
				{
					SendEnter(bot, nowUs);
				}
				return;
			}

			// 죽었다 - 잠깐 뒤 나간다. 서버가 베이스캠프로 돌려놓으면 다시 들어간다 (사망 커밋 중이면 입장이 거절되고 다시 시도)
			if (bot->alive.load() == false)
			{
				if (bot->scaleLeaveSentUs == 0 && nowUs - bot->diedAtUs.load() >= LEAVE_AFTER_DEATH_US)
				{
					bot->scaleLeaveSentUs = nowUs;
					Protocol::C_LEAVE_GAME leave;
					bot->Send(ClientPacketHandler::MakeSendBuffer(leave));
					bot->inGame.store(false);
					bot->reenterAtUs.store(nowUs + REENTER_US);
				}
				return;
			}

			// 기절 · 속박 · 넉백 · 도발 - 사람 클라처럼 입력하지 않는다 (서버가 [MOVE BLOCKED] 로 막고 위치는 보정으로 준다)
			if (nowUs < bot->hardCcUntilUs.load())
			{
				bot->scaleWasMoving = false;
				return;
			}

			const bool casting = nowUs < bot->castUnitlUs;

			if (nowUs >= bot->scalePhaseUntilUs)
			{
				bot->scaleFighting = (bot->scalePhaseUntilUs != 0) && (bot->scaleFighting == false) && o.combat;
				bot->scalePhaseUntilUs = nowUs + (bot->scaleFighting ? Utils::GetRandom(FIGHT_MIN_US, FIGHT_MAX_US) : Utils::GetRandom(ROAM_MIN_US, ROAM_MAX_US));
			}

			BotTarget target;
			if (bot->scaleFighting)
				target = PickTarget(bot);

			const float fromX = bot->x;
			const float fromY = bot->y;
			if (casting == false)
			{
				// 쫓다가 돌아왔으면 지금 자리에서 다리를 새로 고른다 (옛 목표까지 직선이 트였는지 모른다)
				if (target.id == 0 && bot->scaleLegClear == false)
					PickWanderGoal(bot);

				float goalX = bot->destX;
				float goalY = bot->destY;
				float stopAt = 20.f;
				if (target.id != 0)
				{
					goalX = target.x;
					goalY = target.y;
					stopAt = STOP_CM;
					bot->scaleLegClear = false;
				}

				const float dx = goalX - bot->x;
				const float dy = goalY - bot->y;
				const float dist = ::sqrtf(dx * dx + dy * dy);
				// 도착 판정에 1 cm 여유 - 걸음이 정확히 stopAt 에 멈추면 "도착"도 "더 가기"도 아니어서 봇이 영원히 서 있었다
				if (target.id == 0 && dist <= stopAt + 1.f)
				{
					PickWanderGoal(bot);
				}
				else if (dist > stopAt)
				{
					const float step = (std::min)(MOVE_SPEED * dt, dist - stopAt);
					const float nx = bot->x + dx / dist * step;
					const float ny = bot->y + dy / dist * step;
					// 배회 다리는 고를 때 검사했다. 쫓기(상대 자리)만 걸음마다 본다
					if ((target.id == 0 && bot->scaleLegClear) || StepOk(bot, nx, ny))
					{
						bot->x = nx;
						bot->y = ny;
					}
					else
					{
						PickWanderGoal(bot);
					}
				}
			}

			// 움직였을 때만 보낸다 (멈춘 순간 IDLE 한 번) - UE 클라도 서 있으면 이동을 보내지 않는다. 살아 있음은 하트비트가 알린다
			const bool moved = (bot->x != fromX || bot->y != fromY);
			// -movehz - 걷는 동안 보내는 빈도. 그 사이 걸음은 다음 패킷에 쌓인다 (서버 이동 검증은 client_ms 간격으로 잰다)
			const bool due = (nowUs - bot->scaleLastMoveSentUs) >= 1'000'000ull / static_cast<uint64>(o.moveHz) - 2'000;
			if ((moved && due) || (moved == false && bot->scaleWasMoving))
			{
				bot->scaleWasMoving = moved;
				bot->scaleLastMoveSentUs = nowUs;
				Protocol::C_MOVE pkt;
				Protocol::PosInfo* info = pkt.mutable_info();
				info->set_object_id(bot->objectId.load());
				info->set_x(bot->x);
				info->set_y(bot->y);
				info->set_z(bot->z);
				info->set_yaw(moved ? ::atan2f(bot->y - fromY, bot->x - fromX) * 57.2958f : 0.f);
				info->set_state(moved ? Protocol::MOVE_STATE_RUN : Protocol::MOVE_STATE_IDLE);
				pkt.set_client_ms(nowUs / 1000);
				bot->Send(ClientPacketHandler::MakeSendBuffer(pkt));
				++GMoves;
			}

			if (target.id == 0 || casting)
				return;

			if (target.distSq <= SKILL_CM * SKILL_CM && nowUs >= bot->scaleNextSkillUs)
			{
				Protocol::C_SKILL skill;
				skill.set_slot(Protocol::SLOT_WEAPON_PRIMARY);
				skill.set_target_id(target.id);
				skill.set_aim_x(target.x);
				skill.set_aim_y(target.y);
				bot->Send(ClientPacketHandler::MakeSendBuffer(skill));
				bot->scaleNextSkillUs = nowUs + SKILL_US + Utils::GetRandom<uint64>(0, 4'000'000);
				bot->castUnitlUs = nowUs + CAST_HOLD_US;
				++GSkills;
			}
			else if (target.distSq <= ATTACK_CM * ATTACK_CM && nowUs >= bot->scaleNextAttackUs)
			{
				Protocol::C_ATTACK attack;
				attack.set_target_id(target.id);
				bot->Send(ClientPacketHandler::MakeSendBuffer(attack));
				bot->scaleNextAttackUs = nowUs + ATTACK_US;
				++GAttacks;
			}
		}

		// 틱 스레드 하나 - botIndex % tickThreads == shard 인 봇만. 목록은 1 초마다 새로 뜬다 (매 틱 1만 개 shared_ptr 를 복사하지 않게)
		void TickLoop(int32 shard, Options o)
		{
			vector<BotSessionRef> mine;
			uint64 lastRefreshUs = 0;
			uint64 lastUs = Utils::NowMicroseconds();
			uint64 nextUs = lastUs;

			while (true)
			{
				const uint64 nowUs = Utils::NowMicroseconds();
				if (nowUs - lastRefreshUs >= 1'000'000)
				{
					lastRefreshUs = nowUs;
					mine.clear();
					lock_guard<mutex> guard(GBotsLock);
					for (const BotSessionRef& bot : GBots)
						if (bot->botIndex != 0 && static_cast<int32>(bot->botIndex % o.tickThreads) == shard)
							mine.push_back(bot);
				}

				const float dt = (std::min)(0.5f, static_cast<float>(nowUs - lastUs) / 1'000'000.f);
				lastUs = nowUs;

				for (const BotSessionRef& bot : mine)
					TickBot(bot, nowUs, dt, o);

				const uint64 doneUs = Utils::NowMicroseconds();
				const uint64 tookUs = doneUs - nowUs;
				uint64 prev = GTickMaxUs.load();
				while (tookUs > prev && GTickMaxUs.compare_exchange_weak(prev, tookUs) == false) {}
				if (tookUs > static_cast<uint64>(GTickMs) * 1000)
					GTickLateUs.fetch_add(tookUs - static_cast<uint64>(GTickMs) * 1000);

				nextUs += static_cast<uint64>(GTickMs) * 1000;
				if (nextUs <= doneUs)
					nextUs = doneUs;		// 밀렸으면 따라잡으려 몰아 돌지 않는다
				else
					this_thread::sleep_for(chrono::microseconds(nextUs - doneUs));
			}
		}
	}

	void RecordRtt(uint64 micros) { GRtt.Record(micros); }
	void CountEnter() { ++GEnters; }
	void CountEnterReject() { ++GEnterRejects; }
	void CountDeath() { ++GDeaths; }
	void CountKick(int32 reason)
	{
		GKicks[(reason >= 0 && reason < 8) ? reason : 0].fetch_add(1);
		if (reason == Protocol::KICK_REASON_SERVER_FULL)
			GFullUntilUs.store(Utils::NowMicroseconds() + 2'000'000);
	}
	void CountLoginFail() { ++GLoginFails; }

	void Run(ClientServiceRef service, const Options& o)
	{
		GWanderCm = o.wanderCm;
		GTickMs = (std::max)(33, 1000 / (std::max)(1, o.moveHz));
		for (int32 t = 0; t < o.tickThreads; t++)
			GThreadManager->Launch([t, o]() { TickLoop(t, o); });

		// 걸어 둔 접속 - 연결되면(IsConnected) 또는 실패로 사라지면(weak 만료) 빠진다
		vector<pair<weak_ptr<Session>, uint64>> pending;
		double budget = 0.0;
		uint64 lastUs = Utils::NowMicroseconds();
		uint64 lastReportUs = lastUs;
		uint64 startUs = lastUs;

		while (true)
		{
			this_thread::sleep_for(chrono::milliseconds(100));
			const uint64 nowUs = Utils::NowMicroseconds();
			budget = (std::min)(budget + o.connectPerSec * (nowUs - lastUs) / 1'000'000.0, static_cast<double>(o.connectPerSec));
			lastUs = nowUs;

			pending.erase(remove_if(pending.begin(), pending.end(), [nowUs](const pair<weak_ptr<Session>, uint64>& p)
				{
					SessionRef s = p.first.lock();
					return s == nullptr || s->IsConnected() || nowUs - p.second > PENDING_TIMEOUT_US;
				}), pending.end());

			size_t live = 0;
			{
				lock_guard<mutex> guard(GBotsLock);
				live = GBots.size();
			}

			int32 want = o.botCount - static_cast<int32>(live) - static_cast<int32>(pending.size());
			if (nowUs < GFullUntilUs.load())
				want = 0;		// 서버가 가득 찼다 - 2 초 쉬었다 다시
			while (want > 0 && budget >= 1.0)
			{
				SessionRef session = service->CreateSession();
				if (session == nullptr || session->Connect() == false)
					break;
				pending.emplace_back(session, nowUs);
				budget -= 1.0;
				want--;
			}

			if (nowUs - lastReportUs < REPORT_US)
				continue;

			const double sec = (nowUs - lastReportUs) / 1'000'000.0;
			lastReportUs = nowUs;

			int32 inGame = 0;
			int32 loggedIn = 0;
			{
				lock_guard<mutex> guard(GBotsLock);
				live = GBots.size();
				for (const BotSessionRef& bot : GBots)
				{
					loggedIn += bot->loggedIn.load() ? 1 : 0;
					inGame += (bot->inGame.load() && bot->alive.load()) ? 1 : 0;
				}
			}

			const ServerStats::Histogram::Drained rtt = GRtt.Drain();
			auto ms = [](uint64 us) { return us / 1000.0; };
			char line[512];
			::snprintf(line, sizeof(line),
				"[SCALE] t=%llus bots=%zu/%d login=%d in_game=%d pending=%zu | rtt n=%llu p50=%.1fms p95=%.1fms p99=%.1fms max=%.1fms"
				" | enter=%.1f/s reject=%.1f/s death=%.1f/s login_fail=%llu kick(full=%llu cheat=%llu ban=%llu dup=%llu shut=%llu)"
				" | move=%.0f/s atk=%.1f/s skill=%.1f/s | tick_max=%.1fms late=%.1fms\n",
				(nowUs - startUs) / 1'000'000, live, o.botCount, loggedIn, inGame, pending.size(),
				rtt.count, ms(rtt.p50Us), ms(rtt.p95Us), ms(rtt.p99Us), ms(rtt.maxUs),
				GEnters.exchange(0) / sec, GEnterRejects.exchange(0) / sec, GDeaths.exchange(0) / sec, GLoginFails.load(),
				GKicks[Protocol::KICK_REASON_SERVER_FULL].load(), GKicks[Protocol::KICK_REASON_CHEAT_SUSPECT].load(), GKicks[Protocol::KICK_REASON_BANNED].load(),
				GKicks[Protocol::KICK_REASON_DUPLICATE_LOGIN].load(), GKicks[Protocol::KICK_REASON_SERVER_SHUTDOWN].load(),
				GMoves.exchange(0) / sec, GAttacks.exchange(0) / sec, GSkills.exchange(0) / sec,
				ms(GTickMaxUs.exchange(0)), ms(GTickLateUs.exchange(0)));
			LOG_LINE << line;
		}
	}
}
