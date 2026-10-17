#pragma once

class GameSession;

using GameSessionRef = shared_ptr<GameSession>;

class GameSessionManager
{
public:
	void Add(GameSessionRef session);
	void Remove(GameSessionRef session);

	// 베이스캠프에서 이 캐릭터를 보고 있는 세션. 없으면 nullptr (UI 4단계 - 사망 커밋이 끝났을 때 결과를 다시 보낸다)
	GameSessionRef FindInBaseCamp(uint64 characterId);

	// 로그인한 세션 (어디에 있든) - PVE 협동 파티 (ADR-0014). 없으면 nullptr
	GameSessionRef FindByCharacter(uint64 characterId);
	// 캐릭터 이름이 정확히 같은 로그인 세션 - 초대 (이름은 전역 유일, schema uk_name)
	GameSessionRef FindByName(const string& name);

	// 연결만 하고 C_LOGIN 을 보내지 않은 지 timeoutUs 가 지난 세션을 끊는다 - 메인 루프가 10 초마다 (2026-10-17 사용자 결정). 끊은 수
	int32 DropUnauthenticated(uint64 nowUs, uint64 timeoutUs);

private:
	USE_LOCK;
	set<GameSessionRef> _sessions;
};

extern GameSessionManager GSessionManager;
