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
		cout << "[BOT] login failed idx=" << bot->botIndex << endl;
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
	if (pkt.success() == false)
		return true;

	auto bot = static_pointer_cast<BotSession>(session);

	const Protocol::PosInfo& pos = pkt.player().pos_info();

	// 서버가 정해준 스폰 위치에서 시작. 
	bot->x = pos.x();
	bot->y = pos.y();
	bot->z = pos.z();
	bot->destX = pos.x();
	bot->destY = pos.y();

	bot->objectId.store(pkt.player().object_id());
	bot->inGame.store(true);

	return true;
}

bool Handle_S_LEAVE_GAME(PacketSessionRef& session, Protocol::S_LEAVE_GAME& pkt)
{
	auto bot = static_pointer_cast<BotSession>(session);
	bot->inGame.store(false);

	return true;
}

bool Handle_S_SPAWN(PacketSessionRef& session, Protocol::S_SPAWN& pkt)
{
	return true;
}

bool Handle_S_DESPAWN(PacketSessionRef& session, Protocol::S_DESPAWN& pkt)
{
	return false;
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

		cout << "[BOT SNAPBACK] id=" << bot->objectId.load() << " -> (" << bot->x << ", " << bot->y << ")" << endl;
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
		const bool isHard = (type == Protocol::CC_TYPE_STUN)
			|| (type == Protocol::CC_TYPE_ROOT)
			|| (type == Protocol::CC_TYPE_KNOCKBACK)
			|| (type == Protocol::CC_TYPE_LAUNCH);

		if (isHard == false)
			continue;

		const uint64 endUs = nowUs + static_cast<uint64>(info.duration_ms()) * 1000;

		uint64 prev = bot->hardCcUntilUs.load();
		while (endUs > prev && bot->hardCcUntilUs.compare_exchange_weak(prev, endUs) == false)
			;
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

		cout << "[BOT DIED] id=" << bot->objectId.load()
			<< " by=" << info.instigator_id() << endl;

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

bool Handle_S_ITEM_RESULT(PacketSessionRef& session, Protocol::S_ITEM_RESULT& pkt)
{
	return true;
}

bool Handle_S_INVENTORY_SYNC(PacketSessionRef& session, Protocol::S_INVENTORY_SYNC& pkt)
{
	return true;
}

bool Handle_S_BAG_CONTENTS(PacketSessionRef& session, Protocol::S_BAG_CONTENTS& pkt)
{
	return true;
}
