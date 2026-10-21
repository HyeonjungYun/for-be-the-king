#pragma once
#include <mutex>
#include <thread>

/*----------------
	AdminServer
----------------*/

// 관리 포트 (2026-10-21 · Tools/ServerMonitor/PROTOCOL.md) - ServerMonitor(디버깅 툴)가 붙는다.
//   127.0.0.1:FBTK_ADMIN_PORT (기본 7790, 0 = 끔) - 이 PC 에서만. 첫 줄이 hello + 토큰이 아니면 끊는다
//   토큰은 부팅마다 새로 만들어 정보 파일(Logs/admin.json, FBTK_ADMIN_FILE)에 쓴다 - 앱은 그 파일을 읽는다
// 스레드 하나가 select 로 돈다. 게임 상태가 필요한 요청(접속자 · 상세 · 맵 · 방)은 그 방의 잡(DoAsync)에 맡기고 결과 문자열만 받는다 -
// 관리 스레드는 _objects 도 플레이어 필드도 직접 읽지 않는다. 명령(강제 종료 · 정지 해제 · 공지)은 [ADMIN] 로그 → 이벤트 로그에 남는다

struct AdminConfig
{
	uint16	port = 7790;
	uint16	gamePort = 7777;
	string	infoFile;		// 정보 파일 (토큰 · 포트 · 데이터 폴더)
	string	dataDir;
};

class AdminServer
{
public:
	bool	Start(const AdminConfig& config, OUT string& error);
	void	Stop();
	bool	Running() const { return _running.load(); }

	// EventLog 의 구독자 - 이벤트 쓰기 스레드에서 줄마다 (큐에 넣기만)
	void	PublishEvent(const string& line);

	static constexpr int32	MAX_CLIENTS = 4;
	static constexpr size_t	MAX_QUEUED_BYTES = 64ull * 1024 * 1024;	// 느린 접속자 한 명이 메모리를 다 먹지 않게 - 넘으면 이벤트를 버리고 센다

private:
	struct Client;
	using ClientRef = shared_ptr<Client>;

	void	Run();
	void	HandleLine(const ClientRef& client, const string& line);
	void	Send(const ClientRef& client, string line);
	void	Broadcast(const string& line, bool metricsOnly);
	void	PumpMaps(uint64 nowUs);
	void	PumpMetrics();

	string	PlayersJson();
	string	RoomsJson();
	string	PlayerJson(uint64 characterId);
	string	MetricsJson();
	string	Kick(uint64 characterId, const string& reason);
	string	Notice(const string& text);
	void	Unban(const ClientRef& client, uint64 accountId, const string& name);

	AdminConfig				_config;
	string					_token;
	string					_boot;
	uintptr_t				_listen = ~static_cast<uintptr_t>(0);
	atomic<bool>			_running = false;
	atomic<bool>			_stop = false;
	std::thread				_thread;

	std::mutex				_clientsLock;
	vector<ClientRef>		_clients;
	uint64					_metricsSeq = 0;
};

extern AdminServer GAdmin;
