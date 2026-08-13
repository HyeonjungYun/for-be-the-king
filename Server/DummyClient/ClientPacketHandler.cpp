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
	if (pkt.info().object_id() == bot->objectId.load())
	{
		bot->x = pkt.info().x();
		bot->y = pkt.info().y();
		bot->destX = bot->x;
		bot->destY = bot->y;

		cout << "[BOT SNAPBACK] id=" << bot->objectId.load() << " -> (" << bot->x << ", " << bot->y << ")" << endl;
	}

	return true;
}

bool Handle_S_CHAT(PacketSessionRef& session, Protocol::S_CHAT& pkt)
{
	return true;
}
