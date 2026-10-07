#pragma once
#include <cmath>

/*-----------------
	SpawnPlacement
-----------------*/

// 진입 지점 둘레의 빈자리 (design/ux/entry-point.md AC-EP-02 · design/gdd/level-map.md Core Rule 7 · Edge Cases).
//
// 지점 중심을 피해 150 cm 간격 고리 6겹(6 · 12 · … · 36칸 = 126칸)을 차례로 돌며 비어 있는 첫 칸에 놓는다.
// 다 찼으면 반경 900 cm **원** 안 무작위 - 레벨은 지점 중심 900 cm 안에 벽을 두지 않으므로(Core Rule 7) 이 원 안이면
// 벽에 박히지 않는다.
//
// 🔴 예전 폴백은 ±900 cm 정사각형이라 모서리가 1,273 cm 까지 나가 무벽 원을 벗어났다 (2026-10-05 수정).
//    층 정원 30명이라 126칸이 다 차는 일은 없지만, 일어나면 벽 안에 놓인다.
//
// 칸 점유 판정 · 난수는 호출자가 넘긴다 - 테스트가 둘 다 마음대로 정할 수 있게 (AC-LM-10).
namespace SpawnPlacement
{
	constexpr float		SPACING_CM = 150.f;
	constexpr int32		MAX_RINGS = 6;
	constexpr float		MAX_RADIUS_CM = SPACING_CM * MAX_RINGS;	// 900 - level-map entry_clear_radius 와 같아야 한다
	constexpr float		PI_F = 3.14159265f;

	// occupied(x, y) -> bool : 그 자리에 이미 누가 SPACING_CM 안에 있는가
	// rand01() -> float [0, 1) : 폴백에만 쓴다
	template <typename OccupiedFn, typename Rand01Fn>
	void Find(float baseX, float baseY, OccupiedFn&& occupied, Rand01Fn&& rand01, float& outX, float& outY)
	{
		for (int32 ring = 1; ring <= MAX_RINGS; ring++)
		{
			const int32 slots = ring * 6;
			const float radius = ring * SPACING_CM;

			for (int32 i = 0; i < slots; i++)
			{
				const float angle = (2.f * PI_F * i) / slots;
				const float cx = baseX + radius * std::cos(angle);
				const float cy = baseY + radius * std::sin(angle);

				if (occupied(cx, cy) == false)
				{
					outX = cx;
					outY = cy;
					return;
				}
			}
		}

		// 원 안 균등 분포 - 반지름에 sqrt 를 씌워야 가장자리로 몰리지 않는다
		const float r = MAX_RADIUS_CM * std::sqrt(rand01());
		const float a = 2.f * PI_F * rand01();
		outX = baseX + r * std::cos(a);
		outY = baseY + r * std::sin(a);
	}
}
