#include "pch.h"
#include "ClientPacketHandler.h"
#include "BotSession.h"

void BotSession::OnConnected()
{
	{
		lock_guard<mutex> guard(GBotsLock);
		GBots.push_back(static_pointer_cast<BotSession>(GetSessionRef()));

		if (GFreeBotIndices.empty())
		{
			botIndex = 0;
		}
		else
		{
			botIndex = GFreeBotIndices.back();
			GFreeBotIndices.pop_back();
		}

		floorId = (botIndex == 0) ? 0 : (botIndex - 1) % 4;
	}

	if (botIndex == 0)
	{
		LOG_LINE << "[BOT] no free account index" << '\n';
		Disconnect(L"no free bot index");
		return;
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

		if (botIndex != 0)
		{
			GFreeBotIndices.push_back(botIndex);
			botIndex = 0;
		}
	}

	LOG_LINE << "[BOT] disconnected id=" << objectId.load() << '\n';
}
