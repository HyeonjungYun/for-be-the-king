#include "pch.h"
#include "ClientPacketHandler.h"
#include "BotSession.h"

void BotSession::OnConnected()
{
	{
		lock_guard<mutex> guard(GBotsLock);
		GBots.push_back(static_pointer_cast<BotSession>(GetSessionRef()));
	}

	Protocol::C_ENTER_GAME pkt;
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
	cout << "[BOT] disconnected id=" << objectId.load() << endl;
}
