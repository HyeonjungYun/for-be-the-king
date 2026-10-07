#pragma once
#include <cmath>

/*--------------
	FloorBounds
--------------*/

// 층 사각형 경계 (design/gdd/level-map.md Core Rule 1 · 3, AC-LM-02).
//
// 한 층 = 280 m × 280 m, 원점 = 층 중심 → x, y ∈ [-14,000, +14,000] cm. 경계 그 자체(14,000)는 안이다.
// 서버는 이 밖으로 가는 C_MOVE 를 받지 않는다 - 이동 검증 거부와 같은 처리(되돌림, 브로드캐스트 없음).
//
// 🔴 NaN · ±Inf 도 여기서 걸러진다. 속도 검증은 `distance > budget` 비교라 NaN 이면 거짓이 되어 그대로
//    통과했다 - 클라가 NaN 좌표를 보내면 위치가 NaN 이 됐다 (2026-10-05 발견).
namespace FloorBounds
{
	constexpr float	HALF_CM = 14000.f;		// registry floor_size 280 m

	inline bool Contains(float x, float y)
	{
		return std::isfinite(x) && std::isfinite(y) && std::fabs(x) <= HALF_CM && std::fabs(y) <= HALF_CM;
	}
}
