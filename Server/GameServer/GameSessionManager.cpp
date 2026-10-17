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

int32 GameSessionManager::DropUnauthenticated(uint64 nowUs, uint64 timeoutUs)
{
	vector<GameSessionRef> idle;
	{
		WRITE_LOCK;
		for (const GameSessionRef& session : _sessions)
		{
			const uint64 since = session->connectedAtUs.load();
			// 로그인 처리 중(loginStarted)이면 DB 가 느린 것이다 - 건드리지 않는다
			if (session->loginStarted.load() == false && session->accountId.load() == 0 && since != 0 && nowUs > since + timeoutUs)
				idle.push_back(session);
		}
	}

	// 락 밖에서 끊는다 - Disconnect 의 정리(OnDisconnected → Remove)가 이 락을 다시 잡는다
	for (const GameSessionRef& session : idle)
		session->Disconnect(L"login timeout");
	return static_cast<int32>(idle.size());
}

GameSessionRef GameSessionManager::FindByCharacter(uint64 characterId)
{
	WRITE_LOCK;
	for (const GameSessionRef& session : _sessions)
	{
		// accountId 가 0 이 아니면 로그인 작업이 characters 를 다 채운 뒤다 (accountId 를 마지막에 쓴다)
		if (session->accountId.load() == 0)
			continue;

		for (const Protocol::CharacterInfo& character : session->characters)
		{
			if (character.object_info().object_id() == characterId)
				return session;
		}
	}
	return nullptr;
}

GameSessionRef GameSessionManager::FindByName(const string& name)
{
	if (name.empty())
		return nullptr;

	WRITE_LOCK;
	for (const GameSessionRef& session : _sessions)
	{
		if (session->accountId.load() == 0)
			continue;

		for (const Protocol::CharacterInfo& character : session->characters)
		{
			if (character.name() == name)
				return session;
		}
	}
	return nullptr;
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