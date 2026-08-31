#include "pch.h"
#include "ClientPacketHandler.h"
#include "BotSession.h"

void BotSession::OnConnected()
{
	{
		lock_guard<mutex> guard(GBotsLock);
		GBots.push_back(static_pointer_cast<BotSession>(GetSessionRef()));

		botIndex = static_cast<uint32>(GBots.size());
		floorId = (botIndex - 1) % 4;
	}

	char username[32];
	::snprintf(username, sizeof(username), "bot_%u", botIndex);

	Protocol::C_LOGIN loginPkt;
	loginPkt.set_token(Utils::Sha256Hex(username));

	Send(ClientPacketHandler::MakeSendBuffer(loginPkt));
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
