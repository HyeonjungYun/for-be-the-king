#include "pch.h"
#include "MovementValidator.h"
#include "MoveClock.h"
#include "FloorBounds.h"

// 이동 검증 규칙 (MovementValidator.h - Room::HandleMove 에서 떼어낸 순수 함수, 2026-10-21).
// ADR-0007 § ② 3 ~ 7: 예산 적립 × 1.15 · 상한 0.5 s 분량 · 층 밖/NaN 거부 · 거리 > 예산 거부 · 벽 거부 · 수락 시 차감

namespace
{
	constexpr double CEILING = 600.0;			// cm/s (맨몸 이속 근처)
	constexpr double TICK = 0.033;				// 30 Hz
	constexpr double MARGIN = MoveRules::VALIDATION_MARGIN;

	const std::function<bool(float, float)> OpenFloor = [](float x, float y) { return FloorBounds::Contains(x, y); };
	const std::function<bool(float, float, float, float)> NoWalls = [](float, float, float, float) { return false; };

	MoveCheckInput Step(double fromX, double toX, double budget, double deltaSec = TICK)
	{
		MoveCheckInput in;
		in.fromX = fromX;
		in.fromY = 0.0;
		in.toX = toX;
		in.toY = 0.0;
		in.deltaSec = deltaSec;
		in.speedCeiling = CEILING;
		in.budget = budget;
		return in;
	}
}

TEST(MovementValidator, 한_틱_예산보다_멀리_가면_속도_거부이고_예산은_차감되지_않는다)
{
	// 예산 0 에서 33 ms - 적립 = 600 × 1.15 × 0.033 ≈ 22.8 cm. 100 cm 를 주장하면 거부
	const MoveCheckResult r = CheckMove(Step(0.0, 100.0, 0.0), OpenFloor, NoWalls);
	EXPECT_EQ(r.verdict, MoveVerdict::RejectSpeed);
	EXPECT_NEAR(r.budget, CEILING * MARGIN * TICK, 1e-9);		// 적립만 남고 차감은 없다
	EXPECT_DOUBLE_EQ(r.distance, 100.0);
}

TEST(MovementValidator, 예산_안의_이동은_수락되고_간_만큼_차감된다)
{
	const MoveCheckResult r = CheckMove(Step(0.0, 20.0, 0.0), OpenFloor, NoWalls);
	EXPECT_EQ(r.verdict, MoveVerdict::Accept);
	EXPECT_NEAR(r.budget, CEILING * MARGIN * TICK - 20.0, 1e-9);
}

TEST(MovementValidator, 예산은_상한의_반_초_분량을_넘지_않는다)
{
	// 30 초 가만히 있어도 순간이동할 예산이 쌓이지 않는다
	const MoveCheckResult r = CheckMove(Step(0.0, 0.0, 1e9, 30.0), OpenFloor, NoWalls);
	EXPECT_EQ(r.verdict, MoveVerdict::Accept);
	EXPECT_NEAR(r.budget, MoveRules::FullBudget(CEILING), 1e-9);
	EXPECT_NEAR(MoveRules::FullBudget(CEILING), CEILING * MARGIN * MoveRules::MAX_BUDGET_SEC, 1e-9);
}

TEST(MovementValidator, 몰려_도착한_패킷도_MoveClock_간격이면_전부_수락된다)
{
	// soak2 SV-3 109 건의 상황 - 상한 속도로 달리던 쪽이 1 초 멈췄다가 30 개를 한꺼번에 보냈다 (한 패킷 = 33 ms 거리). 서버 처리 간격은 0 에 가깝다.
	// client_ms 가 있으면 MoveClock 이 패킷마다 33 ms 를 주고, 그 적립(× 1.15)으로 30 개가 다 들어간다
	constexpr int32 BURST = 30;
	const double step = CEILING * TICK;					// 19.8 cm - 상한 속도로 33 ms

	MoveClock clock;
	clock.Interval(33'000, 1'000'000);					// 기준 패킷

	double budget = 0.0;
	double x = 0.0;
	int32 accepted = 0;
	for (uint64 i = 1; i <= BURST; i++)
	{
		const double dt = clock.Interval((i == 1) ? BURST * 33'000 : 0, 1'000'000 + i * 33);
		const MoveCheckResult r = CheckMove(Step(x, x + step, budget, dt), OpenFloor, NoWalls);
		budget = r.budget;
		if (r.verdict == MoveVerdict::Accept)
		{
			accepted++;
			x += step;
		}
	}
	EXPECT_EQ(accepted, BURST);

	// 같은 상황에서 client_ms 가 없으면(구버전 - 서버 간격) 첫 패킷이 1 초치를 적립해도 상한(0.5 s 분량)에 잘려 뒤쪽이 거부된다 - MoveClock 이 있는 이유
	budget = 0.0;
	x = 0.0;
	accepted = 0;
	for (int32 i = 1; i <= BURST; i++)
	{
		const double dt = (i == 1) ? BURST * TICK : 0.0;
		const MoveCheckResult r = CheckMove(Step(x, x + step, budget, dt), OpenFloor, NoWalls);
		budget = r.budget;
		if (r.verdict == MoveVerdict::Accept)
		{
			accepted++;
			x += step;
		}
	}
	EXPECT_LT(accepted, BURST);
	EXPECT_GE(accepted, 15);		// 상한 345 cm ÷ 19.8 ≈ 17 개까지는 들어간다
}

TEST(MovementValidator, NaN_무한대_층_밖은_경계_거부이고_예산을_쓰지_않는다)
{
	MoveCheckInput nan = Step(0.0, 10.0, 0.0);
	nan.toX = std::numeric_limits<double>::quiet_NaN();
	const MoveCheckResult r1 = CheckMove(nan, OpenFloor, NoWalls);
	EXPECT_EQ(r1.verdict, MoveVerdict::RejectBounds);
	EXPECT_NEAR(r1.budget, CEILING * MARGIN * TICK, 1e-9);

	MoveCheckInput inf = Step(0.0, 10.0, 0.0);
	inf.toY = std::numeric_limits<double>::infinity();
	EXPECT_EQ(CheckMove(inf, OpenFloor, NoWalls).verdict, MoveVerdict::RejectBounds);

	const MoveCheckResult outside = CheckMove(Step(0.0, FloorBounds::HALF_CM + 1.0, 1e9), OpenFloor, NoWalls);
	EXPECT_EQ(outside.verdict, MoveVerdict::RejectBounds);
}

TEST(MovementValidator, 벽을_지나면_벽_거부이고_벽_검사는_속도를_통과한_뒤에만_한다)
{
	int32 sweeps = 0;
	const std::function<bool(float, float, float, float)> wallEverywhere = [&](float, float, float, float) { sweeps++; return true; };

	// 속도 안 + 벽 → 벽 거부, 예산은 그대로
	const MoveCheckResult wall = CheckMove(Step(0.0, 20.0, 100.0), OpenFloor, wallEverywhere);
	EXPECT_EQ(wall.verdict, MoveVerdict::RejectWall);
	EXPECT_EQ(sweeps, 1);
	EXPECT_NEAR(wall.budget, 100.0 + CEILING * MARGIN * TICK, 1e-9);

	// 속도 초과면 벽 질의를 하지 않는다 (격자 칸 수를 묶는다 - ADR-0006 § ⑥)
	const MoveCheckResult speed = CheckMove(Step(0.0, 5000.0, 0.0), OpenFloor, wallEverywhere);
	EXPECT_EQ(speed.verdict, MoveVerdict::RejectSpeed);
	EXPECT_EQ(sweeps, 1);

	// 벽을 지나는 이동 스킬 창(checkWalls false)이면 벽이 있어도 수락
	MoveCheckInput ghost = Step(0.0, 20.0, 100.0);
	ghost.checkWalls = false;
	EXPECT_EQ(CheckMove(ghost, OpenFloor, wallEverywhere).verdict, MoveVerdict::Accept);
	EXPECT_EQ(sweeps, 1);
}
