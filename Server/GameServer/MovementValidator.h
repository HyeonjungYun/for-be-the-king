#pragma once
#include <functional>

/*--------------------
	MovementValidator
---------------------*/

// 이동 검증의 순수 규칙 (ADR-0007 § ② 3 ~ 7 · TR-movement-011 · 012) - Room 없이 테스트한다 (GameServerTests MovementValidatorTest, 2026-10-21).
// Room::HandleMove 는 입력(자리 · 경과 · 속도 상한 · 예산 · 벽 지도)을 모아 넘기고, 결과로 위반 기록 · 로그 · 보정 S_MOVE 를 한다.
// 규칙 자체는 여기 하나뿐이다 - 예전엔 HandleMove 본문에 박혀 있어 봇 부하로만 검증됐다.
//
//   1. 예산 += 상한 × VALIDATION_MARGIN × 경과,  예산 ≤ 상한 × VALIDATION_MARGIN × MAX_BUDGET_SEC
//   2. 층 밖 · NaN · 무한대                         → RejectBounds (예산은 적립만, 차감 없음)
//   3. 거리 > 예산                                   → RejectSpeed  (예산 그대로)
//   4. 벽을 지난다 (checkWalls 일 때)                 → RejectWall   (예산 그대로)
//   5. 수락 - 예산 -= 거리
namespace MoveRules
{
	constexpr double VALIDATION_MARGIN = 1.15;		// 지터 15 % (validation_margin)
	constexpr double MAX_BUDGET_SEC = 0.5;			// 적립 상한 - 30 초 가만히 있다가 순간이동하지 못하게

	// 입장 때 주는 예산 - 가득 (EnterRoom. 예전엔 0 이라 입장 직후 첫 이동이 거부됐다 - README § 4-4)
	inline double FullBudget(double speedCeiling) { return speedCeiling * VALIDATION_MARGIN * MAX_BUDGET_SEC; }
}

enum class MoveVerdict : uint8
{
	Accept = 0,
	RejectBounds,	// 층 밖 · NaN · 무한대
	RejectSpeed,	// 거리 > 예산
	RejectWall,		// 벽을 지난다
};

struct MoveCheckInput
{
	double	fromX = 0.0;			// 서버가 아는 자리
	double	fromY = 0.0;
	double	toX = 0.0;				// 클라가 주장하는 자리
	double	toY = 0.0;
	double	deltaSec = 0.0;			// 경과 (MoveClock::Interval)
	double	speedCeiling = 0.0;		// 보낸 때의 속도 상한 (cm/s) - 배율 전 (Player::GetSpeedCeiling)
	double	budget = 0.0;			// 지금 예산 (cm)
	bool	checkWalls = true;		// 벽 검사를 하나 - 벽을 지나는 이동 스킬 창 · FBTK_WALLS=0 이면 false
};

struct MoveCheckResult
{
	MoveVerdict	verdict = MoveVerdict::Accept;
	double		budget = 0.0;		// 적립 · 차감 뒤 예산 - 어느 판정이든 부르는 쪽이 그대로 저장한다
	double		distance = 0.0;		// XY 평면 이동 거리 (경계 거부면 0)
};

// insideFloor(x, y) = WallMap::InsideFloor · sweepBlocked(x0, y0, x1, y1) = WallMap::SweepBlocked(r = 0). 벽 질의는 속도를 통과한 뒤에만 부른다
MoveCheckResult CheckMove(const MoveCheckInput& in,
	const std::function<bool(float, float)>& insideFloor,
	const std::function<bool(float, float, float, float)>& sweepBlocked);
