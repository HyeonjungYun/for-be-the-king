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

	// 로그인한 세션 (어디에 있든) - PVE 협동 파티 (ADR-0014). 없으면 nullptr
	GameSessionRef FindByCharacter(uint64 characterId);
	// 캐릭터 이름이 정확히 같은 로그인 세션 - 초대 (이름은 전역 유일, schema uk_name)
	GameSessionRef FindByName(const string& name);

private:
	USE_LOCK;
	set<GameSessionRef> _sessions;
};

extern GameSessionManager GSessionManager;
