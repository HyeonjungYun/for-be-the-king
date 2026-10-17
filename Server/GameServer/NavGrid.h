#pragma once
#include "WallMap.h"
#include <functional>

/*-----------
	NavGrid
-----------*/

// 몬스터 길찾기 격자 (design/gdd/monster-ai.md § K · OQ-2 - 2026-10-17 사용자 선택 "길찾기 (A*)").
//
// 층 하나의 1 m 칸마다 가장 가까운 벽까지의 여유(cm)를 든다. 반지름 r 인 몸은 여유 ≥ r + CLEARANCE_MARGIN_CM 인 칸만 지난다.
// 부팅 때 WallTable 다음에 한 번 만들고 이후 읽기 전용 - 여러 Room 이 락 없이 함께 읽는다.
// 임시 벽(지각변동 1122)은 모른다 - 걸음마다 하는 벽 쓸기가 막고, 막히면 다시 구한다.
//
// FindPath = 8 방향 A* (모서리 자르기 금지) → 정확한 벽 쓸기로 당겨 편 꺾는 점.
// 탐색은 출발 · 도착의 중점을 둘러싼 창(WINDOW_CELLS²) 안에서, 확장 MAX_EXPANSIONS 까지만 - 넘으면 실패이고
// 몬스터는 예전처럼 직선 + 미끄러짐으로 간다 (막히면 T3 로 포기). 순수 로직이라 GameServerTests NavGridTest 가 합성 벽으로 돌린다.

// a -> b 로 반지름 r 원을 밀 때 벽에 닿는가 (WallMap::SweepBlocked) - 당겨 펴기가 쓴다
using NavSweepFn = function<bool(float ax, float ay, float bx, float by, float r)>;

class NavGrid
{
public:
	static constexpr float	CELL_CM				= 100.f;
	static constexpr int32	GRID_N				= static_cast<int32>(2.f * FloorBounds::HALF_CM / CELL_CM);	// 60,000 / 100 = 600
	static constexpr float	CLEARANCE_CAP_CM	= 400.f;		// 이보다 먼 벽은 재지 않는다 (가장 큰 몸 150 + 여유)
	static constexpr float	CLEARANCE_MARGIN_CM	= 10.f;			// 칸 중심이 몸 반지름보다 이만큼은 벽에서 떨어져야 한다
	static constexpr int32	WINDOW_CELLS		= 160;			// 탐색 창 한 변 (160 m) - 출발과 도착이 이 안에 같이 들어야 한다
	static constexpr int32	MAX_EXPANSIONS		= 6000;			// 넘으면 실패 - 한 번의 비용 상한
	static constexpr int32	SNAP_CELLS			= 3;			// 출발 · 도착 칸이 막혔으면 이 안의 가장 가까운 지나갈 수 있는 칸에서

	static_assert(GRID_N * CELL_CM == 2.f * FloorBounds::HALF_CM, "the nav grid must tile the floor square exactly");

	// 벽 · 해안선으로 칸마다 여유를 잰다. 벽이 0 개면 만들지 않고 false (그 층은 길찾기 없음 - 막을 것이 없다)
	bool	Build(const WallMap& walls);
	bool	IsBuilt() const { return _clear.empty() == false; }

	// 칸의 여유 (cm). 층 밖 · 바다 · 벽 안은 0
	float	ClearanceAt(float x, float y) const;
	bool	Walkable(float x, float y, float radiusCm) const;
	uint32	WalkableCells(float radiusCm) const;

	struct PathResult
	{
		bool						found = false;
		vector<pair<float, float>>	points;			// 꺾는 점 - 출발점은 빼고, 마지막 = 도착점 (to)
		int32						expansions = 0;
		const char*					failReason = nullptr;	// window · start · goal · budget · none
	};

	// (from) → (to) 반지름 r 몸의 길. sweep = 당겨 펴기의 정확한 벽 검사 (nullptr 이면 칸 중심을 그대로 쓴다)
	PathResult	FindPath(float fromX, float fromY, float toX, float toY, float radiusCm, const NavSweepFn& sweep) const;

	static int32	CellOf(float v);
	static float	CentreOf(int32 cell);

private:
	bool	WalkableCell(int32 gx, int32 gy, float need) const;
	bool	Snap(int32& gx, int32& gy, float need) const;

	vector<uint16>	_clear;		// GRID_N² - 여유 cm (CLEARANCE_CAP_CM 상한)
};

// 층마다 하나 - 부팅 때 WallTable 다음에 만든다
namespace NavGridTable
{
	// 벽이 있는 층만. 돌려주는 값 = 만든 층 수
	uint32			Build(uint32 floorCount);
	const NavGrid*	Get(uint32 floorId);		// 없으면 nullptr
	void			Clear();
}
