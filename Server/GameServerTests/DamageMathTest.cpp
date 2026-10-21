#include "pch.h"
#include "DamageMath.h"

// 피해 · CC 시간 산술의 하한 (DamageMath.h, 2026-10-21 리뷰 후속).
// 예전 Room::ApplyDamageMods 는 배율 합에 하한이 없어 음수 damage_mod 행이면 피해가 음수(= 클라 회복 표시)였고,
// ApplyCc · Creature::ComputeCcDurationMs 는 저항이 1 을 넘으면 음수 float 를 uint32 로 캐스트했다 (미정의 동작)

TEST(DamageMath, 배율_합이_음수면_피해는_0이다_회복이_아니다)
{
	EXPECT_EQ(DamageMath::Scale(100, -0.5f), 0);
	EXPECT_EQ(DamageMath::Scale(100, -10.f), 0);
	EXPECT_FLOAT_EQ(DamageMath::ClampMultiplier(-1.f), 0.f);
}

TEST(DamageMath, 양수_배율은_반올림해_그대로_곱한다)
{
	EXPECT_EQ(DamageMath::Scale(100, 0.5f), 50);
	EXPECT_EQ(DamageMath::Scale(100, 1.25f), 125);
	EXPECT_EQ(DamageMath::Scale(7, 0.5f), 4);		// 3.5 → 4 (lroundf)
	EXPECT_EQ(DamageMath::Scale(0, 3.f), 0);
	EXPECT_FLOAT_EQ(DamageMath::ClampMultiplier(1.5f), 1.5f);
}

TEST(DamageMath, 저항이_1을_넘으면_CC_시간은_0이다_음수_캐스트가_아니다)
{
	EXPECT_EQ(DamageMath::ResistedMs(1000, 1.5f), 0u);
	EXPECT_EQ(DamageMath::ResistedMs(1000, 1.f), 0u);
	EXPECT_EQ(DamageMath::ResistedMs(1000, 0.3f), 700u);
	EXPECT_EQ(DamageMath::ResistedMs(1000, -0.2f), 1000u);		// 음수 저항은 "없음"
	EXPECT_EQ(DamageMath::ResistedMs(1000, 0.f), 1000u);
}

TEST(DamageMath, 수치_저항은_최대_감소율을_곱한_뒤_같은_규칙이다)
{
	// Creature::ComputeCcDurationMs - resistance × RESISTANCE_MAX_REDUCTION(0.4)
	EXPECT_EQ(DamageMath::MitigatedMs(1000, 1.f, 0.4f), 600u);
	EXPECT_EQ(DamageMath::MitigatedMs(1000, 0.5f, 0.4f), 800u);
	EXPECT_EQ(DamageMath::MitigatedMs(1000, 5.f, 0.4f), 0u);		// 저항 2.0 → 0 (예전엔 음수 → uint32 미정의)
}

TEST(DamageMath, 시간_배율도_하한_0이다)
{
	EXPECT_EQ(DamageMath::ScaleMs(1000, 0.5f), 500u);
	EXPECT_EQ(DamageMath::ScaleMs(1000, -1.f), 0u);
	EXPECT_EQ(DamageMath::ScaleMs(1000, 1.f), 1000u);
}

TEST(DamageMath, 레벨당_증가가_음수면_더하지_않고_상한을_지킨다)
{
	// RoomSkills2 StartVolley - hits + per_level × 강화 레벨, 상한 12
	EXPECT_EQ(DamageMath::ScaledCount(3, -1.f, 5, 12), 3u);		// 음수 per_level → 기본 수 (예전엔 음수 float → uint32)
	EXPECT_EQ(DamageMath::ScaledCount(3, 0.5f, 10, 12), 8u);		// 3 + 5
	EXPECT_EQ(DamageMath::ScaledCount(3, 2.f, 10, 12), 12u);		// 3 + 20 → 상한
	EXPECT_EQ(DamageMath::ScaledCount(3, 0.f, 0, 12), 3u);
}
