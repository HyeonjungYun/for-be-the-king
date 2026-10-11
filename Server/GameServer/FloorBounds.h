#pragma once
#include <cmath>

/*--------------
	FloorBounds
--------------*/

// 층 바깥 안전 사각형 (design/gdd/level-map.md Core Rule 1 · 3, AC-LM-02 · ADR-0013).
//
// 원점 = 층 중심 → x, y ∈ [-30,000, +30,000] cm. 경계 그 자체(30,000)는 안이다.
// 2026-10-11 섬 맵부터 층의 진짜 안은 해안선 다각형이다 (floor_outline.csv → WallMap::InsideFloor). 이 사각형은
// 그것을 감싸는 상한이고 - 섬 589 × 306 m 가 들어간다 - 해안선이 없는 층(2~4층 TEMP)에서는 이것이 곧 층이다.
// 서버는 밖으로 가는 C_MOVE 를 받지 않는다 - 이동 검증 거부와 같은 처리(되돌림, 브로드캐스트 없음).
//
// 🔴 NaN · ±Inf 도 여기서 걸러진다. 속도 검증은 `distance > budget` 비교라 NaN 이면 거짓이 되어 그대로
//    통과했다 - 클라가 NaN 좌표를 보내면 위치가 NaN 이 됐다 (2026-10-05 발견).
namespace FloorBounds
{
	constexpr float	HALF_CM = 30000.f;		// 600 m 정사각형 - 섬(동서 589 m)을 감싼다. 옛 280 m 층은 14,000

	inline bool Contains(float x, float y)
	{
		return std::isfinite(x) && std::isfinite(y) && std::fabs(x) <= HALF_CM && std::fabs(y) <= HALF_CM;
	}
}
