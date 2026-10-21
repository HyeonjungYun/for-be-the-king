#pragma once
#include <algorithm>
#include <cmath>

/*--------------
	DamageMath
---------------*/

// 피해 · CC 시간 · 발 수의 순수 산술 (2026-10-21). Room 없이 테스트한다 - GameServerTests DamageMathTest.
//
// 왜 따로 두나: 예전엔 배율 합(1 + Σ damage_mod)에 하한이 없어 음수 행 하나면 피해가 음수가 됐고(클라는 음수 damage 를 회복으로 그린다),
// 저항이 1 을 넘으면 음수 float 를 uint32 로 캐스트했다(미정의 동작 - 수십억 ms CC). 데이터가 어떻든 여기서 0 아래로 내려가지 않는다
namespace DamageMath
{
	// 배율의 하한은 0 - 음수 배율은 "피해 없음" 이지 "회복" 이 아니다
	inline float ClampMultiplier(float mult)
	{
		return (mult < 0.f) ? 0.f : mult;
	}

	// damage × mult 를 반올림한 정수. 0 이상
	inline int32 Scale(int32 damage, float mult)
	{
		const long scaled = ::lroundf(static_cast<float>(damage) * ClampMultiplier(mult));
		return (scaled < 0) ? 0 : static_cast<int32>(scaled);
	}

	// 시간 × 배율 (내림). 배율 하한 0
	inline uint32 ScaleMs(uint32 baseMs, float mult)
	{
		return static_cast<uint32>(static_cast<float>(baseMs) * ClampMultiplier(mult));
	}

	// 저항(0 ~ 1)만큼 줄어든 시간 - 1 을 넘으면 0, 음수는 저항 없음. 음수 float → uint32 캐스트를 막는다
	inline uint32 ResistedMs(uint32 baseMs, float resist)
	{
		const float r = std::clamp(resist, 0.f, 1.f);
		return static_cast<uint32>(static_cast<float>(baseMs) * (1.f - r));
	}

	// 수치 저항 (resistance × 최대 감소율, Creature::ComputeCcDurationMs) - 같은 규칙
	inline uint32 MitigatedMs(uint32 baseMs, float resistance, float maxReduction)
	{
		return ResistedMs(baseMs, resistance * maxReduction);
	}

	// 기본 수 + 레벨당 증가 × 레벨, 상한 cap. 증가분이 음수면 더하지 않는다 (음수 float → uint32 캐스트를 막는다)
	inline uint32 ScaledCount(uint32 base, float perLevel, uint32 level, uint32 cap)
	{
		float extra = perLevel * static_cast<float>(level);
		if (extra < 0.f)
			extra = 0.f;
		return (std::min)(cap, base + static_cast<uint32>(extra));
	}
}
