#include "pch.h"
#include "GameSessionManager.h"
#include "GameSession.h"

GameSessionManager GSessionManager;

void GameSessionManager::Add(GameSessionRef session)
{
	WRITE_LOCK;
	_sessions.insert(session);
}

void GameSessionManager::Remove(GameSessionRef session)
{
	WRITE_LOCK;
	_sessions.erase(session);
}

void GameSessionManager::Broadcast(SendBufferRef sendBuffer)
{
	WRITE_LOCK;
	for (GameSessionRef session : _sessions)
	{
		session->Send(sendBuffer);
	}
}

GameSessionRef GameSessionManager::FindInBaseCamp(uint64 characterId)
{
	WRITE_LOCK;
	for (const GameSessionRef& session : _sessions)
	{
		// place 를 먼저 본다 - 로그인 작업이 characters 를 채운 뒤에 place 를 BaseCamp 로 바꾸므로(atomic),
		// BaseCamp 로 보이면 characters 는 이미 다 쓰였다.
		if (session->place.load() != SessionPlace::BaseCamp)
			continue;

		for (const Protocol::CharacterInfo& character : session->characters)
		{
			if (character.object_info().object_id() == characterId)
				return session;
		}
	}

	return nullptr;
}