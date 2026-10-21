#include "pch.h"
#include "MovementValidator.h"
#include <cmath>

/*--------------------
	MovementValidator
---------------------*/

MoveCheckResult CheckMove(const MoveCheckInput& in,
	const std::function<bool(float, float)>& insideFloor,
	const std::function<bool(float, float, float, float)>& sweepBlocked)
{
	MoveCheckResult out;

	// 1. 적립 - 지터 몫(1.15)을 얹은 상한으로, 상한의 0.5 초 분량까지만
	const double ceiling = in.speedCeiling * MoveRules::VALIDATION_MARGIN;
	double budget = in.budget + ceiling * in.deltaSec;
	const double maxBudget = ceiling * MoveRules::MAX_BUDGET_SEC;
	if (budget > maxBudget)
		budget = maxBudget;
	out.budget = budget;

	// 2. 층 밖 · NaN · 무한대 - 예산을 쓰지 않고 거부 (FloorBounds.h · WallMap::InsideFloor, level-map.md AC-LM-02)
	if (insideFloor(static_cast<float>(in.toX), static_cast<float>(in.toY)) == false)
	{
		out.verdict = MoveVerdict::RejectBounds;
		return out;
	}

	// 3. XY 평면 거리가 예산을 넘는다
	const double dx = in.toX - in.fromX;
	const double dy = in.toY - in.fromY;
	out.distance = std::sqrt(dx * dx + dy * dy);
	if (out.distance > budget)
	{
		out.verdict = MoveVerdict::RejectSpeed;
		return out;
	}

	// 4. 벽 - 속도 다음이다. 속도 검사가 선분 길이를 묶어 둬야 벽 질의가 보는 격자 칸 수가 묶인다 (ADR-0006 § ⑥)
	if (in.checkWalls && sweepBlocked(static_cast<float>(in.fromX), static_cast<float>(in.fromY), static_cast<float>(in.toX), static_cast<float>(in.toY)))
	{
		out.verdict = MoveVerdict::RejectWall;
		return out;
	}

	// 5. 수락 - 간 만큼 차감
	out.budget = budget - out.distance;
	out.verdict = MoveVerdict::Accept;
	return out;
}
