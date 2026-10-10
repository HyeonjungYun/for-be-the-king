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

		// -floor N: 전원 한 층 (FoW · 테스트 모드 실기동 - 봇끼리 · 사람과 한 방에 모은다, 2026-10-11)
		floorId = (GForceFloor.load() >= 0) ? static_cast<uint32>(GForceFloor.load()) : ((botIndex == 0) ? 0 : (botIndex - 1) % 4);
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
