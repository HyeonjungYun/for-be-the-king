#pragma once

/*--------------
	MoveClock
--------------*/

// 이동 검증이 패킷 사이 경과 시간을 무엇으로 잴지 정한다 (roadmap.md § 0-11 후속, 2026-10-05).
//
// 🔴 왜: 예전에는 서버가 패킷을 "처리한 시각" 간격을 썼다. 보내는 쪽(클라 · 봇)이나 서버 어느 쪽이 잠깐 멈췄다가
//    밀린 C_MOVE 가 한꺼번에 처리되면, 간격이 0 에 가까운데 위치 차이는 그대로라 정상 이동을 거부했다
//    (G2 soak2 SV-3 109건 · § 0-10 4차). 이제 클라가 C_MOVE.client_ms 에 보낸 시각을 싣고 그 간격을 쓴다.
//
// 조작 방지 — 클라 시계는 믿되 서버 시계를 넘어서지는 못한다:
//  - 클라가 서버보다 MAX_LEAD_US 넘게 앞서 달리면(시각을 부풀리는 스피드핵) 넘은 만큼 간격을 깎는다.
//    누적 상한이다 - 스피드핵이 한 세션에 얻는 이득은 이 시간만큼의 이동이 전부이고, 그 뒤로는 서버 속도로 묶인다.
//    패킷 한 개의 이동 상한(Room MAX_BUDGET_SEC 0.5초 분량)은 따로 그대로라 순간이동은 안 된다.
//  - 클라가 뒤처진 시간은 MAX_LAG_US 까지만 기억한다 - 통신을 끊었다 몰아 보내도(랙 스위치) 그 이상은 인정하지 않는다.
//  - client_ms 가 0(구버전) · 첫 패킷 · 시계가 거꾸로 가면 서버 간격을 쓰고 기준을 다시 잡는다.
//
// 🔴 MAX_LEAD 가 0.5초면 안 되는 이유 (2026-10-05 실측): 기준은 입장 후 첫 패킷이다. 그 패킷이 늦게 도착하면
//    (보내는 쪽이 멈춘 사이에 입장) 이후 정상 패킷이 전부 "앞섰다" 로 보인다 - 앞섬이 기준 패킷의 지연만큼 생긴다.
//    1.5초씩 몰아 보내는 봇에서 0.5초 상한은 3분에 1,000 ~ 2,000건을 거부했다. 지연 몫을 덮도록 뒤처짐과 같은 2초로 둔다.
struct MoveClock
{
	static constexpr int64	MAX_LEAD_US = 2'000'000;	// 기준 패킷 지연 몫 + 스피드핵이 얻을 수 있는 누적 이득의 상한
	static constexpr int64	MAX_LAG_US = 2'000'000;		// 몰아 보낸 이동을 받아 주는 한계

	uint64	lastClientMs = 0;
	int64	leadUs = 0;			// 누적(클라 경과 - 서버 경과). 양수 = 클라가 앞섰다
	bool	announced = false;	// 이 접속의 첫 이동을 로그로 남겼다 (Room::HandleMove)

	void	Reset() { lastClientMs = 0; leadUs = 0; announced = false; }

	// 이번 패킷에 쓸 경과 시간(초). serverDtUs = 직전 패킷부터 서버 수신(처리) 간격
	double	Interval(uint64 serverDtUs, uint64 clientMs)
	{
		if (clientMs == 0 || lastClientMs == 0 || clientMs < lastClientMs)
		{
			// 기준만 다시 잡고 쌓인 앞섬(leadUs)은 그대로 둔다. 0 으로 돌리던 때는 낮은 시각 · 높은 시각을 번갈아 보내면
			// 높은 패킷마다 상한(2초)을 새로 받아 속도가 사실상 무제한이었다 (2026-10-17 보안 점검 H1).
			// 정상 클라 시계는 거꾸로 가지 않는다 - 입장 · 재접속은 Reset 이 0 으로 돌린다
			lastClientMs = clientMs;
			return static_cast<double>(serverDtUs) / 1'000'000.0;
		}

		int64 clientDtUs = static_cast<int64>(clientMs - lastClientMs) * 1000;
		lastClientMs = clientMs;

		leadUs += clientDtUs - static_cast<int64>(serverDtUs);

		if (leadUs > MAX_LEAD_US)
		{
			clientDtUs -= (leadUs - MAX_LEAD_US);
			leadUs = MAX_LEAD_US;
			if (clientDtUs < 0)
				clientDtUs = 0;
		}

		if (leadUs < -MAX_LAG_US)
			leadUs = -MAX_LAG_US;

		return static_cast<double>(clientDtUs) / 1'000'000.0;
	}
};
