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
