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

	if (GItemBehavior.load())
	{
		lock_guard<mutex> guard(bot->itemLock);
		const float escapeSec = Utils::GetRandom(BOT_ESCAPE_MIN_SEC, BOT_ESCAPE_MAX_SEC);
		bot->escapeAtUs = Utils::NowMicroseconds() + static_cast<uint64>(escapeSec * 1'000'000.f);
	}

	bot->inGame.store(true);

	return true;
}

bool Handle_S_LEAVE_GAME(PacketSessionRef& session, Protocol::S_LEAVE_GAME& pkt)
{
	auto bot = static_pointer_cast<BotSession>(session);
	bot->inGame.store(false);

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
	if (GItemBehavior.load() == false)
		return true;

	auto bot = static_pointer_cast<BotSession>(session);
	lock_guard<mutex> guard(bot->itemLock);

	for (const Protocol::ObjectInfo& info : pkt.players())
	{
		if (info.object_type() == Protocol::OBJECT_TYPE_BAG)
			bot->bags[info.object_id()] = { info.pos_info().x(), info.pos_info().y() };
	}

	return true;
}

bool Handle_S_DESPAWN(PacketSessionRef& session, Protocol::S_DESPAWN& pkt)
{
	if (GItemBehavior.load() == false)
		return true;

	auto bot = static_pointer_cast<BotSession>(session);
	lock_guard<mutex> guard(bot->itemLock);

	for (uint64 id : pkt.object_ids())
	{
		bot->bags.erase(id);
		if (bot->openBagId == id)
			bot->openBagId = 0;
	}

	return true;
}

bool Handle_S_MOVE(PacketSessionRef& session, Protocol::S_MOVE& pkt)
{
	auto bot = static_pointer_cast<BotSession>(session);

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

	return true;
}

bool Handle_S_CC_STATE(PacketSessionRef& session, Protocol::S_CC_STATE& pkt)
{
	return true;
}

bool Handle_S_DIED(PacketSessionRef& session, Protocol::S_DIED& pkt)
{
	auto bot = static_pointer_cast<BotSession>(session);

	for (const Protocol::DiedInfo& info : pkt.deaths())
	{
		if (info.victim_id() != bot->objectId.load())
			continue;

		bot->alive.store(false);

		LOG_LINE << "[BOT DIED] id=" << bot->objectId.load()
			<< " by=" << info.instigator_id() << '\n';

		bot->Disconnect(L"Died");
	}
	return true;
}

bool Handle_S_CHAT(PacketSessionRef& session, Protocol::S_CHAT& pkt)
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

		if (info.effect_id() != 2)
			continue;

		bot->dashUntilUs.store(nowUs + DASH_DURATION_US);
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
	if (GItemBehavior.load() == false)
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
	if (GItemBehavior.load() == false)
		return true;

	// 던전 동기화는 소지 · 착용 전체다. 베이스캠프 동기화에는 보관도 있지만 봇은 보관을 쓰지 않는다
	auto bot = static_pointer_cast<BotSession>(session);
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
