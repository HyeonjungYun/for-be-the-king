#include "pch.h"
#include "LonePath.h"

// 고독한 길 (2026-10-17) - design/quick-specs/lone-path-2026-10-17.md AC-LP-01 ~ 05

namespace
{
	LonePathRules Defaults() { return LonePathRules(); }
}

// AC-LP-01 다구대일이 아니면(주변 적 1 명 이하) 받는 피해 · CC 그대로
TEST(LonePath, 혼자_상대면_효과_없음)
{
	const LonePathRules r = Defaults();
	EXPECT_FLOAT_EQ(LonePath::DamageTakenMult(4, 0, r), 1.f);
	EXPECT_FLOAT_EQ(LonePath::DamageTakenMult(4, 1, r), 1.f);
	EXPECT_FLOAT_EQ(LonePath::CcDurationMult(4, 1, r), 1.f);
}

// AC-LP-02 적 2 명부터 착용 수만큼 - 1 개 -8 % / -10 %, 4 개 -32 % / -40 %
TEST(LonePath, 착용_수만큼_다구대일_방어)
{
	const LonePathRules r = Defaults();
	EXPECT_NEAR(LonePath::DamageTakenMult(1, 2, r), 0.92f, 1e-5f);
	EXPECT_NEAR(LonePath::CcDurationMult(1, 2, r), 0.90f, 1e-5f);
	EXPECT_NEAR(LonePath::DamageTakenMult(4, 5, r), 0.68f, 1e-5f);
	EXPECT_NEAR(LonePath::CcDurationMult(4, 5, r), 0.60f, 1e-5f);
}

// AC-LP-03 상한 4 개 - 그 이상 껴도 더 세지지 않는다 (CC 저항 상한 40 %)
TEST(LonePath, 상한을_넘지_않는다)
{
	const LonePathRules r = Defaults();
	EXPECT_FLOAT_EQ(LonePath::CcDurationMult(6, 3, r), LonePath::CcDurationMult(4, 3, r));
	EXPECT_EQ(LonePath::EffectivePieces(-1, r), 0);
	EXPECT_NEAR(LonePath::AttackBonus(6, r), 0.12f, 1e-5f);
	EXPECT_FLOAT_EQ(LonePath::AttackBonus(0, r), 0.f);
}

// AC-LP-04 옵션 확률 - 고유 번호 해시로 굴려 약 5 %, 같은 번호는 늘 같은 결과
TEST(LonePath, 옵션은_약_5퍼센트로_붙는다)
{
	const LonePathRules r = Defaults();
	int32 hits = 0;
	for (uint64 id = 1; id <= 20000; id++)
		hits += LonePath::Rolls(LonePath::RollOf(id), r) ? 1 : 0;
	EXPECT_GT(hits, 800);		// 5 % = 1000
	EXPECT_LT(hits, 1200);
	EXPECT_EQ(LonePath::RollOf(123456789), LonePath::RollOf(123456789));
	EXPECT_LT(LonePath::RollOf(UINT64_MAX), 100u);
}

// AC-LP-05 설정 파일 - 모르는 키 · 범위 밖 · 완전 면역이 되는 조합은 거절
TEST(LonePath, 설정_검사)
{
	string error;
	LonePathRules r;
	EXPECT_TRUE(r.Parse("key,value\nroll_pct,10\nmax_pieces,3\n", error)) << error;
	EXPECT_EQ(r.rollPct, 10u);
	EXPECT_EQ(r.maxPieces, 3);

	LonePathRules bad1;
	EXPECT_FALSE(bad1.Parse("key,value\nnope,1\n", error));
	LonePathRules bad2;
	EXPECT_FALSE(bad2.Parse("key,value\nroll_pct,101\n", error));
	LonePathRules bad3;
	EXPECT_FALSE(bad3.Parse("key,value\ncc_reduce_per_piece,0.2\nmax_pieces,4\n", error));		// 80 % - 상한 40 % 초과
}
