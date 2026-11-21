#pragma once
#include "ServerStats.h"

class BotSession;
using BotSessionRef = std::shared_ptr<BotSession>;
class ClientService;
using ClientServiceRef = std::shared_ptr<ClientService>;

/*-----------------
	 ScaleBots
-----------------*/

// -scale (2026-11-21 동접 1만 - docs/server-scaling-10k.md). 수천~1만 봇을 한 프로세스에서 돌리는 가벼운 부하 생성기.
//   · 접속을 초당 N 개로 나눠 건다 (한꺼번에 1만을 걸면 서버 accept 대기열이 넘친다)
//   · 틱 스레드 여러 개가 봇을 나눠 맡는다 (예전엔 스레드 하나가 전원을 돌고, 봇마다 전원을 훑어 O(N²) 이었다)
//   · 봇은 입장 지점 둘레를 걷고, 보이는 가장 가까운 생물을 때린다. 죽으면 나갔다가 다시 들어간다 (끊지 않는다)
//   · 1 초마다 하트비트 - 왕복(RTT)을 재고 다음 하트비트에 실어 서버도 모은다. 10 초마다 요약 한 줄
// 기존 모드(-ai · -items …)와 섞지 않는다
namespace ScaleBots
{
	extern atomic<bool>	GOn;

	struct Options
	{
		int32	botCount = 1000;
		int32	tickThreads = 4;
		int32	connectPerSec = 500;
		int32	heartbeatMs = 1000;
		int32	moveHz = 30;
		float	wanderCm = 1500.f;	// 입장 지점 둘레 배회 반경. 15 m = 뭉침(최악 - 모두가 서로를 본다), 8000 = 섬에 흩어짐 (0 층만 벽 검사)		// C_MOVE 보내는 빈도 (걷는 동안). UE 클라는 30 Hz (S1Player MOVE_PACKET_SEND_DELAY)
		bool	combat = true;		// -scalepeace 면 걷기만
	};

	// 패킷 스레드 → 집계
	void	RecordRtt(uint64 micros);
	void	CountEnter();
	void	CountEnterReject();
	void	CountDeath();
	void	CountKick(int32 reason);
	void	CountLoginFail();

	// main 에서 - 돌아오지 않는다
	void	Run(ClientServiceRef service, const Options& options);
}
