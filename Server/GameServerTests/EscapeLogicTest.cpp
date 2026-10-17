#include "pch.h"
#include "EscapeLogic.h"
#include "TowerTable.h"
#include "WallTable.h"
#include "Telemetry.h"
#include "ItemTable.h"
#include <thread>
#include <fstream>
#include <filesystem>
#include <chrono>

// design/gdd/escape-timer.md § Acceptance Criteria A - 이름 끝의 AC 번호가 그 행이다. 시계는 dtUs 로 주입한다.

namespace
{
	constexpr uint64 MS = 1000;
	constexpr uint64 SEC = 1'000'000;
	constexpr uint64 TICK = 100 * MS;

	EscapeConfig Defaults()
	{
		return EscapeConfig();
	}

	// 같은 점유자 수로 n 틱 - 완충한 틱 번호(1부터)를 돌려준다. 0 = 완충 없음
	int32 RunTicks(EscapeLogic::Gauge& g, int32 occupants, int32 ticks, const EscapeConfig& cfg)
	{
		for (int32 i = 1; i <= ticks; i++)
		{
			if (EscapeLogic::Step(g, occupants, TICK, cfg).completed)
				return i;
		}
		return 0;
	}
}

TEST(EscapeLogic, 점유자가_있으면_100ms_마다_채우고_600번째_틱에_완충한다_AC_ET_01)
{
	const EscapeConfig cfg = Defaults();
	EscapeLogic::Gauge g;

	for (int32 i = 1; i <= 599; i++)
	{
		const EscapeLogic::StepResult r = EscapeLogic::Step(g, 1, TICK, cfg);
		ASSERT_FALSE(r.completed) << i;
		ASSERT_EQ(g.progressUs, static_cast<uint64>(i) * TICK);
		ASSERT_EQ(g.state, Protocol::ESCAPE_GAUGE_FILLING);
	}

	const EscapeLogic::StepResult last = EscapeLogic::Step(g, 1, TICK, cfg);
	EXPECT_TRUE(last.completed);
	EXPECT_EQ(last.after, Protocol::ESCAPE_GAUGE_COMPLETED);
	EXPECT_EQ(g.progressUs, 0u);
	EXPECT_EQ(g.state, Protocol::ESCAPE_GAUGE_IDLE);
}

TEST(EscapeLogic, 인원과_상관없이_같은_틱에_완충한다_AC_ET_02)
{
	const EscapeConfig cfg = Defaults();
	for (int32 occupants : { 1, 5, 30 })
	{
		EscapeLogic::Gauge g;
		EXPECT_EQ(RunTicks(g, occupants, 1000, cfg), 600) << occupants;
	}
}

TEST(EscapeLogic, 아무도_없으면_2배로_역행하고_0에서_멈춘다_AC_ET_03)
{
	EscapeConfig cfg = Defaults();
	EscapeLogic::Gauge g;
	g.progressUs = 40 * SEC;
	g.state = Protocol::ESCAPE_GAUGE_FILLING;

	EscapeLogic::StepResult r = EscapeLogic::Step(g, 0, TICK, cfg);
	EXPECT_EQ(r.before, Protocol::ESCAPE_GAUGE_FILLING);
	EXPECT_EQ(r.after, Protocol::ESCAPE_GAUGE_DRAINING);

	RunTicks(g, 0, 99, cfg);
	EXPECT_EQ(g.progressUs, 20 * SEC);								// 100 틱 뒤 절반

	RunTicks(g, 0, 99, cfg);
	EXPECT_EQ(g.progressUs, 200 * MS);
	EXPECT_EQ(g.state, Protocol::ESCAPE_GAUGE_DRAINING);
	EscapeLogic::Step(g, 0, TICK, cfg);								// 200 번째 틱
	EXPECT_EQ(g.progressUs, 0u);
	EXPECT_EQ(g.state, Protocol::ESCAPE_GAUGE_IDLE);

	cfg.reverseMultX100 = 100;										// 1 배면 400 틱
	g.progressUs = 40 * SEC;
	RunTicks(g, 0, 399, cfg);
	EXPECT_EQ(g.progressUs, TICK);
	EscapeLogic::Step(g, 0, TICK, cfg);
	EXPECT_EQ(g.progressUs, 0u);
}

TEST(EscapeLogic, 40초_채우고_10초_비우면_20초가_남고_400틱에_완충한다_AC_ET_04)
{
	const EscapeConfig cfg = Defaults();
	EscapeLogic::Gauge g;
	RunTicks(g, 1, 400, cfg);										// 40 s
	RunTicks(g, 0, 100, cfg);										// 10 s 비움 → -20 s
	EXPECT_EQ(g.progressUs, 20 * SEC);
	EXPECT_EQ(RunTicks(g, 1, 1000, cfg), 400);
}

TEST(EscapeLogic, 한_틱에_250ms_넘게_반영하지_않는다_AC_ET_05)
{
	const EscapeConfig cfg = Defaults();
	EscapeLogic::Gauge g;
	EscapeLogic::Step(g, 1, 5 * SEC, cfg);
	EXPECT_EQ(g.progressUs, 250 * MS);

	g.progressUs = 10 * SEC;
	EscapeLogic::Step(g, 0, 5 * SEC, cfg);
	EXPECT_EQ(g.progressUs, 10 * SEC - 500 * MS);					// 역행 2 배도 상한 기준
}

TEST(EscapeLogic, 구역은_이력으로_판정한다_AC_ET_06)
{
	const EscapeConfig cfg = Defaults();
	EXPECT_TRUE(EscapeLogic::InZone(false, 1000.f, cfg));
	EXPECT_FALSE(EscapeLogic::InZone(false, 1000.01f, cfg));
	EXPECT_TRUE(EscapeLogic::InZone(true, 1100.f, cfg));
	EXPECT_FALSE(EscapeLogic::InZone(true, 1100.01f, cfg));
	EXPECT_FALSE(EscapeLogic::InZone(true, NAN, cfg));
	EXPECT_FALSE(EscapeLogic::InZone(false, INFINITY, cfg));
}

TEST(EscapeLogic, 받는_사람은_거리로_가시_경보_없음이_된다_AC_ET_18)
{
	const EscapeConfig cfg = Defaults();
	using A = EscapeLogic::Audience;
	EXPECT_EQ(EscapeLogic::AudienceFor(true, 0.f, cfg), A::Full);
	EXPECT_EQ(EscapeLogic::AudienceFor(false, 2499.f, cfg), A::Full);
	EXPECT_EQ(EscapeLogic::AudienceFor(false, 2500.f, cfg), A::Full);
	EXPECT_EQ(EscapeLogic::AudienceFor(false, 2501.f, cfg), A::Alert);
	EXPECT_EQ(EscapeLogic::AudienceFor(false, 4000.f, cfg), A::Alert);
	EXPECT_EQ(EscapeLogic::AudienceFor(false, 4001.f, cfg), A::None);
	EXPECT_EQ(EscapeLogic::AudienceFor(false, NAN, cfg), A::None);
}

TEST(EscapeLogic, 만료_골드는_30퍼센트이고_넘치지_않는다_AC_ET_09)
{
	struct Case { uint64 g; uint64 kept; };
	for (const Case& c : { Case{ 1000, 300 }, Case{ 7, 2 }, Case{ 3, 0 }, Case{ 99, 29 }, Case{ 100, 30 }, Case{ 0, 0 } })
		EXPECT_EQ(EscapeLogic::GoldKept(c.g, 30), c.kept) << c.g;

	// 곱해도 넘치지 않는 범위에서는 ⌊0.30 g⌋ 그대로
	for (uint64 g : { 123'456'789ull, 1ull << 40, 999'999'999'999ull })
		EXPECT_EQ(EscapeLogic::GoldKept(g, 30), g * 30 / 100) << g;

	// 2⁶⁰ · 2⁶⁴−1 - g × 30 은 넘치지만 결과는 g 보다 작고 ⌊0.30 g⌋ 의 정수 근사 범위 안 (오버플로 없음)
	for (uint64 g : { 1ull << 60, ~0ull })
	{
		const uint64 kept = EscapeLogic::GoldKept(g, 30);
		EXPECT_LT(kept, g);
		EXPECT_GE(kept, g / 100 * 30);
		EXPECT_LE(kept, g / 100 * 30 + 29);
	}
	EXPECT_EQ(EscapeLogic::GoldKept(500, 100), 500u);
	EXPECT_EQ(EscapeLogic::GoldKept(500, 0), 0u);
}

TEST(EscapeLogic, 체류_마감_시각_자체가_만료이고_남은_시간은_0에서_멈춘다_AC_ET_10)
{
	const uint64 enter = 100 * SEC;
	const uint64 deadline = enter + 1800 * SEC;
	EXPECT_FALSE(EscapeLogic::Expired(deadline, deadline - 1));
	EXPECT_TRUE(EscapeLogic::Expired(deadline, deadline));
	EXPECT_FALSE(EscapeLogic::Expired(0, deadline));				// 0 = 런 밖
	EXPECT_EQ(EscapeLogic::RemainingUs(deadline, enter), 1800 * SEC);
	EXPECT_EQ(EscapeLogic::RemainingUs(deadline, deadline + SEC), 0u);
}

TEST(EscapeLogic, 진행률과_속도가_상태에_맞다)
{
	const EscapeConfig cfg = Defaults();
	EscapeLogic::Gauge g;
	g.progressUs = 30 * SEC;
	EXPECT_EQ(EscapeLogic::Permille(g, cfg), 500u);
	EXPECT_FLOAT_EQ(EscapeLogic::RatePerSecond(Protocol::ESCAPE_GAUGE_FILLING, cfg), 1.f / 60.f);
	EXPECT_FLOAT_EQ(EscapeLogic::RatePerSecond(Protocol::ESCAPE_GAUGE_DRAINING, cfg), -2.f / 60.f);
	EXPECT_FLOAT_EQ(EscapeLogic::RatePerSecond(Protocol::ESCAPE_GAUGE_IDLE, cfg), 0.f);
}

TEST(EscapeConfig, 실제_설정_파일과_범위_검사_AC_ET_13_28)
{
	string error;
	EscapeConfig cfg;
	ASSERT_TRUE(cfg.Load(ItemTable::DefaultDataDir(), OUT error)) << error;
	EXPECT_FLOAT_EQ(cfg.zoneEnterCm, 667.f);		// 2026-10-17 사용자 결정 - 1000 의 2/3
	EXPECT_FLOAT_EQ(cfg.zoneLeaveCm, 733.f);
	EXPECT_EQ(cfg.fillUs, 60 * SEC);
	EXPECT_FLOAT_EQ(cfg.alertCm, 4000.f);
	EXPECT_EQ(cfg.runLimitUs, 1800 * SEC);
	EXPECT_FALSE(cfg.debugEscapeCommand);

	auto parse = [](const string& rows, OUT string& err)
	{
		EscapeConfig c;
		return c.Parse("key,value\n" + rows, OUT err) && c.Validate(OUT err);
	};

	EXPECT_TRUE(parse("zone_enter_radius_cm,1100\n", OUT error)) << error;
	EXPECT_TRUE(parse("zone_enter_radius_cm,500\n", OUT error)) << error;				// 하한
	EXPECT_FALSE(parse("zone_enter_radius_cm,499\n", OUT error));
	EXPECT_FALSE(parse("zone_leave_radius_cm,1201\n", OUT error));				// 무벽 원 12 m 밖
	EXPECT_FALSE(parse("zone_enter_radius_cm,1150\n", OUT error));				// enter > leave(1100)
	EXPECT_FALSE(parse("gauge_alert_radius_m,24\n", OUT error));				// R_alert < R_vis
	EXPECT_FALSE(parse("expiry_gold_kept_pct,101\n", OUT error));
	EXPECT_FALSE(parse("no_such_key,1\n", OUT error));
	EXPECT_FALSE(parse("escape_debug_command,maybe\n", OUT error));
}

TEST(TowerTable, 탈출_규칙_층당_3에서_4개_간격_해안선_검사_AC_ET_13)
{
	// 해안선은 WallTable 이 쥔다 - 다른 테스트가 남긴 표에 기대지 않게 해안선 없는 표로 시작한다
	string error;
	ASSERT_TRUE(WallTable::Parse(nullptr, nullptr, 4, {}, OUT error)) << error;

	ASSERT_TRUE(TowerTable::Load(ItemTable::DefaultDataDir(), 4, OUT error)) << error;
	EXPECT_TRUE(TowerTable::ValidateForEscape(1100.f, 4000.f, 4, OUT error)) << error;		// 실제 1층 (섬 - 4개, 172 m 이상 간격 > 80 m)

	ASSERT_TRUE(TowerTable::Parse("tower_id,floor_id,x,y\nTa,0,0,0\nTb,0,10000,0\n", 4, OUT error));
	EXPECT_FALSE(TowerTable::ValidateForEscape(1100.f, 4000.f, 4, OUT error));				// 2개
	EXPECT_NE(error.find("3 to 4"), string::npos) << error;

	ASSERT_TRUE(TowerTable::Parse("tower_id,floor_id,x,y\nTa,0,0,0\nTb,0,7000,0\nTc,0,-9000,0\n", 4, OUT error));
	EXPECT_FALSE(TowerTable::ValidateForEscape(1100.f, 4000.f, 4, OUT error));				// 70 m < 2 × 40 m

	// 섬 1층처럼 4개는 된다 (게이지 배열 Player::MAX_TOWERS = 4), 5개는 안 된다
	const string four = "tower_id,floor_id,x,y\nTa,0,0,0\nTb,0,10000,0\nTc,0,-10000,0\nTd,0,0,15000\n";
	ASSERT_TRUE(TowerTable::Parse(four, 4, OUT error)) << error;
	EXPECT_TRUE(TowerTable::ValidateForEscape(1100.f, 4000.f, 4, OUT error)) << error;
	ASSERT_TRUE(TowerTable::Parse(four + "Te,0,0,-15000\n", 4, OUT error)) << error;
	EXPECT_FALSE(TowerTable::ValidateForEscape(1100.f, 4000.f, 4, OUT error));

	// 해안선이 있으면 그 안쪽 12 m - 북쪽 해안 x = 11,000 에서 10 m 인 Tb 는 거부
	const string coast = "floor_id,seq,x,y\n0,0,11000,-20000\n0,1,11000,20000\n0,2,-11000,20000\n0,3,-11000,-20000\n";
	ASSERT_TRUE(WallTable::Parse(nullptr, nullptr, 4, {}, OUT error, &coast)) << error;
	ASSERT_TRUE(TowerTable::Parse(four, 4, OUT error)) << error;
	EXPECT_FALSE(TowerTable::ValidateForEscape(1100.f, 4000.f, 4, OUT error));
	EXPECT_NE(error.find("tower Tb"), string::npos) << error;
	ASSERT_TRUE(TowerTable::Parse("tower_id,floor_id,x,y\nTa,0,0,0\nTb,0,9800,0\nTc,0,-9800,0\nTd,0,0,15000\n", 4, OUT error)) << error;
	EXPECT_TRUE(TowerTable::ValidateForEscape(1100.f, 4000.f, 4, OUT error)) << error;		// 12 m 딱

	ASSERT_TRUE(TowerTable::Parse("tower_id,floor_id,x,y\n", 4, OUT error));
	EXPECT_TRUE(TowerTable::ValidateForEscape(1100.f, 4000.f, 4, OUT error));				// TEMP 층 - 타워 없음

	ASSERT_TRUE(WallTable::Parse(nullptr, nullptr, 4, {}, OUT error)) << error;				// 다음 테스트에 남기지 않는다
}

TEST(Telemetry, 런_한_줄은_22열이다_AC_ET_15)
{
	RunRow row;
	row.charId = 7;
	row.reason = "EXPIRED";
	row.lastTowerId = "Tb";
	const string line = Telemetry::FormatRun(1, 2, 3, row);

	const string header = Telemetry::RUNS_HEADER;
	EXPECT_EQ(count(line.begin(), line.end(), ',') + 1, 22);
	EXPECT_EQ(count(header.begin(), header.end(), ',') + 1, 22);
	EXPECT_EQ(line.find('\n'), string::npos);
}

TEST(Telemetry, 네_스레드가_동시에_써도_깨진_줄이_없다_AC_ET_16)
{
	// 방 4개 = 스레드 4개. 각 2,500줄 → 10,000줄 + 헤더, 줄마다 7열이어야 한다
	const string dir = ItemTable::DefaultDataDir() + "\\..\\Logs\\telemetry-test";
	{
		Telemetry t;
		ASSERT_TRUE(t.Open(dir));

		vector<thread> threads;
		for (int32 n = 0; n < 4; n++)
		{
			threads.emplace_back([&t, n]()
				{
					for (int32 i = 0; i < 2500; i++)
						t.AppendGauge(static_cast<uint32>(n), "Ta", "FILL_START", static_cast<uint32>(i % 1000), 1);
				});
		}
		for (thread& th : threads)
			th.join();
	}	// 파일이 닫힌다

	string newest;
	for (const auto& entry : std::filesystem::directory_iterator(dir))
	{
		const string name = entry.path().string();
		if (entry.path().filename().string().rfind("gauge-", 0) == 0 && name > newest)
			newest = name;
	}
	ASSERT_FALSE(newest.empty());

	ifstream in(newest);
	string line;
	int32 lines = 0;
	int32 broken = 0;
	while (getline(in, line))
	{
		lines++;
		if (count(line.begin(), line.end(), ',') != 6)
			broken++;
	}
	EXPECT_EQ(lines, 10'001);
	EXPECT_EQ(broken, 0);
}

TEST(Telemetry, 디스크가_멈춰도_Append_는_기다리지_않고_닫을_때_전부_쓴다)
{
	// 회귀: 예전에는 Room 잡 안에서 ofstream 에 쓰고 flush 했다 - 쓰기가 멈추면 그 방이 같이 멈췄다 (G2 동기 로그와 같은 길)
	// 쓰기 스레드를 묶음마다 300 ms 멈춰 두고 1,000 줄을 넣는다. 동기 쓰기라면 최소 300 ms 가 걸린다.
	const string dir = ItemTable::DefaultDataDir() + "\\..\\Logs\\telemetry-test-stall";
	chrono::steady_clock::duration appendTime{};
	{
		Telemetry t;
		ASSERT_TRUE(t.Open(dir));
		t.SetWriteStallForTest(300);

		const auto start = chrono::steady_clock::now();
		for (int32 i = 0; i < 1000; i++)
			t.AppendGauge(0, "Ta", "FILL_START", static_cast<uint32>(i), 1);
		appendTime = chrono::steady_clock::now() - start;
	}	// 소멸자가 남은 줄을 다 쓰고 닫는다

	EXPECT_LT(chrono::duration_cast<chrono::milliseconds>(appendTime).count(), 100) << "Append 가 쓰기를 기다렸다";

	string newest;
	for (const auto& entry : std::filesystem::directory_iterator(dir))
	{
		const string name = entry.path().string();
		if (entry.path().filename().string().rfind("gauge-", 0) == 0 && name > newest)
			newest = name;
	}
	ASSERT_FALSE(newest.empty());

	ifstream in(newest);
	string line;
	int32 lines = 0;
	while (getline(in, line))
		lines++;
	EXPECT_EQ(lines, 1'001) << "닫을 때 큐에 남은 줄을 버렸다";
}
