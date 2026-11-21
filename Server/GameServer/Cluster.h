#pragma once

class DBConnection;

/*-------------
	Cluster
-------------*/

// 서버 여러 대 (2026-11-22 동접 1만 2 단계 - docs/server-scaling-10k.md § 5). 공용 저장소는 모든 서버가 이미 붙는 MySQL 이다:
//   game_servers     - 서버마다 5 초에 한 번 인원 · 채널 수 (인증 서버의 방 배정 /lobby/assign 이 읽는다)
//   online_accounts  - 계정이 어느 서버에 있나. 한 계정은 한 서버에만 (Tools/db/schema_cluster.sql)
//
// 서버 간 메시지 = online_accounts.kick_requested. 다른 서버에 붙어 있는 계정이 여기로 로그인하면 그 줄에 깃발을 세우고 1 초 뒤 다시 시도한다.
// 깃발이 선 서버는 1 초마다 그것을 보고 옛 연결을 끊고(S_KICK{DUPLICATE_LOGIN}) 줄을 지운다 - 같은 서버 안의 kick-old 를 서버 사이로 넓힌 것.
// 15 초 넘게 소식이 없는(죽은) 서버의 줄은 새 서버가 그냥 가져간다.
//
// FBTK_SERVER_ID (기본 1) · FBTK_PUBLIC_HOST (기본 127.0.0.1 - 클라가 붙을 주소) · FBTK_CLUSTER=0 이면 끈다 (표가 없는 옛 DB).
// 표가 없거나 질의가 실패하면 로그 한 줄 남기고 혼자 도는 서버처럼 동작한다 (로그인을 막지 않는다)
namespace Cluster
{
	struct Config
	{
		uint32	serverId = 1;
		string	host = "127.0.0.1";
		uint16	port = 7777;
		uint16	adminPort = 0;
		int32	maxSessions = 0;
		bool	enabled = true;
	};

	void	Init(const Config& config);
	bool	Enabled();
	uint32	ServerId();

	// 로그인 DB 작업 안에서 (DB 스레드) - 이 계정을 이 서버 것으로 적는다
	enum class Claim : uint8
	{
		Ok,				// 적었다 (또는 클러스터 꺼짐)
		OtherServer,	// 다른 살아 있는 서버에 붙어 있다 - 그 서버에 끊기를 부탁했다. 잠깐 뒤 다시 시도
	};
	Claim	ClaimAccount(DBConnection* conn, uint64 accountId, uint64 characterId);

	// 계정이 이 서버를 떠났다 (끊김 · 로그아웃) - 줄을 지운다 (DB 큐, 낮은 우선순위). 그 사이 같은 계정이 다시 붙었으면 지우지 않는다
	void	ReleaseAccount(uint64 accountId);

	// 주 루프가 1 초마다 - 5 초마다 등록부를 갱신하고, 1 초마다 끊기 부탁을 확인한다 (DB 큐로 보낸다)
	void	Tick(uint64 nowUs, int32 ccu, int32 channels);

	// 부팅 (DB 연결 뒤 · 접속 받기 전) - 등록 + 지난번 이 서버가 남긴 접속 줄을 지운다
	void	Boot(DBConnection* conn);

	// 종료 - 상태를 down 으로, 이 서버의 접속 줄을 지운다 (DB 큐 - 종료가 DB 를 비우기 전에 부른다)
	void	Shutdown();
}
