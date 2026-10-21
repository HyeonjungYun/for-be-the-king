#pragma once

/*----------------
	ServerConfig
-----------------*/

// 서버 설정 - 환경변수 FBTK_* 를 부팅 때 한 번 읽어 담는다 (2026-10-21 리뷰 후속).
// 예전엔 main() 이 _dupenv_s 블록 26 벌로 하나씩 읽었다. 변수 이름 · 의미는 그대로다 (하네스 · 실행 묶음 · 문서 호환).
//
// 시험 전용 훅(FBTK_FAIL_DEATH_COMMIT · FBTK_FAKE_ACK_LOSS · FBTK_LOGIN_DELAY_MS …)은 FBTK_TEST_HOOKS 가 1 인 빌드에서만 읽고,
// 그 효과를 내는 코드도 그 빌드에만 들어간다 (#if FBTK_TEST_HOOKS). 기본 빌드(개발 · CI · 하네스)는 1, 배포 묶음은
// Tools/package/package-bundle.ps1 이 /p:FbtkTestHooks=0 으로 만든다 - 배포 바이너리에는 DB 트랜잭션 안에서 자는 코드가 없다
struct ServerConfig
{
	// ── 접속 ──
	string		bindAddress = "127.0.0.1";		// FBTK_BIND_ADDRESS - 같은 공간 LAN 플레이테스트는 0.0.0.0 (docs/playtest-network.md)
	uint16		port = 7777;					// FBTK_PORT
	int32		workerThreads = 5;				// FBTK_WORKER_THREADS - IOCP 워커 (ADR-0007)

	// ── DB ──
	string		dbHost = "127.0.0.1";			// FBTK_DB_HOST
	uint32		dbPort = 3306;					// FBTK_DB_PORT - 실행 묶음은 3307
	string		dbUser = "fbtk_app";			// FBTK_DB_USER - 전용 계정 (Tools/db/schema_app_user.sql). 예전엔 root 가 박혀 있었다
	string		dbPassword;						// FBTK_DB_PASSWORD - 필수
	string		dbSchema = "forbetheking";		// FBTK_DB_SCHEMA
	int32		dbThreads = 4;					// FBTK_DB_THREADS - DB 워커 = 커넥션 수 (ADR-0007)

	// ── 디렉터리 (dataDir 는 FBTK_DATA_DIR - ItemTable::DefaultDataDir) ──
	string		dataDir;
	string		journalDir;						// FBTK_JOURNAL_DIR - 런 끝 저장의 write-ahead 기록 (기본 Logs/pending-commits)
	string		telemetryDir;					// FBTK_TELEMETRY_DIR - 기본 Logs/telemetry
	string		metricsDir;						// FBTK_METRICS_DIR - ServerStats CSV (기본 Logs/metrics, "0" 이면 끔)
	string		crashDir;						// FBTK_CRASH_DIR - 미니덤프 (기본 Logs/crash)
	string		eventsDir;						// FBTK_EVENTS_DIR - 구조화 이벤트 로그 JSONL (기본 Logs/events, "0" 이면 파일 없음 - 관리 포트는 돈다)

	// ── 관리 포트 (2026-10-21 · AdminServer · Tools/ServerMonitor) ──
	uint16		adminPort = 7790;				// FBTK_ADMIN_PORT - 127.0.0.1 전용. 0 = 끔
	string		adminFile;						// FBTK_ADMIN_FILE - 토큰 · 포트를 적는 정보 파일 (기본 Logs/admin.json)

	// ── 운영 스위치 ──
	bool		fowEnabled = true;				// FBTK_FOW (0 = 전원이 전부 본다 - A/B 비교 전용)
	uint64		fowLogViewer = 0;				// FBTK_FOW_LOG - 그 캐릭터의 시야 스폰 · 디스폰을 로그 (테스트 전용이지만 읽기만 한다)
	bool		monstersOn = true;				// FBTK_MONSTERS (0 = 몬스터 없음 - G3 비교)
	bool		wallCheck = true;				// FBTK_WALLS (0 = 이동 검증이 벽을 건너뛴다 - 소크 비교 · 비상용)
	bool		botRegear = false;				// FBTK_BOT_REGEAR - bot_ 캐릭터 기본 3점 (부하 측정)
	bool		botLoadouts = false;			// FBTK_BOT_LOADOUTS - bot_ 캐릭터 무작위 빌드 (실행 묶음 AddBots)
	uint64		disconnectGraceUs = 180'000'000;	// FBTK_DISCONNECT_GRACE_MS - 끊긴 몸이 던전에 남는 시간
	uint64		routeSeed = 0;					// FBTK_ROUTE_SEED - 0 = 부팅 시각 (순찰 길이 부팅마다 다르다)
	uint64		idleTimeoutUs = 30'000'000;		// FBTK_IDLE_TIMEOUT_MS - 로그인한 연결이 아무 패킷도 안 보내면 끊는다 (클라 · 봇은 5 s 마다 C_HEARTBEAT). 0 = 끔
	uint32		shutdownDrainMs = 10'000;		// FBTK_SHUTDOWN_DRAIN_MS - Ctrl+C 뒤 DB 큐를 기다리는 상한 (창 닫기는 2.5 s 고정)

#if FBTK_TEST_HOOKS
	// ── 시험 전용 훅 - 검증 하네스가 고장 · 지연 · 디버그 명령을 주입한다. 배포 빌드에는 없다 ──
	struct Hooks
	{
		bool	failDeathCommit = false;		// FBTK_FAIL_DEATH_COMMIT=1 - 모든 사망 저장을 쓰기 뒤에 실패시킨다 (AC-CH-07 롤백)
		uint32	deathCommitDelayMs = 0;			// FBTK_DEATH_COMMIT_DELAY_MS - 사망 저장을 늦춘다 (재입장 가드)
		uint32	baseCampDelayMs = 0;			// FBTK_BASECAMP_DELAY_MS - 베이스캠프 아이템 조작을 잠근 채 늦춘다
		int32	failEscapeCommits = 0;			// FBTK_FAIL_ESCAPE_COMMIT=N - 처음 N 번의 탈출 · 만료 저장이 실패한다
		int32	fakeAckLoss = 0;				// FBTK_FAKE_ACK_LOSS=N - 처음 N 번의 런 끝 저장이 커밋 뒤 응답을 잃은 척한다
		int32	fakeLoginFail = 0;				// FBTK_FAKE_LOGIN_FAIL=N - 처음 N 번의 로그인이 캐릭터 조회에 실패한다
		uint32	loginDelayMs = 0;				// FBTK_LOGIN_DELAY_MS - 로그인 DB 작업을 늦춘다
		uint32	logStallMs = 0;					// FBTK_LOG_STALL_MS - 로그 쓰기 스레드를 10 s 마다 이만큼 멈춘다
		bool	entryFromDb = false;			// FBTK_ENTRY_FROM_DB=1 - 진입 지점 대신 저장 좌표에서 입장 (측정 재현)
		bool	debugGold = false;				// FBTK_DEBUG_GOLD=1 - /gold N
		bool	grantEndpoint = false;			// FBTK_GRANT_ENDPOINT=1 - C_GRANT_REWARD (루프백 전용 시험 끝점)
		bool	debugCommands = false;			// FBTK_DEBUG_COMMANDS=1 - /bag /unbag /die /stun /root /silence /slow
		bool	testMode = false;				// FBTK_TEST_MODE=1 - /t 명령 (디버그 명령도 켠다)
	};
	Hooks		hooks;
	bool		AnyHook() const;
#endif

	static ServerConfig	FromEnv();
	void				LogSummary() const;

	// 환경변수 하나 - 있으면 true. 값이 비어 있어도 "있다"
	static bool			ReadEnv(const char* name, OUT string& value);
};
