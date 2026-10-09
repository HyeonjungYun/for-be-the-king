#include "pch.h"
#include "ClientPacketHandler.h"
#include "BufferReader.h"
#include "BotSession.h"

PacketHandlerFunc GPacketHandler[UINT16_MAX];

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

	bot->Send(ClientPacketHandler::MakeSendBuffer(enterPkt));

	return true;
}

bool Handle_S_ENTER_GAME(PacketSessionRef& session, Protocol::S_ENTER_GAME& pkt)
{
	auto bot = static_pointer_cast<BotSession>(session);

	if (pkt.success() == false)
	{
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

	if (GItemBehavior.load())
	{
		lock_guard<mutex> guard(bot->itemLock);
		const float escapeSec = Utils::GetRandom(BOT_ESCAPE_MIN_SEC, BOT_ESCAPE_MAX_SEC);
		bot->escapeAtUs = Utils::NowMicroseconds() + static_cast<uint64>(escapeSec * 1'000'000.f);
	}

	bot->inGame.store(true);

	LOG_LINE << "[BOT] entered char=" << bot->objectId.load() << " at=(" << bot->x << "," << bot->y << ") run_left=" << pkt.run_remaining_ms() << "ms\n";

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
	if (GItemBehavior.load() == false && GMonsterPull.load() == false)
		return true;

	auto bot = static_pointer_cast<BotSession>(session);
	lock_guard<mutex> guard(bot->itemLock);

	for (const Protocol::ObjectInfo& info : pkt.players())
	{
		if (info.object_type() == Protocol::OBJECT_TYPE_BAG && GItemBehavior.load())
			bot->bags[info.object_id()] = { info.pos_info().x(), info.pos_info().y() };
		else if (info.creature_type() == Protocol::CREATE_TYPE_MONSTER && GMonsterPull.load())
			bot->monsters[info.object_id()] = { info.pos_info().x(), info.pos_info().y() };
	}

	return true;
}

bool Handle_S_DESPAWN(PacketSessionRef& session, Protocol::S_DESPAWN& pkt)
{
	if (GItemBehavior.load() == false && GMonsterPull.load() == false)
		return true;

	auto bot = static_pointer_cast<BotSession>(session);
	lock_guard<mutex> guard(bot->itemLock);

	for (uint64 id : pkt.object_ids())
	{
		bot->bags.erase(id);
		bot->monsters.erase(id);
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
			|| (type == Protocol::CC_TYPE_LAUNCH);

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
	return false;
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

	if (GItemBehavior.load() == false && GSwapArmor.load() == false)
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
	if (GItemBehavior.load() == false)
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
