#include "pch.h"
#include "CheatGuard.h"

// 안티치트 위반 기록 (CheatGuard.h · design/quick-specs/anticheat-2026-10-19.md)

namespace
{
	constexpr uint64 SEC = 1'000'000;
	constexpr uint64 T0 = 100 * SEC;

	CheatRules Rules()
	{
		CheatRules r;			// 기본값 - anticheat.csv 와 같다
		return r;
	}
}

TEST(CheatGuard, 속도핵은_1초_안에_끊긴다)
{
	const CheatRules rules = Rules();
	CheatGuard guard;

	// 조작 클라: 이동 패킷마다(30 Hz) 예산 초과 - 3 점씩
	CheatAction last = CheatAction::None;
	int kicks = 0, warns = 0;
	uint64 kickedAt = 0;
	for (int i = 0; i < 60; i++)
	{
		const uint64 now = T0 + i * 33'000;
		last = guard.Report(Violation::MoveSpeed, now, rules);
		if (last == CheatAction::Warn) warns++;
		if (last == CheatAction::Kick) { kicks++; if (kickedAt == 0) kickedAt = now; }
	}
	EXPECT_EQ(warns, 1);
	EXPECT_EQ(kicks, 1);			// 한 번만 - 그 뒤로는 None
	EXPECT_TRUE(guard.Kicked());
	EXPECT_LT(kickedAt - T0, 1 * SEC);
	EXPECT_EQ(guard.Count(Violation::MoveSpeed), 60u);
}

TEST(CheatGuard, 렉으로_가끔_되돌림을_맞는_정상_플레이어는_쌓이지_않는다)
{
	const CheatRules rules = Rules();
	CheatGuard guard;

	// 10 분 동안 2 초마다 한 번 예산 초과 (지연 순간마다 하나) - 3 점 올리고 2 초에 4 점 내려간다
	for (int i = 0; i < 300; i++)
		EXPECT_EQ(guard.Report(Violation::MoveSpeed, T0 + i * 2 * SEC, rules), CheatAction::None);
	EXPECT_LT(guard.Score(T0 + 600 * SEC, rules), rules.warnScore);
	EXPECT_FALSE(guard.Kicked());
}

TEST(CheatGuard, 점수는_초당_감쇠하고_경고는_다시_넘을_때_다시_울린다)
{
	CheatRules rules = Rules();
	rules.kickScore = 1000.f;
	CheatGuard guard;

	for (int i = 0; i < 7; i++)
		guard.Report(Violation::MoveSpeed, T0, rules);		// 21 점 - 경고
	EXPECT_NEAR(guard.Score(T0, rules), 21.f, 1e-4);
	EXPECT_NEAR(guard.Score(T0 + 5 * SEC, rules), 11.f, 1e-4);	// 2 점/초 x 5 초
	EXPECT_NEAR(guard.Score(T0 + 60 * SEC, rules), 0.f, 1e-4);	// 0 아래로는 안 간다

	// 경고 기준 아래로 내려왔다 - 다시 넘으면 다시 경고
	CheatAction a = CheatAction::None;
	for (int i = 0; i < 7; i++)
		a = (a == CheatAction::Warn) ? a : guard.Report(Violation::MoveSpeed, T0 + 60 * SEC, rules);
	EXPECT_EQ(a, CheatAction::Warn);
}

TEST(CheatGuard, 관찰_모드면_세기만_하고_끊지_않는다)
{
	CheatRules rules = Rules();
	rules.enabled = false;
	CheatGuard guard;
	for (int i = 0; i < 100; i++)
		EXPECT_NE(guard.Report(Violation::MoveBounds, T0, rules), CheatAction::Kick);
	EXPECT_FALSE(guard.Kicked());
	EXPECT_EQ(guard.Total(), 100u);
}

// 회귀 (2026-10-19 실측): 끊긴 몸에 다시 붙은 접속이 '이미 끊었음' 표시를 물려받아 다시는 끊기지 않았다 -
// Room::HandleReattach 가 Reset 을 부른다. Reset 뒤에는 새 접속처럼 다시 끊긴다
TEST(CheatGuard, Reset_뒤에는_다시_끊긴다)
{
	// Arrange - 한 번 끊긴 상태
	const CheatRules rules = Rules();
	CheatGuard guard;
	for (int i = 0; i < 30; i++)
		guard.Report(Violation::MoveSpeed, T0, rules);
	ASSERT_TRUE(guard.Kicked());
	EXPECT_EQ(guard.Report(Violation::MoveSpeed, T0, rules), CheatAction::None);		// 같은 접속에서는 다시 끊지 않는다

	// Act - 재접속
	guard.Reset();

	// Assert - 새 접속의 위반은 다시 끊긴다
	CheatAction last = CheatAction::None;
	for (int i = 0; i < 30 && last != CheatAction::Kick; i++)
		last = guard.Report(Violation::MoveSpeed, T0 + SEC, rules);
	EXPECT_EQ(last, CheatAction::Kick);
}

TEST(CheatGuard, 요약은_많은_순이다)
{
	CheatGuard guard;
	guard.Report(Violation::AttackRange, T0);
	for (int i = 0; i < 3; i++) guard.Report(Violation::MoveWall, T0);
	for (int i = 0; i < 2; i++) guard.Report(Violation::SkillCooldown, T0);
	EXPECT_EQ(guard.Summary(), "move_wall:3 skill_cooldown:2 attack_range:1");
	EXPECT_EQ(CheatGuard().Summary(), "-");
}

TEST(CheatRules, csv_로_가중치와_기준을_읽는다)
{
	CheatRules rules;
	string error;
	ASSERT_TRUE(rules.Parse("key,value\nkick_score,40\nweight_move_speed,7\nenabled,0\nban_minutes,0\n", OUT error)) << error;
	EXPECT_FLOAT_EQ(rules.kickScore, 40.f);
	EXPECT_FLOAT_EQ(rules.Weight(Violation::MoveSpeed), 7.f);
	EXPECT_FLOAT_EQ(rules.Weight(Violation::MoveWall), 4.f);		// 없는 키는 기본값
	EXPECT_FALSE(rules.enabled);
	EXPECT_EQ(rules.banMinutes, 0u);
}

TEST(CheatRules, 모르는_키와_범위_밖과_역전된_기준은_거부한다)
{
	string error;
	EXPECT_FALSE(CheatRules().Parse("key,value\nweight_fly_hack,3\n", OUT error));
	EXPECT_FALSE(CheatRules().Parse("key,value\nsomething,3\n", OUT error));
	EXPECT_FALSE(CheatRules().Parse("key,value\nweight_move_speed,-1\n", OUT error));
	EXPECT_FALSE(CheatRules().Parse("key,value\nwarn_score,80\nkick_score,60\n", OUT error));
}

TEST(CheatBan, 창_안의_strike_가_기준에_닿으면_정지한다)
{
	const CheatRules rules = Rules();		// 24 시간에 3 번 → 60 분
	const uint64 now = 2'000'000'000;

	EXPECT_EQ(CheatBan::BanUntil({ now - 100, now - 50 }, now, rules), 0u);
	EXPECT_EQ(CheatBan::BanUntil({ now - 100, now - 50, now }, now, rules), now + 3600);
	// 창(24 시간) 밖의 strike 는 세지 않는다
	EXPECT_EQ(CheatBan::BanUntil({ now - 25 * 3600, now - 50, now }, now, rules), 0u);

	CheatRules noBan = rules;
	noBan.banMinutes = 0;
	EXPECT_EQ(CheatBan::BanUntil({ now, now, now, now }, now, noBan), 0u);
}
