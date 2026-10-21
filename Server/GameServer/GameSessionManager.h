#pragma once

class GameSession;

using GameSessionRef = shared_ptr<GameSession>;

class GameSessionManager
{
public:
	// IP 당 연결 수 상한 (보안 점검 H2 마지막 조각, 2026-10-18) - 넘으면 false (부르는 쪽이 끊는다). 루프백(127.x)은 봇 · 개발용이라 세지 않는다.
	// 상한 = FBTK_MAX_CONN_PER_IP (기본 MAX_CONN_PER_IP_DEFAULT, 0 = 끔)
	static constexpr int32	MAX_CONN_PER_IP_DEFAULT = 16;
	bool Add(GameSessionRef session, const string& ip);
	void Remove(GameSessionRef session);
	int32 ConnectionsFrom(const string& ip);

	// 베이스캠프에서 이 캐릭터를 보고 있는 세션. 없으면 nullptr (UI 4단계 - 사망 커밋이 끝났을 때 결과를 다시 보낸다)
	GameSessionRef FindInBaseCamp(uint64 characterId);

	// 로그인한 세션 (어디에 있든) - PVE 협동 파티 (ADR-0014). 없으면 nullptr
	GameSessionRef FindByCharacter(uint64 characterId);
	// 캐릭터 이름이 정확히 같은 로그인 세션 - 초대 (이름은 전역 유일, schema uk_name)
	GameSessionRef FindByName(const string& name);

	// 연결만 하고 C_LOGIN 을 보내지 않은 지 timeoutUs 가 지난 세션을 끊는다 - 메인 루프가 10 초마다 (2026-10-17 사용자 결정). 끊은 수
	int32 DropUnauthenticated(uint64 nowUs, uint64 timeoutUs);

	// 정상 종료 (2026-10-21) - 모든 세션을 끊는다. 던전의 몸은 유예 몸으로 남고(DB 없음), 베이스캠프 몸은 사라진다. 끊은 수.
	// before 가 있으면 끊기 직전에 세션마다 부른다 (S_KICK{SERVER_SHUTDOWN} 보내기)
	int32 DisconnectAll(const WCHAR* cause, const function<void(const GameSessionRef&)>& before = nullptr);

	// 지금 세션 전부의 사본 (락 밖에서 쓰라고) - 관리 포트의 접속자 목록 · 전체 공지 (2026-10-21)
	vector<GameSessionRef> Snapshot();

	// 로그인한 연결이 timeoutUs 동안 아무 패킷도 보내지 않았다 - 끊는다 (2026-10-21). 클라는 5 s 마다 C_HEARTBEAT 를 보낸다.
	// TCP keepalive 는 죽은 호스트를 잡고, 이것은 TCP 는 살았지만 멈춘 클라를 잡는다. 메인 루프가 10 s 마다. 끊은 수
	int32 DropIdle(uint64 nowUs, uint64 timeoutUs);

private:
	USE_LOCK;
	set<GameSessionRef> _sessions;
	unordered_map<GameSession*, string>	_ipOf;		// 센 연결만 (루프백 제외)
	unordered_map<string, int32>		_perIp;
};

extern GameSessionManager GSessionManager;
