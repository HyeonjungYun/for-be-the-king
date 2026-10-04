#pragma once

class GameSession;

using GameSessionRef = shared_ptr<GameSession>;

class GameSessionManager
{
public:
	void Add(GameSessionRef session);
	void Remove(GameSessionRef session);
	void Broadcast(SendBufferRef sendBuffer);

	// 베이스캠프에서 이 캐릭터를 보고 있는 세션. 없으면 nullptr (UI 4단계 - 사망 커밋이 끝났을 때 결과를 다시 보낸다)
	GameSessionRef FindInBaseCamp(uint64 characterId);

private:
	USE_LOCK;
	set<GameSessionRef> _sessions;
};

extern GameSessionManager GSessionManager;
