#include "pch.h"
#include <iostream>
#include <timeapi.h>
#include "ThreadManager.h"
#include "Service.h"
#include "Session.h"
#include "ClientPacketHandler.h"
#include "BotSession.h"
#include "BotPath.h"
#include "BotAiDriver.h"

#pragma comment(lib, "winmm.lib")

mutex GBotsLock;
vector<BotSessionRef> GBots;
vector<uint32> GFreeBotIndices;
std::atomic<uint64> GSentThisWindow{ 0 };
std::atomic<uint64> GDetoursThisWindow{ 0 };		// 하네스 걸음이 벽 · 바다를 만나 길을 구한 수 (2026-10-18)
atomic<int32> GEnhance = 0;
atomic<int32> GPvpParty = 0;
atomic<uint64> GBetrayAfterUs = 0;
atomic<uint64> GKickAfterUs = 0;
atomic<bool> GEnhanceDup = false;

atomic<bool> GItemBehavior = false;
atomic<bool> GNoClientMs = false;
atomic<bool> GBurst = false;
atomic<bool> GMonsterPull = false;
atomic<uint32> GEntryPointId = 0;
atomic<uint64> GMonsterKills = 0;
atomic<uint64> GMonsterAttacksSent = 0;
atomic<bool> GTowerMode = false;
atomic<bool> GIdleOne = false;
atomic<bool> GIdleAll = false;
atomic<int32> GForceFloor = -1;
int32 GBotBase = 0;
bool GRawWalk = false;	// -rawwalk 1: 하네스 걸음이 벽을 모르던 예전 방식 (비교용, 2026-10-18)		// -botbase N: bot_{N+1} 부터 쓴다 - 두 하네스를 함께 돌릴 때 계정이 겹치지 않게 (2026-10-18 PVE 40 판 + PvP 120)
float GTowerX = 0.f;
float GTowerY = 0.f;
atomic<uint64> GRunGold = 0;
string GEnterChat;
atomic<uint32> GChatAfterMs = 0;
atomic<bool> GSwapArmor = false;
atomic<bool> GStayDead = false;
atomic<bool> GHoldOnReject = false;
atomic<int32> GRunMode = 0;
atomic<uint64> GCampEnterAfterUs = 0;
atomic<int32> GPartySize = 3;
atomic<bool> GAllyFire = false;
atomic<int32> GTowerCount = 0;
atomic<uint64> GLeaveEscaped = 0;
atomic<uint64> GLeaveExpired = 0;
uint64 GBurstStartUs = 0;
atomic<uint64> GBagOpenSent = 0;
atomic<uint64> GLootSent = 0;
atomic<uint64> GEquipSent = 0;
atomic<uint64> GEscapeSent = 0;
atomic<uint64> GReenterSent = 0;
atomic<uint64> GItemOk = 0;
atomic<uint64> GItemFail = 0;

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
	constexpr float WAYPOINT_REACHED_CM = 5.f;	// -tower 길찾기 - 이 안이면 다음 꺾는 점으로 (한 틱 이동은 이 점에서 멈춘다)
	constexpr float DETOUR_REACHED_CM = 30.f;	// 하네스 걸음 우회 - 꺾는 점 이 안이면 다음 점 (2026-10-18)
	constexpr float DETOUR_GOAL_SLACK_CM = 300.f;	// 목표가 이만큼 움직이면 우회 길을 버린다
	constexpr uint64 DETOUR_REPLAN_US = 1'000'000;	// 우회 길을 다시 구하는 간격 (봇마다)
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

	// 아이템 행동 (-items). 서버 줍기 사거리는 200 cm (Room.cpp LOOT_RANGE_CM) - 이동 오차만큼 안쪽에서 연다
	constexpr float BOT_LOOT_RANGE = 180.f;
	constexpr float BOT_LOOT_RANGE_SQ = BOT_LOOT_RANGE * BOT_LOOT_RANGE;
	constexpr uint64 LOOT_TRY_INTERVAL_US = 2'000'000;
	constexpr uint64 EQUIP_TRY_INTERVAL_US = 1'500'000;
}

namespace
{
	// -burst: 20초마다 1.5초 동안 모든 봇이 이동을 쥐고 있다가 한꺼번에 보낸다. 같은 PC 경합으로 봇 프로세스가
	// 멈췄다 몰아 보낸 G2 soak2 의 SV-3 109건을 일부러 만든다 (roadmap.md § 0-11 후속)
	constexpr uint64 BURST_PERIOD_US = 20'000'000;
	constexpr uint64 BURST_HOLD_US = 1'500'000;

	bool InBurstHold(uint64 nowUs)
	{
		if (GBurst.load() == false || nowUs < GBurstStartUs)
			return false;

		return ((nowUs - GBurstStartUs) % BURST_PERIOD_US) >= (BURST_PERIOD_US - BURST_HOLD_US);
	}

	// -burst 동안에는 그 봇의 모든 패킷을 순서대로 쥔다 - 멈춘 프로세스 · TCP 처럼 스킬과 이동이 함께 늦는다.
	// 이동만 쥐면 대시 스킬이 먼저 도착해 서버의 대시 창이 이동보다 먼저 끝나는, 실제로는 없는 순서가 생긴다
	void SendOrHold(const BotSessionRef& bot, SendBufferRef buffer, uint64 nowUs)
	{
		if (InBurstHold(nowUs))
			bot->heldPackets.push_back(buffer);
		else
			bot->Send(buffer);
	}

	// 근처 가방을 하나 연다. 연 가방은 잊는다 - 같은 빈 가방을 2초마다 다시 여는 일이 없게(사람도 그렇다)
	void TryOpenBag(const BotSessionRef& bot, uint64 nowUs)
	{
		if (nowUs < bot->nextLootAtUs)
			return;

		bot->nextLootAtUs = nowUs + LOOT_TRY_INTERVAL_US;

		uint64 bagId = 0;
		{
			lock_guard<mutex> guard(bot->itemLock);

			// 열기가 거절되면(사거리 · 이미 사라짐) 내용이 오지 않는다 - 다음 시도에서 기다림을 푼다
			bot->openBagId = 0;

			int32 carried = 0;
			for (const auto& [id, item] : bot->items)
			{
				if (item.state() == Protocol::ITEM_STATE_CARRIED)
					carried++;
			}

			if (carried >= BOT_INVENTORY_SLOTS)
				return;

			float bestSq = BOT_LOOT_RANGE_SQ;
			for (const auto& [id, pos] : bot->bags)
			{
				const float dx = pos.first - bot->x;
				const float dy = pos.second - bot->y;
				const float distSq = dx * dx + dy * dy;
				if (distSq <= bestSq)
				{
					bestSq = distSq;
					bagId = id;
				}
			}

			if (bagId == 0)
				return;

			bot->bags.erase(bagId);
			bot->openBagId = bagId;
		}

		Protocol::C_BAG_OPEN openPkt;
		openPkt.set_bag_id(bagId);
		SendOrHold(bot, ClientPacketHandler::MakeSendBuffer(openPkt), nowUs);
		GBagOpenSent.fetch_add(1);
	}

	// 빈 부위가 있거나 더 높은 등급이면 입는다. 부위는 종류 id 의 백의 자리다 (Server/Data/item_types.csv)
	void TryEquip(const BotSessionRef& bot, uint64 nowUs)
	{
		if (nowUs < bot->nextEquipAtUs)
			return;

		bot->nextEquipAtUs = nowUs + EQUIP_TRY_INTERVAL_US;

		uint64 instanceId = 0;
		int32 slot = 0;
		{
			lock_guard<mutex> guard(bot->itemLock);

			int32 wornGrade[Protocol::SLOT_TRINKET + 1] = {};
			for (const auto& [id, item] : bot->items)
			{
				if (item.state() == Protocol::ITEM_STATE_EQUIPPED && item.slot() <= Protocol::SLOT_TRINKET)
					wornGrade[item.slot()] = item.grade();
			}

			int32 bestGrade = 0;
			for (const auto& [id, item] : bot->items)
			{
				if (item.state() != Protocol::ITEM_STATE_CARRIED)
					continue;

				const int32 part = static_cast<int32>(item.item_type_id() / 100);
				if (part < Protocol::SLOT_WEAPON_PRIMARY || part > Protocol::SLOT_TRINKET || part == Protocol::SLOT_WEAPON_SECONDARY)
					continue;

				if (item.grade() <= wornGrade[part] || item.grade() <= bestGrade)
					continue;

				bestGrade = item.grade();
				instanceId = id;
				slot = part;
			}
		}

		if (instanceId == 0)
			return;

		Protocol::C_EQUIP equipPkt;
		equipPkt.set_request_id(bot->NextRequestId());
		equipPkt.set_instance_id(instanceId);
		equipPkt.set_slot(static_cast<Protocol::EquipSlot>(slot));
		SendOrHold(bot, ClientPacketHandler::MakeSendBuffer(equipPkt), nowUs);
		GEquipSent.fetch_add(1);
	}

	// -swap: 입장 3 초 뒤 갑옷을 벗고, 2 초 뒤 같은 갑옷을 다시 입는다. 서버 [STATS] unequip · equip 줄과
	// [BOT HIT] dmg=0 (체력 재동기화) 으로 장비 스탯 · 체력 비율 유지가 던전 안에서 도는지 본다 (2026-10-07)
	void TrySwapArmor(const BotSessionRef& bot, uint64 nowUs)
	{
		const uint64 at = bot->swapAtUs.load();
		if (at == 0 || nowUs < at)
			return;

		if (bot->swapStep == 0)
		{
			uint64 armorId = 0;
			{
				lock_guard<mutex> guard(bot->itemLock);
				for (const auto& [id, item] : bot->items)
				{
					if (item.state() == Protocol::ITEM_STATE_EQUIPPED && item.slot() == Protocol::SLOT_ARMOR)
						armorId = id;
				}
			}

			bot->swapAtUs.store(0);
			if (armorId == 0)
				return;

			Protocol::C_UNEQUIP pkt;
			pkt.set_request_id(bot->NextRequestId());
			pkt.set_instance_id(armorId);
			bot->Send(ClientPacketHandler::MakeSendBuffer(pkt));

			bot->swapItemId = armorId;
			bot->swapStep = 1;
			bot->swapAtUs.store(nowUs + 2'000'000);
			LOG_LINE << "[BOT SWAP] unequip char=" << bot->objectId.load() << " item=" << armorId << '\n';
			return;
		}

		if (bot->swapStep == 1)
		{
			Protocol::C_EQUIP pkt;
			pkt.set_request_id(bot->NextRequestId());
			pkt.set_instance_id(bot->swapItemId);
			pkt.set_slot(Protocol::SLOT_ARMOR);
			bot->Send(ClientPacketHandler::MakeSendBuffer(pkt));

			bot->swapStep = 0;
			bot->swapAtUs.store(0);
			LOG_LINE << "[BOT SWAP] equip char=" << bot->objectId.load() << " item=" << bot->swapItemId << '\n';
		}
	}

	void TryEscape(const BotSessionRef& bot, uint64 nowUs)
	{
		{
			lock_guard<mutex> guard(bot->itemLock);
			if (bot->escapeAtUs == 0 || nowUs < bot->escapeAtUs)
				return;

			bot->escapeAtUs = 0;
		}

		// 탈출 구역은 P4 범위라 아직 없다 - 디버그 채팅 명령이 같은 커밋 경로(BeginEscapeCommit)를 탄다
		Protocol::C_CHAT chatPkt;
		chatPkt.set_msg("/escape");
		SendOrHold(bot, ClientPacketHandler::MakeSendBuffer(chatPkt), nowUs);
		GEscapeSent.fetch_add(1);
	}
}

namespace
{
	constexpr uint64 GRANT_GOLD = 100;

	void RunGrantBenchmark(int32 sessionCount, int32 uniqueCount, bool spread)
	{
		LOG_LINE << "[GRANT] waiting for " << sessionCount << " session(s) to log in" << '\n';

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
			LOG_LINE << "[GRANT] no session logged in — abort" << '\n';
			return;
		}

		vector<uint64> targets;

		if (spread)
		{
			for (BotSessionRef& bot : snapshot)
			{
				const uint64 id = bot->characterId.load();
				if (id != 0)
					targets.push_back(id);
			}
		}

		if (targets.empty())
			targets.push_back(targetId);

		const uint64 expectedReplies = static_cast<uint64>(snapshot.size()) * uniqueCount;

		LOG_LINE << "[GRANT] target character=" << targetId << " sessions=" << snapshot.size() << " unique=" << uniqueCount << " total=" << expectedReplies << '\n';

		const uint64 startUs = Utils::NowMicroseconds();

		vector<thread> senders;

		for (BotSessionRef& bot : snapshot)
		{
			senders.emplace_back([bot, uniqueCount, &targets]()
				{
					for (int32 i = 0; i < uniqueCount; i++)
					{
						Protocol::C_GRANT_REWARD pkt;
						pkt.set_request_id("grant-" + to_string(i));
						pkt.set_character_id(targets[i%targets.size()]);
						pkt.set_gold(GRANT_GOLD);
						pkt.set_reason("bench");

						bot->Send(ClientPacketHandler::MakeSendBuffer(pkt));
					}
				});
		}

		for (thread& t : senders)
			t.join();

		const uint64 sentUs = Utils::NowMicroseconds();
		LOG_LINE << "[GRANT] all sent in " << (sentUs - startUs) / 1000.0 << " ms — waiting for replies" << '\n';

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

		LOG_LINE << "\n===== GRANT BENCHMARK =====\n"
			<< " sessions      " << snapshot.size() << "\n"
			<< " targets       " << targets.size()
			<< (targets.size() == 1 ? "  (집중)" : "  (분산)") << "\n"
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
			<< "===========================\n" << '\n';
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

	const int32 runMode = GRunMode.load();

	for (BotSessionRef& bot : snapshot)
	{
		// 협동 - 번호로 나눈 첫 봇이 아직 안 들어온 묶음 봇을 초대한다 (로그인 순서가 섞여 NOT_FOUND 면 2 s 뒤 다시)
		if (runMode == Protocol::RUN_MODE_PVE_COOP && bot->inGame.load() == false)
		{
			const uint64 inviteAt = bot->nextInviteAtUs.load();
			const int32 size = (std::max)(2, (std::min)(3, GPartySize.load()));
			size_t members = 0;
			{
				lock_guard<mutex> guard(bot->partyLock);
				members = bot->partyMembers.size();
			}
			if (inviteAt != 0 && nowUs >= inviteAt && static_cast<int32>(members) < size)
			{
				bot->nextInviteAtUs.store(nowUs + 2'000'000);
				for (int32 k = 1; k < size; k++)
				{
					Protocol::C_PARTY_INVITE invite;
					invite.set_target_name("bot_" + to_string(bot->botIndex + k));
					bot->Send(ClientPacketHandler::MakeSendBuffer(invite));
				}
			}
		}

		// -pvpparty N: 1 번 봇이 입장 3 s 뒤 같은 진입 지점의 bot_2..bot_N 을 이름으로 초대 (party-betrayal R2) · -betrayafter s: 2 번 봇이 s 초 뒤 나간다 (= 배신, R5)
		if (bot->inGame.load() && bot->enteredAtUs != 0)
		{
			const int32 pvpParty = GPvpParty.load();
			if (pvpParty > 1 && bot->botIndex == 1 && bot->pvpInviteSent == false && nowUs >= bot->enteredAtUs + 3'000'000)
			{
				bot->pvpInviteSent = true;
				for (int32 k = 2; k <= pvpParty; k++)
				{
					Protocol::C_PARTY_INVITE invite;
					invite.set_target_name("bot_" + to_string(k));
					bot->Send(ClientPacketHandler::MakeSendBuffer(invite));
				}
				LOG_LINE << "[BOT PARTY] pvp invites sent by idx=1 to bot_2..bot_" << pvpParty << '\n';
			}
			const uint64 betrayAfter = GBetrayAfterUs.load();
			if (betrayAfter > 0 && bot->botIndex == 2 && bot->betraySent == false && nowUs >= bot->enteredAtUs + betrayAfter)
			{
				bot->betraySent = true;
				Protocol::C_PARTY_LEAVE leave;
				leave.set_character_id(bot->characterId.load());
				bot->Send(ClientPacketHandler::MakeSendBuffer(leave));
				LOG_LINE << "[BOT PARTY] idx=2 leaves the party inside the dungeon (betrayal) char=" << bot->characterId.load() << '\n';
			}
			// -kickafter s: 1 번 봇(파티장)이 s 초 뒤 첫 파티원을 내보내려 한다 - 던전 파티는 PARTY_NO_KICK_IN_RUN 으로 거절돼야 하고 파티는 그대로여야 한다 (R4 · AC-PB-05)
			const uint64 kickAfter = GKickAfterUs.load();
			if (kickAfter > 0 && bot->botIndex == 1 && bot->kickSent == false && nowUs >= bot->enteredAtUs + kickAfter)
			{
				uint64 victim = 0;
				{
					lock_guard<mutex> guard(bot->partyLock);
					for (const uint64 id : bot->partyMembers)
						if (id != bot->characterId.load()) { victim = id; break; }
				}
				if (victim != 0)
				{
					bot->kickSent = true;
					Protocol::C_PARTY_LEAVE kick;
					kick.set_character_id(victim);
					bot->Send(ClientPacketHandler::MakeSendBuffer(kick));
					LOG_LINE << "[BOT PARTY] idx=1 kicks char=" << victim << " inside the dungeon (expect PARTY_NO_KICK_IN_RUN)\n";
				}
			}
		}

		// -mode camp -campenter s: 베이스캠프 맵에서 s 초 놀다가 PVE 솔로로 들어간다 - 서버가 캠프 몸을 빼는지 (basecamp-map AC-BC-07)
		if (runMode == Protocol::RUN_MODE_BASECAMP && GCampEnterAfterUs.load() > 0 && bot->inGame.load() && bot->campEnterSent == false
			&& bot->enteredAtUs != 0 && nowUs >= bot->enteredAtUs + GCampEnterAfterUs.load())
		{
			bot->campEnterSent = true;
			Protocol::C_ENTER_GAME enterPkt;
			enterPkt.set_playerindex(0);
			enterPkt.set_floor_id(0);
			enterPkt.set_entry_point_id(GEntryPointId.load());
			enterPkt.set_mode(Protocol::RUN_MODE_PVE_SOLO);
			bot->Send(ClientPacketHandler::MakeSendBuffer(enterPkt));
			LOG_LINE << "[BOT CAMP] idx=" << bot->botIndex << " leaves the camp for PVE solo\n";
		}

		// 탈출해 베이스캠프에 있는 봇 - 잠깐 뒤 같은 층으로 다시 들어간다 (협동은 파티장이 파티 상태를 보고 연다)
		const uint64 reenterAt = bot->reenterAtUs.load();
		if (reenterAt != 0 && nowUs >= reenterAt && bot->inGame.load() == false && bot->alive.load())
		{
			bot->reenterAtUs.store(0);
			if (runMode == Protocol::RUN_MODE_PVE_COOP)
				continue;

			Protocol::C_ENTER_GAME enterPkt;
			enterPkt.set_playerindex(0);
			enterPkt.set_floor_id(runMode == Protocol::RUN_MODE_PVE_SOLO ? 0 : bot->floorId);
			enterPkt.set_entry_point_id(GEntryPointId.load());
			enterPkt.set_mode(static_cast<Protocol::RunMode>(runMode));
			bot->Send(ClientPacketHandler::MakeSendBuffer(enterPkt));
			GReenterSent.fetch_add(1);
			continue;
		}

		if (bot->inGame.load() == false)
			continue;

		// 죽은 몸은 움직이지 않는다. -staydead -swap 이면 갑옷 교체 요청만 보내 서버가 거절하는지 본다
		if (bot->alive.load() == false)
		{
			if (GStayDead.load() && GSwapArmor.load())
				TrySwapArmor(bot, nowUs);
			continue;
		}

		// 쥐고 있던 이동을 한꺼번에 - 서버에는 간격 0 으로 몰려 도착한다
		if (bot->heldPackets.empty() == false && InBurstHold(nowUs) == false)
		{
			for (SendBufferRef& held : bot->heldPackets)
				bot->Send(held);
			bot->heldPackets.clear();
		}

		if (nowUs < bot->hardCcUntilUs.load())
			continue;

		// -ai: 두뇌가 판단하고 패킷으로 실행한다 (BotAiDriver) - 아래 기존 행동은 쓰지 않는다
		if (BotAiDriver::GAi.load())
		{
			BotAiDriver::Tick(bot, nowUs, deltaTime);
			continue;
		}

		// -idleone: 1 번 봇은 같은 자리를 보내기만 한다 - 서버에는 "움직이지 않음"으로 보인다. -idleall: 전원 (FoW 실기동 - 자리가 정해져야 한다)
		if ((GIdleOne.load() && bot->botIndex == 1) || GIdleAll.load())
		{
			Protocol::C_MOVE pkt;
			Protocol::PosInfo* info = pkt.mutable_info();
			info->set_object_id(bot->objectId.load());
			info->set_x(bot->x);
			info->set_y(bot->y);
			info->set_z(bot->z);
			info->set_state(Protocol::MOVE_STATE_IDLE);
			if (GNoClientMs.load() == false)
				pkt.set_client_ms(nowUs / 1000);
			SendOrHold(bot, ClientPacketHandler::MakeSendBuffer(pkt), nowUs);
			continue;
		}

		BotSessionRef target = nullptr;
		float bestDistSq = FLT_MAX;

		// PVE - 봇끼리 싸우지 않는다. -allyfire 면 일부러 파티원만 노린다 (서버가 [ALLY] 로 막는지 본다)
		const bool pve = (runMode != Protocol::RUN_MODE_PVP);
		const bool allyFire = pve && GAllyFire.load();

		for (BotSessionRef& other : snapshot)
		{
			if (other == bot)
				continue;
			if (other->inGame.load() == false || other->alive.load() == false)
				continue;
			if (pve && (allyFire == false || bot->IsPartyMember(other->characterId.load()) == false))
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

		// -monsterpull: 봇 대신 가장 가까운 몬스터. 봇끼리는 싸우지 않는다. PVE 도 같다 (-allyfire 는 파티원을 노린다)
		uint64 monsterId = 0;
		if (GMonsterPull.load() || (pve && allyFire == false))
		{
			target = nullptr;
			lock_guard<mutex> guard(bot->itemLock);
			for (const auto& [id, pos] : bot->monsters)
			{
				const float mx = pos.first - bot->x;
				const float my = pos.second - bot->y;
				const float distSq = mx * mx + my * my;
				if (monsterId == 0 || distSq < bestDistSq)
				{
					monsterId = id;
					bestDistSq = distSq;
					bot->destX = pos.first;
					bot->destY = pos.second;
				}
			}
		}

		// -tower: 싸우지 않고 그 지점으로 걸어가 선다 - 서버 게이지가 채워 탈출시킨다
		float stopDistance = BOT_STOP_DISTANCE;
		if (GTowerMode.load())
		{
			target = nullptr;
			monsterId = 1;			// 배회 분기를 막는다 - 아래 이동은 dest 로만 간다 (공격은 안 한다: bestDistSq 가 크다)
			bestDistSq = FLT_MAX;
			bot->destX = GTowerX;
			bot->destY = GTowerY;

			// -towercount: 지켜보는 봇 - 제자리에 선다 (다른 사람의 완충 S_DESPAWN 을 받는 쪽)
			const bool watcher = (GTowerCount.load() > 0 && static_cast<int32>(bot->botIndex) > GTowerCount.load());
			if (watcher)
			{
				bot->destX = bot->x;
				bot->destY = bot->y;
			}

			// 벽이 있는 층 - 길을 찾아 꺾는 점을 하나씩 밟는다. 중간 점은 정확히 밟아야 다음 직선이 벽을 피한다 (멈춤 거리 0)
			if (watcher == false && BotPath::IsLoaded() && bot->floorId == BotPath::LoadedFloor())
			{
				if (bot->replanPath.exchange(false))
				{
					bot->path = BotPath::Find(bot->x, bot->y, GTowerX, GTowerY);
					bot->pathIndex = 0;
					LOG_LINE << "[BOT PATH] char=" << bot->objectId.load() << " from=(" << static_cast<int32>(bot->x) << "," << static_cast<int32>(bot->y)
						<< ") waypoints=" << bot->path.size() << '\n';
				}

				while (bot->pathIndex < bot->path.size())
				{
					const float wx = bot->path[bot->pathIndex].first - bot->x;
					const float wy = bot->path[bot->pathIndex].second - bot->y;
					if (wx * wx + wy * wy > WAYPOINT_REACHED_CM * WAYPOINT_REACHED_CM)
						break;
					bot->pathIndex++;
				}

				if (bot->path.empty() == false)
				{
					const size_t at = (std::min)(bot->pathIndex, bot->path.size() - 1);
					bot->destX = bot->path[at].first;
					bot->destY = bot->path[at].second;
					stopDistance = 0.f;
				}
			}
		}

		if (target != nullptr)
		{
			bot->destX = target->x;
			bot->destY = target->y;
		}

		// 우회 중이면 목표 대신 꺾는 점 (2026-10-18). 목표가 3 m 넘게 바뀌었으면 버린다 - 아래 벽 검사가 필요하면 다시 구한다
		const bool pathWorld = (GTowerMode.load() == false) && BotPath::IsLoaded() && bot->floorId == BotPath::LoadedFloor();
		const float goalX = bot->destX;
		const float goalY = bot->destY;
		if (pathWorld && bot->detour.empty() == false)
		{
			const float gx = goalX - bot->detourGoalX;
			const float gy = goalY - bot->detourGoalY;
			if (gx * gx + gy * gy > DETOUR_GOAL_SLACK_CM * DETOUR_GOAL_SLACK_CM)
			{
				bot->detour.clear();
			}
			else
			{
				while (bot->detourIndex < bot->detour.size())
				{
					const float wx = bot->detour[bot->detourIndex].first - bot->x;
					const float wy = bot->detour[bot->detourIndex].second - bot->y;
					if (wx * wx + wy * wy > DETOUR_REACHED_CM * DETOUR_REACHED_CM)
						break;
					bot->detourIndex++;
				}
				if (bot->detourIndex >= bot->detour.size())
				{
					bot->detour.clear();
				}
				else
				{
					bot->destX = bot->detour[bot->detourIndex].first;
					bot->destY = bot->detour[bot->detourIndex].second;
				}
			}
		}
		const float stepFromX = bot->x;
		const float stepFromY = bot->y;

		const float dx = bot->destX - bot->x;
		const float dy = bot->destY - bot->y;
		const float dist = ::sqrtf(dx * dx + dy * dy);

		const bool casting = (nowUs < bot->castUnitlUs);
		const bool dashing = (nowUs < bot->dashUntilUs.load());

		const uint64 slowEnd = bot->slowUntilUs.load();
		float moveSpeed = BOT_MOVE_SPEED;
		if (nowUs < slowEnd)
		{
			moveSpeed = BOT_MOVE_SPEED * (1.f - bot->slowMagnitude.load());
			if (moveSpeed < BOT_MOVE_SPEED * 0.5f)
				moveSpeed = BOT_MOVE_SPEED * 0.5f;
		}
		else if (bot->slowMagnitude.load() != 0.f)
		{
			bot->slowMagnitude.store(0.f);
		}

		if (dashing)
		{
			const float step = DASH_SPEED * deltaTime;
			bot->x += bot->dashDirX * step;
			bot->y += bot->dashDirY * step;
		}
		else if (casting)
		{

		}
		else if (target == nullptr && monsterId == 0)
		{
			if (dist < ARRIVE_EPSILON)
			{
				bot->destX = bot->x + Utils::GetRandom(-WANDER_RADIUS, WANDER_RADIUS);
				bot->destY = bot->y + Utils::GetRandom(-WANDER_RADIUS, WANDER_RADIUS);
			}
			else
			{
				const float step = moveSpeed * deltaTime;
				const float ratio = (step < dist) ? (step / dist) : 1.f;

				bot->x += dx * ratio;
				bot->y += dy * ratio;
			}
		}
		else if (dist > stopDistance)
		{
			const float step = moveSpeed * deltaTime;
			const float ratio = (step < dist) ? (step / dist) : 1.f;

			bot->x += dx * ratio;
			bot->y += dy * ratio;
		}

		// 하네스 걸음도 벽 · 바다를 안다 (2026-10-18) - 내딛은 자리가 걸을 수 없으면 되돌린다. 배회면 다른 곳을, 쫓는 중이면
		// 목표까지 BotPath 길을 구해(1 초에 한 번) 다음 틱부터 꺾는 점을 따른다. 대시는 그 자리에서 멈춘다
		if (pathWorld && (bot->x != stepFromX || bot->y != stepFromY) && BotPath::Walkable(bot->x, bot->y) == false)
		{
			bot->x = stepFromX;
			bot->y = stepFromY;
			if (dashing)
			{
				bot->dashUntilUs.store(0);
			}
			else if (target == nullptr && monsterId == 0)
			{
				bot->destX = bot->x + Utils::GetRandom(-WANDER_RADIUS, WANDER_RADIUS);
				bot->destY = bot->y + Utils::GetRandom(-WANDER_RADIUS, WANDER_RADIUS);
			}
			else if (nowUs >= bot->nextDetourUs)
			{
				bot->nextDetourUs = nowUs + DETOUR_REPLAN_US;
				bot->detour = BotPath::Find(bot->x, bot->y, goalX, goalY);
				bot->detourIndex = 0;
				bot->detourGoalX = goalX;
				bot->detourGoalY = goalY;
				++GDetoursThisWindow;
			}
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

			// 보낸 시각 - 서버 이동 검증이 패킷 간격을 이걸로 잰다. -noclientms 면 비운다(예전 방식과 비교용)
			if (GNoClientMs.load() == false)
				pkt.set_client_ms(nowUs / 1000);

			// -burst: 송신이 멈춘 프로세스 흉내 - 쥐고 있다가 창이 끝나면 한꺼번에 보낸다 (G2 soak2 재현)
			SendOrHold(bot, ClientPacketHandler::MakeSendBuffer(pkt), nowUs);

			++GSentThisWindow;
		}

		if (GTowerMode.load())
		{
			// 스킬도 쓰지 않는다 - 이동만 (대시가 구역 밖으로 튀어 나가지 않게)
		}
		else if (casting == false && dashing == false && nowUs >= bot->nextDashAtUs)
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

				SendOrHold(bot, ClientPacketHandler::MakeSendBuffer(dashPkt), nowUs);

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

			SendOrHold(bot, ClientPacketHandler::MakeSendBuffer(areaPkt), nowUs);

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

			SendOrHold(bot, ClientPacketHandler::MakeSendBuffer(skillPkt), nowUs);

			bot->nextSkillAtUs = nowUs + SKILL_TRY_INTERVAL_US;
			bot->castUnitlUs = nowUs + BOT_CAST_HOLD_US;
			++GSkillSentThisWindow;
		}
		else if (target != nullptr && bestDistSq <= ATTACK_RANGE_SQ && nowUs >= bot->nextAttackAtUs)
		{
			Protocol::C_ATTACK attackPkt;
			attackPkt.set_target_id(target->objectId.load());

			SendOrHold(bot, ClientPacketHandler::MakeSendBuffer(attackPkt), nowUs);

			bot->nextAttackAtUs = nowUs + ATTACK_INTERVAL_US;
			++GAttackSentThisWindow;
		}
		else if (monsterId != 0 && bestDistSq <= ATTACK_RANGE_SQ && nowUs >= bot->nextAttackAtUs)
		{
			Protocol::C_ATTACK attackPkt;
			attackPkt.set_target_id(monsterId);

			SendOrHold(bot, ClientPacketHandler::MakeSendBuffer(attackPkt), nowUs);

			bot->nextAttackAtUs = nowUs + ATTACK_INTERVAL_US;
			++GAttackSentThisWindow;
			GMonsterAttacksSent.fetch_add(1);
		}

		// 아이템 행동 (G2 의 B 조건). 시전 · 돌진 중에는 하지 않는다 - 사람도 싸우는 손으로 줍지 않는다
		if (GItemBehavior.load() && casting == false && dashing == false)
		{
			TryOpenBag(bot, nowUs);
			TryEquip(bot, nowUs);
			TryEscape(bot, nowUs);
		}

		if (GSwapArmor.load())
			TrySwapArmor(bot, nowUs);

		if (const uint64 chatAt = bot->chatAtUs.load(); chatAt != 0 && nowUs >= chatAt)
		{
			bot->chatAtUs.store(0);
			Protocol::C_CHAT chat;
			chat.set_msg(GEnterChat);
			SendOrHold(bot, ClientPacketHandler::MakeSendBuffer(chat), nowUs);
			LOG_LINE << "[BOT CHAT] char=" << bot->objectId.load() << " " << GEnterChat << '\n';
		}
	}
}

int main(int argc, char* argv[])
{
	::setvbuf(stdout, nullptr, _IONBF, 0);

	int32 botCount = 1;
	int32 grantUnique = 0;

	for (int i = 1; i < argc - 1; i++)
	{
		if (::strcmp(argv[i], "-bots") == 0)
			botCount = ::atoi(argv[i + 1]);
		else if (::strcmp(argv[i], "-grant") == 0)
			grantUnique = ::atoi(argv[i + 1]);
		else if (::strcmp(argv[i], "-entry") == 0)
			GEntryPointId.store(static_cast<uint32>(::atoi(argv[i + 1])));
		else if (::strcmp(argv[i], "-gold") == 0)
			GRunGold.store(::strtoull(argv[i + 1], nullptr, 10));
		else if (::strcmp(argv[i], "-chat") == 0)
			GEnterChat = argv[i + 1];
		else if (::strcmp(argv[i], "-chatafter") == 0)
			GChatAfterMs.store(static_cast<uint32>(::atoi(argv[i + 1])));
		else if (::strcmp(argv[i], "-towercount") == 0)
			GTowerCount.store(::atoi(argv[i + 1]));
		else if (::strcmp(argv[i], "-enhance") == 0)
			GEnhance.store(::atoi(argv[i + 1]));
		else if (::strcmp(argv[i], "-pvpparty") == 0)
			GPvpParty.store(::atoi(argv[i + 1]));
		else if (::strcmp(argv[i], "-betrayafter") == 0)
			GBetrayAfterUs.store(static_cast<uint64>(::atof(argv[i + 1]) * 1'000'000.0));
		else if (::strcmp(argv[i], "-campenter") == 0)
			GCampEnterAfterUs.store(static_cast<uint64>(::atof(argv[i + 1]) * 1'000'000.0));
		else if (::strcmp(argv[i], "-kickafter") == 0)
			GKickAfterUs.store(static_cast<uint64>(::atof(argv[i + 1]) * 1'000'000.0));
		else if (::strcmp(argv[i], "-rawwalk") == 0)
			GRawWalk = (::atoi(argv[i + 1]) != 0);
		else if (::strcmp(argv[i], "-botbase") == 0)
			GBotBase = (std::max)(0, ::atoi(argv[i + 1]));
		else if (::strcmp(argv[i], "-floor") == 0)
			GForceFloor.store(::atoi(argv[i + 1]));
		else if (::strcmp(argv[i], "-mode") == 0)
		{
			const string mode = argv[i + 1];
			GRunMode.store(mode == "solo" ? Protocol::RUN_MODE_PVE_SOLO : mode == "coop" ? Protocol::RUN_MODE_PVE_COOP
				: mode == "camp" ? Protocol::RUN_MODE_BASECAMP : Protocol::RUN_MODE_PVP);
		}
		else if (::strcmp(argv[i], "-party") == 0)
			GPartySize.store(::atoi(argv[i + 1]));
		else if (::strcmp(argv[i], "-tower") == 0 && i + 2 < argc)
		{
			GTowerMode.store(true);
			GTowerX = static_cast<float>(::atof(argv[i + 1]));
			GTowerY = static_cast<float>(::atof(argv[i + 2]));
		}
	}

	bool grantSpread = false;
	for (int i = 1; i < argc; i++)
	{
		if (::strcmp(argv[i], "-spread") == 0)
			grantSpread = true;
		else if (::strcmp(argv[i], "-items") == 0)
			GItemBehavior.store(true);
		else if (::strcmp(argv[i], "-noclientms") == 0)
			GNoClientMs.store(true);
		else if (::strcmp(argv[i], "-burst") == 0)
			GBurst.store(true);
		else if (::strcmp(argv[i], "-monsterpull") == 0)
			GMonsterPull.store(true);
		else if (::strcmp(argv[i], "-idleone") == 0)
			GIdleOne.store(true);
		else if (::strcmp(argv[i], "-idleall") == 0)
			GIdleAll.store(true);
		else if (::strcmp(argv[i], "-enhancedup") == 0)
			GEnhanceDup.store(true);
		else if (::strcmp(argv[i], "-ai") == 0)
			BotAiDriver::GAi.store(true);
		else if (::strcmp(argv[i], "-aitrace") == 0)
			BotAiDriver::GAiTrace.store(true);
		else if (::strcmp(argv[i], "-aiscript") == 0 && i + 1 < argc)
		{
			string error;
			if (BotAiDriver::LoadScript(argv[++i], OUT error) == false)
			{
				cout << "[AI] script invalid - " << error << '\n';
				return 1;
			}
			BotAiDriver::GAi.store(true);
		}
		else if (::strcmp(argv[i], "-swap") == 0)
			GSwapArmor.store(true);
		else if (::strcmp(argv[i], "-staydead") == 0)
			GStayDead.store(true);
		else if (::strcmp(argv[i], "-holdonreject") == 0)
			GHoldOnReject.store(true);
		else if (::strcmp(argv[i], "-allyfire") == 0)
			GAllyFire.store(true);
	}

	GBurstStartUs = Utils::NowMicroseconds();

	if (botCount < 1)
		botCount = 1;

	// -tower 는 벽을 돌아간다 - 벽이 있는 층(0층)만 (BotPath.h).
	// 2026-10-18 부터 다른 하네스 걸음(-items · -monsterpull · 봇끼리)도 벽 · 바다를 안다 - -rawwalk 면 예전처럼 직선 (비교용)
	if (GTowerMode.load() || GRawWalk == false)
		BotPath::Load(0);

	// -ai 는 벽을 돌아가고(0층) 두뇌 데이터(bot_brain.csv · skills.csv · towers.csv)를 읽는다 - 없으면 시작하지 않는다
	if (BotAiDriver::GAi.load())
	{
		if (BotPath::IsLoaded() == false && BotPath::Load(0) == false)
			return 1;
		string error;
		if (BotAiDriver::Init(OUT error) == false)
		{
			LOG_LINE << "[AI] init failed - " << error << '\n';
			return 1;
		}
	}

	// -pathcheck: 0층을 끝에서 끝까지 벽을 돌아 걸으면 몇 초인가 - 계산만 하고 끝낸다.
	// 섬(2026-10-11)은 서쪽 끝 ↔ 동쪽 끝(직선 약 570 m · 168 s) · 북 ↔ 남(약 280 m · 82 s), 각 끝은 해안선 안쪽 8~13 m.
	// 옛 280 m 정사각형의 대각선(396 m, "약 116 초 ±15 %")은 모서리가 바다라 더 못 잰다
	for (int i = 1; i < argc; i++)
	{
		if (::strcmp(argv[i], "-pathcheck") != 0)
			continue;

		if (BotPath::Load(0) == false)
			return 1;

		const float pairs[4][4] =
		{
			{      0.f, -28500.f,   1000.f,  28200.f },		// 서쪽 끝 → 동쪽 끝
			{   1000.f,  28200.f,      0.f, -28500.f },
			{  14000.f,      0.f, -14000.f,   -600.f },		// 북 → 남
			{ -14000.f,   -600.f,  14000.f,      0.f },
		};

		for (const auto& p : pairs)
		{
			const vector<pair<float, float>> path = BotPath::Find(p[0], p[1], p[2], p[3]);
			float length = 0.f;
			float ax = p[0];
			float ay = p[1];
			for (const auto& [x, y] : path)
			{
				length += ::sqrtf((x - ax) * (x - ax) + (y - ay) * (y - ay));
				ax = x;
				ay = y;
			}

			const float straight = ::sqrtf((p[2] - p[0]) * (p[2] - p[0]) + (p[3] - p[1]) * (p[3] - p[1]));
			LOG_LINE << "[PATHCHECK] (" << p[0] << "," << p[1] << ") -> (" << p[2] << "," << p[3] << ") waypoints=" << path.size()
				<< " straight=" << straight / 100.f << "m path=" << length / 100.f << "m (+" << (length / straight - 1.f) * 100.f << "%)"
				<< " time=" << length / BOT_MOVE_SPEED << "s at " << BOT_MOVE_SPEED << "cm/s\n";
		}

		return 0;
	}

	LOG_LINE << "[BOT] launching " << botCount << " bot(s)"
		<< (GItemBehavior.load() ? " - items ON (open bags, loot, equip, escape + re-enter)" : " - items OFF")
		<< (GNoClientMs.load() ? " - client_ms OFF" : " - client_ms ON")
		<< (GBurst.load() ? " - burst ON (all packets held 1.5s every 20s)" : "")
		<< (GMonsterPull.load() ? " - monster pull ON (chase + hit the nearest monster)" : "")
		<< (GRunMode.load() == Protocol::RUN_MODE_PVE_SOLO ? " - PVE solo" : GRunMode.load() == Protocol::RUN_MODE_PVE_COOP
			? " - PVE co-op party=" + to_string(GPartySize.load()) + (GAllyFire.load() ? " ALLY FIRE (negative test)" : "") : string())
		<< (GTowerMode.load() ? " - tower mode (walk to " + to_string(static_cast<int32>(GTowerX)) + "," + to_string(static_cast<int32>(GTowerY)) + " and stand)" : string())
		<< (BotAiDriver::GAi.load() ? string(" - AI brain ON") + (BotAiDriver::GAiTrace.load() ? " (trace)" : "") + (BotAiDriver::HasScript() ? " (bot 1 scripted)" : "") : string())
		<< " - entry=" << GEntryPointId.load() << " gold=" << GRunGold.load() << '\n';

	// 계정 번호 풀. 끊긴 세션의 번호를 다음 세션이 그대로 물려 받는다
	{
		lock_guard<mutex> guard(GBotsLock);
		GFreeBotIndices.clear();
		GFreeBotIndices.reserve(botCount);
		for (int32 i = botCount; i >= 1; i--)
			GFreeBotIndices.push_back(static_cast<uint32>(GBotBase + i));
	}

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
		RunGrantBenchmark(botCount, grantUnique, grantSpread);
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

			LOG_LINE << "[BOT] send rate = "
				<< (botCount > 0 ? sent / sec / botCount : 0.0)
				<< " Hz/bot  (target 30)"
				<< "   attacks = " << (attacks / sec) << "/s"
				<< "   skills = " << (skills / sec) << "/s"
				<< "   detours = " << GDetoursThisWindow.exchange(0) << '\n';

			if (GMonsterPull.load())
			{
				LOG_LINE << "[BOT MON] attacks = " << GMonsterAttacksSent.exchange(0) / sec << "/s"
					<< "   kills total = " << GMonsterKills.load() << '\n';
			}

			if (GTowerMode.load())
			{
				LOG_LINE << "[BOT RUN] escaped total = " << GLeaveEscaped.load() << "   expired total = " << GLeaveExpired.load() << '\n';
			}

			if (BotAiDriver::GAi.load())
			{
				LOG_LINE << "[BOT AI] decisions = " << BotAiDriver::GAiDecisions.exchange(0) / sec << "/s"
					<< "   branch changes = " << BotAiDriver::GAiBranchChanges.exchange(0) / sec << "/s"
					<< "   attacks = " << BotAiDriver::GAiAttacks.exchange(0) / sec << "/s"
					<< "   skills = " << BotAiDriver::GAiSkills.exchange(0) / sec << "/s"
					<< "   opens = " << BotAiDriver::GAiLootOpens.exchange(0) / sec << "/s"
					<< "   escaped total = " << GLeaveEscaped.load() << "   expired total = " << GLeaveExpired.load() << '\n';
			}

			if (GItemBehavior.load())
			{
				LOG_LINE << "[BOT ITEMS] opens = " << GBagOpenSent.exchange(0) / sec << "/s"
					<< "   loots = " << GLootSent.exchange(0) / sec << "/s"
					<< "   equips = " << GEquipSent.exchange(0) / sec << "/s"
					<< "   escapes = " << GEscapeSent.exchange(0) / sec << "/s"
					<< "   reenters = " << GReenterSent.exchange(0) / sec << "/s"
					<< "   item ok = " << GItemOk.exchange(0) << "  fail = " << GItemFail.exchange(0) << '\n';
			}

			lastReportUs = nowUs;
		}
	}

	GThreadManager->Join();
}
