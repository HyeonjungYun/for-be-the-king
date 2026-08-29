#include "pch.h"
#include "ClientPacketHandler.h"
#include "BotSession.h"

void BotSession::OnConnected()
{
	{
		lock_guard<mutex> guard(GBotsLock);
		GBots.push_back(static_pointer_cast<BotSession>(GetSessionRef()));

		floorId = static_cast<uint32>(GBots.size() - 1) % 4;
	}

	Protocol::C_ENTER_GAME pkt;
	pkt.set_floor_id(floorId);
	Send(ClientPacketHandler::MakeSendBuffer(pkt));
}

void BotSession::OnRecvPacket(BYTE* buffer, int32 len)
{
	PacketSessionRef session = GetPacketSessionRef();
	ClientPacketHandler::HandlePacket(session, buffer, len);
}

void BotSession::OnDisconnected()
{
	inGame.store(false);
	alive.store(false);

	{
		lock_guard<mutex> guard(GBotsLock);

		BotSessionRef self = static_pointer_cast<BotSession>(GetSessionRef());
		GBots.erase(remove(GBots.begin(), GBots.end(), self), GBots.end());
	}

	cout << "[BOT] disconnected id=" << objectId.load() << endl;
}
