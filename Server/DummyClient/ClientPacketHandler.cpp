#include "pch.h"
#include "ClientPacketHandler.h"
#include "BufferReader.h"
#include "BotSession.h"
#include "BotAiDriver.h"

PacketHandlerFunc GPacketHandler[UINT16_MAX + 1];

bool Handle_INVALID(PacketSessionRef& session, BYTE* buffer, int32 len)
{
	return false;
}

bool Handle_S_LOGIN(PacketSessionRef& session, Protocol::S_LOGIN& pkt)
{
	auto bot = static_pointer_cast<BotSession>(session);

	if (pkt.success() == false || pkt.characters_size() == 0)
	{
		LOG_LINE << "[BOT] login failed idx=" << bot->botIndex << " - reconnecting" << '\n';
		bot->Disconnect(L"login failed");
		return true;
	}

	Protocol::C_ENTER_GAME enterPkt;
	enterPkt.set_playerindex(0);
	enterPkt.set_floor_id(bot->floorId);
	enterPkt.set_entry_point_id(GEntryPointId.load());

	const Protocol::CharacterInfo& Character = pkt.characters(0);

	bot->characterId.store(Character.object_info().object_id());

	uint64 expectedId = 0;

	uint64 expected = 0;
	GGrantTargetId.compare_exchange_strong(expected, Character.object_info().object_id());

	// PVE (ADR-0014) - 솔로는 바로 자기 인스턴스로. 협동은 파티부터: 번호로 나눈 첫 봇이 나머지를 초대하고(틱이 보낸다),
	// 모두 모이면 서버가 말하는 파티장이 입장해 멤버를 데려간다 (Handle_S_PARTY_STATE)
	const int32 mode = GRunMode.load();
	if (mode == Protocol::RUN_MODE_PVE_COOP)
	{
		const int32 size = (std::max)(2, (std::min)(3, GPartySize.load()));
		if ((static_cast<int32>(bot->botIndex) - 1) % size == 0)
			bot->nextInviteAtUs.store(Utils::NowMicroseconds() + 1'000'000);		// 같은 묶음 봇이 로그인할 틈
		return true;
	}
	if (mode == Protocol::RUN_MODE_PVE_SOLO)
	{
		enterPkt.set_mode(Protocol::RUN_MODE_PVE_SOLO);
		enterPkt.set_floor_id(0);
	}
	if (mode == Protocol::RUN_MODE_BASECAMP)
		enterPkt.set_mode(Protocol::RUN_MODE_BASECAMP);		// 베이스캠프 맵 (2026-10-17) - 런이 아니라 개인 방

	// -enhance N (N > 0): 베이스캠프에 남아 강화만 보낸다 (item-enhance AC-EN-02) - 입장하지 않는다
	if (GEnhance.load() > 0)
	{
		LOG_LINE << "[BOT ENHANCE] char=" << bot->characterId.load() << " staying in base camp for the enhance test\n";
		return true;
	}

	bot->Send(ClientPacketHandler::MakeSendBuffer(enterPkt));

	return true;
}

bool Handle_S_PARTY_INVITED(PacketSessionRef& session, Protocol::S_PARTY_INVITED& pkt)
{
	auto bot = static_pointer_cast<BotSession>(session);

	// 봇은 받는다
	Protocol::C_PARTY_RESPONSE response;
	response.set_party_id(pkt.party_id());
	response.set_accept(true);
	bot->Send(ClientPacketHandler::MakeSendBuffer(response));

	LOG_LINE << "[BOT PARTY] invited idx=" << bot->botIndex << " by=" << pkt.from_name() << " party=" << pkt.party_id() << " - accept\n";
	return true;
}

bool Handle_S_PARTY_BETRAYED(PacketSessionRef& session, Protocol::S_PARTY_BETRAYED& pkt)
{
	auto bot = static_pointer_cast<BotSession>(session);
	LOG_LINE << "[BOT PARTY] betrayed-broadcast seen by char=" << bot->characterId.load() << " betrayer=" << pkt.betrayer_id() << " name=" << pkt.betrayer_name()
		<< " party=" << pkt.party_id() << " victims=" << pkt.victim_names_size() << '\n';
	return true;
}

bool Handle_S_PARTY_STATE(PacketSessionRef& session, Protocol::S_PARTY_STATE& pkt)
{
	auto bot = static_pointer_cast<BotSession>(session);
	const uint64 me = bot->characterId.load();

	bool leader = false;
	{
		lock_guard<mutex> guard(bot->partyLock);
		bot->partyMembers.clear();
		for (const Protocol::PartyMember& m : pkt.members())
		{
			bot->partyMembers.push_back(m.character_id());
			if (m.character_id() == me && m.leader())
				leader = true;
		}
	}
	bot->partyId.store(pkt.party_id());
	bot->partyLeader.store(leader);
	const bool wasInRun = bot->partyInRun.exchange(pkt.in_run());

	if (pkt.result() != Protocol::PARTY_OK)
		LOG_LINE << "[BOT PARTY] idx=" << bot->botIndex << " result=" << Protocol::PartyResult_Name(pkt.result()) << " subject=" << pkt.subject_name()
			<< " party=" << pkt.party_id() << " members=" << pkt.members_size() << '\n';		// 거절 뒤에도 파티가 그대로인지 (party-betrayal AC-PB-05)

	// 파티 인스턴스가 닫혔다 - 다음 판을 열 수 있다
	if (wasInRun && pkt.in_run() == false)
		bot->coopEnterSent.store(false);

	// 다 모였고 판이 없고 나는 베이스캠프 - 파티장이 연다 (멤버는 서버가 함께 데려간다)
	const int32 size = (std::max)(2, (std::min)(3, GPartySize.load()));
	if (GRunMode.load() == Protocol::RUN_MODE_PVE_COOP && leader && pkt.members_size() >= size && pkt.in_run() == false
		&& bot->inGame.load() == false && bot->coopEnterSent.exchange(true) == false)
	{
		Protocol::C_ENTER_GAME enterPkt;
		enterPkt.set_playerindex(0);
		enterPkt.set_floor_id(0);
		enterPkt.set_entry_point_id(GEntryPointId.load());
		enterPkt.set_mode(Protocol::RUN_MODE_PVE_COOP);
		bot->Send(ClientPacketHandler::MakeSendBuffer(enterPkt));
		LOG_LINE << "[BOT PARTY] leader idx=" << bot->botIndex << " enters co-op with " << pkt.members_size() << " members\n";
	}
	return true;
}

bool Handle_S_ENTER_GAME(PacketSessionRef& session, Protocol::S_ENTER_GAME& pkt)
{
	auto bot = static_pointer_cast<BotSession>(session);

	if (pkt.success() == false)
	{
		// 협동 - 끊지 않고 베이스캠프에서 파티를 기다린다 (파티 상태가 다음 입장을 정한다)
		if (GRunMode.load() == Protocol::RUN_MODE_PVE_COOP)
		{
			LOG_LINE << "[BOT] co-op enter rejected idx=" << bot->botIndex << " char=" << bot->characterId.load() << " - waiting for the party\n";
			bot->coopEnterSent.store(false);
			return true;
		}

		// -holdonreject: UE 클라처럼 베이스캠프에 남아 있다가 다시 입장한다 - 기다리는 동안 이 세션이 베이스캠프 동기화를 받는다
		if (GHoldOnReject.load())
		{
			LOG_LINE << "[BOT] enter rejected idx=" << bot->botIndex
				<< " char=" << bot->characterId.load() << " - waiting in base camp" << '\n';
			bot->reenterAtUs.store(Utils::NowMicroseconds() + BOT_REENTER_DELAY_US);
			return true;
		}

		LOG_LINE << "[BOT] enter rejected idx=" << bot->botIndex
			<< " char=" << bot->characterId.load() << " - reconnecting" << '\n';
		bot->Disconnect(L"enter rejected");
		return true;
	}


	const Protocol::PosInfo& pos = pkt.player().pos_info();

	// 서버가 정해준 스폰 위치에서 시작. 
	bot->x = pos.x();
	bot->y = pos.y();
	bot->z = pos.z();
	bot->destX = pos.x();
	bot->destY = pos.y();

	bot->objectId.store(pkt.player().object_id());
	bot->replanPath.store(true);

	// 새 방 - 지난 방에서 본 몬스터는 잊는다 (베이스캠프 허수아비는 조용히 사라져 S_DESPAWN 이 오지 않는다)
	{
		lock_guard<mutex> guard(bot->itemLock);
		bot->monsters.clear();
	}
	bot->enteredAtUs = Utils::NowMicroseconds();
	bot->pvpInviteSent = false;
	bot->betraySent = false;

	// -ai 감각 - 새 판: 내 체력 · 런 시간 · 무기, 본 것은 잊는다
	if (BotAiDriver::GAi.load())
	{
		lock_guard<mutex> guard(bot->itemLock);
		bot->ai.Reset();
		bot->ai.hp = pkt.player().hp() > 0 ? pkt.player().hp() : 500;
		bot->ai.maxHp = pkt.player().max_hp() > 0 ? pkt.player().max_hp() : 500;
		bot->ai.runEndsAtUs = pkt.run_remaining_ms() > 0 ? Utils::NowMicroseconds() + static_cast<uint64>(pkt.run_remaining_ms()) * 1000 : 0;
		bot->ai.weaponTypeId = pkt.player().weapon_type_id();
		bot->ai.bb = BotAI::Blackboard();
		bot->ai.bb.seed = static_cast<uint32>(pkt.player().object_id() * 2654435761u + 1u);
	}

	if (GItemBehavior.load())
	{
		lock_guard<mutex> guard(bot->itemLock);
		const float escapeSec = Utils::GetRandom(BOT_ESCAPE_MIN_SEC, BOT_ESCAPE_MAX_SEC);
		bot->escapeAtUs = Utils::NowMicroseconds() + static_cast<uint64>(escapeSec * 1'000'000.f);
	}

	bot->inGame.store(true);

	LOG_LINE << "[BOT] entered char=" << bot->objectId.load() << " at=(" << bot->x << "," << bot->y << ") run_left=" << pkt.run_remaining_ms() << "ms"
		<< " mode=" << Protocol::RunMode_Name(pkt.mode()) << '\n';

	// -gold N: 런 골드 (서버 FBTK_DEBUG_GOLD=1 일 때만 받는다) - 만료 30 % · 탈출 100 % 시험 (AC-ET-20)
	if (const uint64 gold = GRunGold.load(); gold > 0)
	{
		Protocol::C_CHAT chat;
		chat.set_msg("/gold " + to_string(gold));
		bot->Send(ClientPacketHandler::MakeSendBuffer(chat));
	}

	if (GSwapArmor.load())
		bot->swapAtUs.store(Utils::NowMicroseconds() + 3'000'000);

	// -chat "<명령>": 입장마다 한 번 보낸다 - 디버그 명령 게이트(FBTK_DEBUG_COMMANDS) 시험. -chatafter 가 있으면 그만큼 뒤에 (틱이 보낸다)
	if (GEnterChat.empty() == false && GChatAfterMs.load() > 0)
	{
		bot->chatAtUs.store(Utils::NowMicroseconds() + static_cast<uint64>(GChatAfterMs.load()) * 1000);
	}
	else if (GEnterChat.empty() == false)
	{
		Protocol::C_CHAT chat;
		chat.set_msg(GEnterChat);
		bot->Send(ClientPacketHandler::MakeSendBuffer(chat));
	}

	return true;
}

bool Handle_S_LEAVE_GAME(PacketSessionRef& session, Protocol::S_LEAVE_GAME& pkt)
{
	auto bot = static_pointer_cast<BotSession>(session);
	bot->inGame.store(false);

	LOG_LINE << "[BOT LEAVE] char=" << bot->objectId.load() << " reason=" << pkt.reason() << " gold_kept=" << pkt.gold_kept()
		<< " destroyed=" << pkt.items_destroyed() << '\n';
	if (pkt.reason() == Protocol::RUN_END_ESCAPED)
		GLeaveEscaped.fetch_add(1);
	else if (pkt.reason() == Protocol::RUN_END_EXPIRED)
		GLeaveExpired.fetch_add(1);

	// -tower 루프: 탈출하면 베이스캠프에서 잠깐 쉬고 다시 들어가 같은 타워로 (AC-ET-30 · 26)
	if (GTowerMode.load() && GItemBehavior.load() == false)
		bot->reenterAtUs.store(Utils::NowMicroseconds() + BOT_REENTER_DELAY_US);

	// 탈출 커밋이 끝났다 - 베이스캠프에서 잠깐 쉬고 다시 들어간다. 던전에서 본 가방은 이제 모른다
	if (GItemBehavior.load())
	{
		{
			lock_guard<mutex> guard(bot->itemLock);
			bot->bags.clear();
			bot->openBagId = 0;
			bot->escapeAtUs = 0;
		}

		bot->reenterAtUs.store(Utils::NowMicroseconds() + BOT_REENTER_DELAY_US);
	}

	return true;
}

bool Handle_S_SPAWN(PacketSessionRef& session, Protocol::S_SPAWN& pkt)
{
	const bool ai = BotAiDriver::GAi.load();
	if (GItemBehavior.load() == false && GMonsterPull.load() == false && ai == false)
		return true;

	auto bot = static_pointer_cast<BotSession>(session);
	lock_guard<mutex> guard(bot->itemLock);

	for (const Protocol::ObjectInfo& info : pkt.players())
	{
		// -ai 감각 - 생물은 전부 (가방은 아래 bags 에)
		if (ai && info.object_type() != Protocol::OBJECT_TYPE_BAG && info.object_id() != bot->objectId.load())
		{
			BotAiCreature& c = bot->ai.creatures[info.object_id()];
			c.monster = (info.creature_type() == Protocol::CREATE_TYPE_MONSTER);
			c.x = info.pos_info().x();
			c.y = info.pos_info().y();
			c.hp = info.hp();
			c.maxHp = (std::max)(1, info.max_hp());
			c.monsterTypeId = info.monster_type_id();
			c.ally = (c.monster == false && GRunMode.load() != Protocol::RUN_MODE_PVP);		// PVE 의 다른 플레이어 = 파티원
		}
		if (info.object_type() == Protocol::OBJECT_TYPE_BAG && (GItemBehavior.load() || ai))
			bot->bags[info.object_id()] = { info.pos_info().x(), info.pos_info().y() };
		else if (info.creature_type() == Protocol::CREATE_TYPE_MONSTER && GMonsterPull.load())
			bot->monsters[info.object_id()] = { info.pos_info().x(), info.pos_info().y() };
	}

	return true;
}

bool Handle_S_DESPAWN(PacketSessionRef& session, Protocol::S_DESPAWN& pkt)
{
	if (GItemBehavior.load() == false && GMonsterPull.load() == false && BotAiDriver::GAi.load() == false)
		return true;

	auto bot = static_pointer_cast<BotSession>(session);
	lock_guard<mutex> guard(bot->itemLock);

	for (uint64 id : pkt.object_ids())
	{
		bot->bags.erase(id);
		bot->monsters.erase(id);
		bot->ai.creatures.erase(id);
		if (bot->openBagId == id)
			bot->openBagId = 0;
	}

	return true;
}

bool Handle_S_MOVE(PacketSessionRef& session, Protocol::S_MOVE& pkt)
{
	auto bot = static_pointer_cast<BotSession>(session);

	// 몬스터는 서버가 움직인다 - 같은 S_MOVE 에 실려 온다 (monster-ai.md Core Rule 25)
	if (GMonsterPull.load() && pkt.correction() == false)
	{
		lock_guard<mutex> guard(bot->itemLock);
		for (const Protocol::PosInfo& info : pkt.infos())
		{
			auto it = bot->monsters.find(info.object_id());
			if (it != bot->monsters.end())
				it->second = { info.x(), info.y() };
		}
	}
	// -ai 감각 - 아는 생물의 자리
	if (BotAiDriver::GAi.load() && pkt.correction() == false)
	{
		lock_guard<mutex> guard(bot->itemLock);
		for (const Protocol::PosInfo& info : pkt.infos())
		{
			auto it = bot->ai.creatures.find(info.object_id());
			if (it != bot->ai.creatures.end())
			{
				it->second.x = info.x();
				it->second.y = info.y();
			}
		}
	}

	if (pkt.correction() == false)
		return true;

	for (const Protocol::PosInfo& info : pkt.infos())
	{
		if (info.object_id() != bot->objectId.load())
			continue;

		bot->x = info.x();
		bot->y = info.y();
		bot->destX = bot->x;
		bot->destY = bot->y;

		LOG_LINE << "[BOT SNAPBACK] id=" << bot->objectId.load() << " -> (" << bot->x << ", " << bot->y << ")" << '\n';
	}

	return true;
}

bool Handle_S_ATTACK(PacketSessionRef& session, Protocol::S_ATTACK& pkt)
{
	return true;
}

bool Handle_S_ATTACK_CANCEL(PacketSessionRef& session, Protocol::S_ATTACK_CANCEL& pkt)
{
	return true;
}

bool Handle_S_DAMAGE(PacketSessionRef& session, Protocol::S_DAMAGE& pkt)
{
	// 맞은 것은 처음 30건만 - 장비 스탯이 판정에 들어갔는지 숫자로 본다 (서버 [STATS] 와 맞춰 읽는다, 2026-10-07).
	// 피해 0(체력 재동기화 - 착용 교체 · 몬스터 귀환)은 드물어서 전부 찍는다
	static atomic<int32> logged = 0;

	auto bot = static_pointer_cast<BotSession>(session);
	if (BotAiDriver::GAi.load())
	{
		// -ai 감각 - 체력 · 나를 때린 적
		const uint64 nowUs = Utils::NowMicroseconds();
		lock_guard<mutex> guard(bot->itemLock);
		for (const Protocol::DamageInfo& info : pkt.damages())
		{
			if (info.target_id() == bot->objectId.load())
			{
				bot->ai.hp = info.remaining_hp();
				if (info.max_hp() > 0)
					bot->ai.maxHp = info.max_hp();
				if (info.damage() > 0)
				{
					bot->ai.lastDamagedUs = nowUs;
					auto it = bot->ai.creatures.find(info.attacker_id());
					if (it != bot->ai.creatures.end())
						it->second.lastHitMeUs = nowUs;
				}
			}
			else
			{
				auto it = bot->ai.creatures.find(info.target_id());
				if (it != bot->ai.creatures.end())
				{
					it->second.hp = info.remaining_hp();
					if (info.max_hp() > 0)
						it->second.maxHp = info.max_hp();
				}
			}
		}
	}
	for (const Protocol::DamageInfo& info : pkt.damages())
	{
		// 남의 최대 체력 변화 - 던전 안 장비 교체를 방 안의 다른 사람도 받는지 (DamageInfo.max_hp, 2026-10-08)
		if (info.target_id() != bot->objectId.load())
		{
			if (info.max_hp() > 0)
				LOG_LINE << "[BOT SEE MAXHP] observer=" << bot->objectId.load() << " target=" << info.target_id()
					<< " max=" << info.max_hp() << " hp=" << info.remaining_hp() << '\n';
			continue;
		}
		if (info.damage() != 0 && logged.fetch_add(1) >= 30)
			continue;

		cout << "[BOT HIT] target=" << info.target_id() << " attacker=" << info.attacker_id() << " dmg=" << info.damage()
			<< " hp=" << info.remaining_hp() << " max=" << info.max_hp() << " crit=" << info.is_crit()
			<< " type=" << (info.damage_type() == Protocol::DAMAGE_TYPE_MAGIC ? "magic" : "physical") << '\n';
	}
	return true;
}

bool Handle_S_CC(PacketSessionRef& session, Protocol::S_CC& pkt)
{
	auto bot = static_pointer_cast<BotSession>(session);

	const uint64 nowUs = Utils::NowMicroseconds();

	for (const Protocol::CcEventInfo& info : pkt.applied())
	{
		if (info.target_id() != bot->objectId.load())
			continue;

		const Protocol::CcType type = info.cc_type();
		const uint64 endUs = nowUs + static_cast<uint64>(info.duration_ms()) * 1000;

		const bool isHard = (type == Protocol::CC_TYPE_STUN)
			|| (type == Protocol::CC_TYPE_ROOT)
			|| (type == Protocol::CC_TYPE_KNOCKBACK)
			|| (type == Protocol::CC_TYPE_LAUNCH)
			|| (type == Protocol::CC_TYPE_TAUNT);		// 도발 - 서버가 끌고 간다, 봇은 가만히 (보정으로 위치를 받는다)

		if (isHard)
		{
			uint64 prev = bot->hardCcUntilUs.load();
			while (endUs > prev && bot->hardCcUntilUs.compare_exchange_weak(prev, endUs) == false)
				;
			continue;
		}

		if (type == Protocol::CC_TYPE_SLOW)
		{
			// 이전 감속이 이미 끝났으면 최대값을 리셋한다. (틱 스레드는 읽기만 한다)
			if (bot->slowUntilUs.load() <= nowUs)
				bot->slowMagnitude.store(0.f);

			float clamped = info.magnitude();
			if (clamped < 0.f)
				clamped = 0.f;
			if (clamped > BOT_SLOW_CAP)
				clamped = BOT_SLOW_CAP;

			uint64 prevEnd = bot->slowUntilUs.load();
			while (endUs > prevEnd && bot->slowUntilUs.compare_exchange_weak(prevEnd, endUs) == false)
				;

			float prevMag = bot->slowMagnitude.load();
			while (clamped > prevMag && bot->slowMagnitude.compare_exchange_weak(prevMag, clamped) == false)
				;
		}
	}

	// 넉백 · 끌기 - 서버가 위치를 끝점으로 옮겼다. 봇은 벽을 모르니 끝점을 그대로 받는다 (서버 값 = 벽 보정 뒤 실제 거리).
	// 틀리면 CC 뒤 첫 이동이 거부되고 보정(S_MOVE correction)으로 맞춰진다 (skill-roster 1 단계)
	for (const Protocol::KnockbackInfo& info : pkt.knockbacks())
	{
		if (info.target_id() != bot->objectId.load())
			continue;

		bot->x += info.dir_x() * info.distance();
		bot->y += info.dir_y() * info.distance();
		bot->destX = bot->x;
		bot->destY = bot->y;

		LOG_LINE << "[BOT PUSHED] id=" << bot->objectId.load() << " by=" << info.instigator_id()
			<< " dist=" << info.distance() << " -> (" << bot->x << ", " << bot->y << ")" << '\n';
	}

	return true;
}

bool Handle_S_CC_STATE(PacketSessionRef& session, Protocol::S_CC_STATE& pkt)
{
	return true;
}

bool Handle_S_STATUS(PacketSessionRef& session, Protocol::S_STATUS& pkt)
{
	// 상태 효과 (지속 피해 · 버프 · 표식) - 봇은 처음 20건만 찍는다. 서버 [DOT] · [BUFF] · [MARK] 와 맞춰 읽는다 (skill-roster 1 단계)
	static atomic<int32> logged = 0;

	auto bot = static_pointer_cast<BotSession>(session);
	for (const Protocol::StatusInfo& info : pkt.applied())
	{
		if (info.target_id() != bot->objectId.load() || logged.fetch_add(1) >= 20)
			continue;

		LOG_LINE << "[BOT STATUS] target=" << info.target_id() << " kind=" << info.kind() << " param=" << info.param()
			<< " stacks=" << info.stacks() << " ms=" << info.remaining_ms() << " mag=" << info.magnitude()
			<< " by=" << info.instigator_id() << '\n';
	}
	return true;
}

bool Handle_S_DIED(PacketSessionRef& session, Protocol::S_DIED& pkt)
{
	auto bot = static_pointer_cast<BotSession>(session);

	for (const Protocol::DiedInfo& info : pkt.deaths())
	{
		if (BotAiDriver::GAi.load())
		{
			lock_guard<mutex> guard(bot->itemLock);
			bot->ai.creatures.erase(info.victim_id());
			if (info.victim_id() == bot->objectId.load())
				bot->ai.hp = 0;
		}
		if (GMonsterPull.load())
		{
			lock_guard<mutex> guard(bot->itemLock);
			if (bot->monsters.erase(info.victim_id()) > 0 && info.instigator_id() == bot->objectId.load())
			{
				GMonsterKills.fetch_add(1);
				LOG_LINE << "[BOT KILL] monster=" << info.victim_id() << " by=" << bot->objectId.load() << " cause=" << info.cause() << '\n';
			}
		}

		if (info.victim_id() != bot->objectId.load())
			continue;

		bot->alive.store(false);

		LOG_LINE << "[BOT DIED] id=" << bot->objectId.load()
			<< " by=" << info.instigator_id() << " cause=" << info.cause() << '\n';

		// -staydead: 죽어도 끊지 않는다 - 사망 저장 중에 죽은 몸이 보낸 요청을 서버가 거절하는지 본다 (2026-10-08)
		if (GStayDead.load() == false)
			bot->Disconnect(L"Died");
	}
	return true;
}

bool Handle_S_CHAT(PacketSessionRef& session, Protocol::S_CHAT& pkt)
{
	return true;
}

// 봇은 게이지 · 상자 상태를 그리지 않는다 - 받아서 로그만 (escape-timer.md · radar.md 패킷)
bool Handle_S_ESCAPE_GAUGE(PacketSessionRef& session, Protocol::S_ESCAPE_GAUGE& pkt)
{
	if (BotAiDriver::GAi.load())
	{
		auto bot = static_pointer_cast<BotSession>(session);
		lock_guard<mutex> guard(bot->itemLock);
		bot->ai.gaugePermille = pkt.progress_permille();
		bot->ai.inZone = pkt.self_in_zone();
	}
	if (GTowerMode.load())
	{
		auto bot = static_pointer_cast<BotSession>(session);
		LOG_LINE << "[BOT GAUGE] char=" << bot->objectId.load() << " tower=" << pkt.tower_id() << " state=" << pkt.state()
			<< " permille=" << pkt.progress_permille() << " self=" << (pkt.self_in_zone() ? 1 : 0) << '\n';
	}
	return true;
}

bool Handle_S_CHEST_STATE(PacketSessionRef& session, Protocol::S_CHEST_STATE& pkt)
{
	return true;
}

bool Handle_S_SKILL_CAST(PacketSessionRef& session, Protocol::S_SKILL_CAST& pkt)
{
	return false;
}

bool Handle_S_SKILL_CANCEL(PacketSessionRef& session, Protocol::S_SKILL_CANCEL& pkt)
{
	return false;
}

bool Handle_S_SKILL_HIT(PacketSessionRef& session, Protocol::S_SKILL_HIT& pkt)
{
	auto bot = static_pointer_cast<BotSession>(session);

	const uint64 myId = bot->objectId.load();
	const uint64 nowUs = Utils::NowMicroseconds();

	for (const Protocol::SkillHitInfo& info : pkt.hits())
	{
		if (info.caster_id() != myId)
			continue;

		// 신발 질주 (2026-10-10 로스터 1003 - 예전 effect 2)
		if (info.effect_id() != 1003)
			continue;

		bot->dashUntilUs.store(nowUs + DASH_DURATION_US);
	}

	return true;
}

bool Handle_S_SKILL_REVEAL(PacketSessionRef& session, Protocol::S_SKILL_REVEAL& pkt)
{
	// 총명 (#91) - 상대의 스킬 구성. 봇은 로그만
	auto bot = static_pointer_cast<BotSession>(session);
	LogLine out;
	out << "[BOT REVEAL] me=" << bot->objectId.load() << " target=" << pkt.target_id() << " skills=";
	for (const Protocol::SkillInfo& s : pkt.skills())
		out << " [" << s.slot() << "]" << s.skill_id();
	out << '\n';
	return true;
}

bool Handle_S_WEAPON_CHANGE(PacketSessionRef& session, Protocol::S_WEAPON_CHANGE& pkt)
{
	// 무기 교체 (anim, 2026-10-12) - 봇은 그리지 않는다
	return true;
}

bool Handle_S_VISION(PacketSessionRef& session, Protocol::S_VISION& pkt)
{
	// FoW 시야 (2026-10-11) - 봇은 그리지 않는다. 추가 시야원이 생길 때만 로그 (분신 · 종이매 · 탐지 확인용)
	if (pkt.sources_size() > 0)
	{
		auto bot = static_pointer_cast<BotSession>(session);
		LOG_LINE << "[BOT VISION] me=" << bot->objectId.load() << " radius=" << pkt.radius_cm() << " sources=" << pkt.sources_size() << '\n';
	}
	return true;
}

bool Handle_S_ZONE(PacketSessionRef& session, Protocol::S_ZONE& pkt)
{
	// 장판 (3 단계) - 봇은 그리지 않는다. 처음 20건만 찍는다
	static atomic<int32> logged = 0;
	for (const Protocol::ZoneInfo& info : pkt.spawned())
	{
		if (logged.fetch_add(1) >= 20)
			break;
		LOG_LINE << "[BOT ZONE] id=" << info.zone_id() << " caster=" << info.caster_id() << " effect=" << info.effect_id()
			<< " r=" << info.radius_cm() << " " << info.param() << " for=" << info.duration_ms() << "ms" << '\n';
	}
	return true;
}

bool Handle_S_PROJECTILE(PacketSessionRef& session, Protocol::S_PROJECTILE& pkt)
{
	// 투사체 (2 단계) - 봇은 그리지 않는다. 내 것의 끝만 처음 30건 찍는다 (HIT / WALL / EXPIRED 비율을 본다)
	static atomic<int32> logged = 0;

	auto bot = static_pointer_cast<BotSession>(session);
	const uint64 myId = bot->objectId.load();

	for (const Protocol::ProjectileEndInfo& info : pkt.ended())
	{
		// 발사체 주인은 spawned 에만 있다 - 끝 패킷엔 없어 여기서는 전부 센다 (처음 30건)
		if (logged.fetch_add(1) >= 30)
			break;

		LOG_LINE << "[BOT PROJ END] id=" << info.projectile_id() << " reason=" << info.reason()
			<< " target=" << info.target_id() << " at=(" << info.x() << ", " << info.y() << ") seen_by=" << myId << '\n';
	}
	return true;
}

bool Handle_S_EQUIP_SYNC(PacketSessionRef& session, Protocol::S_EQUIP_SYNC& pkt)
{
	// -ai 감각 - 내 스킬 칸 (장비가 바뀔 때마다 전체)
	if (BotAiDriver::GAi.load())
	{
		auto bot = static_pointer_cast<BotSession>(session);
		lock_guard<mutex> guard(bot->itemLock);
		bot->ai.skillSlots.clear();
		for (const Protocol::SkillInfo& info : pkt.skills())
		{
			if (info.skill_id() != 0 && info.bind_state() == Protocol::BIND_BOUND)
				bot->ai.skillSlots.emplace_back(static_cast<uint32>(info.slot()), info.skill_id());
		}
	}
	return true;
}

bool Handle_S_GRANT_REWARD(PacketSessionRef& session, Protocol::S_GRANT_REWARD& pkt)
{
	switch (pkt.result())
	{
	case Protocol::GRANT_OK:			GGrantOk.fetch_add(1); break;
	case Protocol::GRANT_ALREADY:		GGrantAlready.fetch_add(1); break;
	case Protocol::GRANT_NO_TARGET:		GGrantNoTarget.fetch_add(1); break;
	case Protocol::GRANT_BAD_REQUEST:	GGrantBad.fetch_add(1); break;
	default:							GGrantError.fetch_add(1); break;
	}

	GGrantReplies.fetch_add(1);
	return true;
}

namespace
{
	bool IsHeld(const Protocol::ItemInstance& item)
	{
		return item.state() == Protocol::ITEM_STATE_CARRIED || item.state() == Protocol::ITEM_STATE_EQUIPPED;
	}
}

bool Handle_S_ITEM_RESULT(PacketSessionRef& session, Protocol::S_ITEM_RESULT& pkt)
{
	// -enhance: 결과를 그대로 남긴다 (item-enhance AC-EN-02 · 03)
	if (GEnhance.load() != 0 && pkt.request_id().rfind("enh", 0) == 0)
	{
		auto bot = static_pointer_cast<BotSession>(session);
		LOG_LINE << "[BOT ENHANCE] char=" << bot->characterId.load() << " req=" << pkt.request_id() << " result=" << Protocol::ItemResult_Name(pkt.result());
		for (const Protocol::ItemInstance& item : pkt.changed())
			LOG_LINE << " item=" << item.instance_id() << " level=" << item.level() << " state=" << item.state();
		LOG_LINE << '\n';
	}
	if (GItemBehavior.load() == false && GSwapArmor.load() == false)
		return true;

	if (pkt.result() != Protocol::ITEM_OK)
	{
		GItemFail.fetch_add(1);
		return true;
	}

	GItemOk.fetch_add(1);

	auto bot = static_pointer_cast<BotSession>(session);
	lock_guard<mutex> guard(bot->itemLock);

	for (const Protocol::ItemInstance& item : pkt.changed())
	{
		if (IsHeld(item))
			bot->items[item.instance_id()] = item;
		else
			bot->items.erase(item.instance_id());
	}

	return true;
}

bool Handle_S_INVENTORY_SYNC(PacketSessionRef& session, Protocol::S_INVENTORY_SYNC& pkt)
{
	auto bot = static_pointer_cast<BotSession>(session);

	// 베이스캠프에서 받은 동기화 - 사망 저장이 끝나기 전 상태인지(소지 · 착용이 남았는지) 서버 로그와 맞춰 본다
	if (GHoldOnReject.load() && bot->inGame.load() == false)
		LOG_LINE << "[BOT BASECAMP SYNC] char=" << bot->characterId.load() << " items=" << pkt.items_size() << '\n';

	// -enhance N: 베이스캠프 동기화(입장 전)에서 첫 소지/보관 아이템에 N 번. 입장 뒤 동기화면 -1 일 때만 1 번(거절 확인). 한 세션에 한 번
	if (GEnhance.load() != 0 && bot->enhanceSent == false && pkt.items_size() > 0)
	{
		const bool inDungeon = bot->inGame.load();
		const int32 n = GEnhance.load();
		if ((n > 0 && inDungeon == false) || (n < 0 && inDungeon))
		{
			bot->enhanceSent = true;
			const Protocol::ItemInstance& first = pkt.items(0);
			for (int32 k = 0; k < (n > 0 ? n : 1); k++)
			{
				Protocol::C_ENHANCE req;
				req.set_request_id("enh" + to_string(k) + "-" + bot->NextRequestId());
				req.set_instance_id(first.instance_id());
				bot->Send(ClientPacketHandler::MakeSendBuffer(req));
				if (k == 0 && GEnhanceDup.load())		// -enhancedup: 같은 request_id 재전송 - 서버가 ITEM_ALREADY 로 막아야 한다 (TR-item-027)
					bot->Send(ClientPacketHandler::MakeSendBuffer(req));
			}
			LOG_LINE << "[BOT ENHANCE] char=" << bot->characterId.load() << " sent=" << (n > 0 ? n : 1) << " item=" << first.instance_id() << " level_before=" << first.level()
				<< (inDungeon ? " (in dungeon - expect reject)" : " (base camp)") << '\n';
		}
	}

	if (GItemBehavior.load() == false && GSwapArmor.load() == false && BotAiDriver::GAi.load() == false)
		return true;

	// 던전 동기화는 소지 · 착용 전체다. 베이스캠프 동기화에는 보관도 있지만 봇은 보관을 쓰지 않는다
	lock_guard<mutex> guard(bot->itemLock);

	bot->items.clear();
	for (const Protocol::ItemInstance& item : pkt.items())
	{
		if (IsHeld(item))
			bot->items[item.instance_id()] = item;
	}

	return true;
}

bool Handle_S_BAG_CONTENTS(PacketSessionRef& session, Protocol::S_BAG_CONTENTS& pkt)
{
	if (GItemBehavior.load() == false && BotAiDriver::GAi.load() == false)
		return true;

	// 내가 연 가방의 첫 내용에만 반응한다. 열어 둔 동안 남이 가져갈 때마다 다시 오는 갱신은 흘려보낸다
	constexpr int32 TAKE_PER_OPEN = 3;

	auto bot = static_pointer_cast<BotSession>(session);
	vector<uint64> toTake;
	{
		lock_guard<mutex> guard(bot->itemLock);
		if (bot->openBagId == 0 || pkt.bag_id() != bot->openBagId)
			return true;

		bot->openBagId = 0;

		int32 carried = 0;
		for (const auto& [id, item] : bot->items)
		{
			if (item.state() == Protocol::ITEM_STATE_CARRIED)
				carried++;
		}

		const int32 room = min(BOT_INVENTORY_SLOTS - carried, TAKE_PER_OPEN);
		for (int32 i = 0; i < pkt.items_size() && static_cast<int32>(toTake.size()) < room; i++)
			toTake.push_back(pkt.items(i).instance_id());
	}

	for (uint64 instanceId : toTake)
	{
		Protocol::C_LOOT lootPkt;
		lootPkt.set_request_id(bot->NextRequestId());
		lootPkt.set_bag_id(pkt.bag_id());
		lootPkt.set_instance_id(instanceId);

		bot->Send(ClientPacketHandler::MakeSendBuffer(lootPkt));
		GLootSent.fetch_add(1);
	}

	return true;
}
