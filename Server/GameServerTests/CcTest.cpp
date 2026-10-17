#include "pch.h"
#include "Creature.h"

// combat-system.md § Acceptance Criteria — CC 하드(19~26) · 소프트(28)
// AC 번호는 그 문서의 번호다. game-concept.md 의 AC-5 · AC-6 은 25 · 28 이 직접 검증한다

namespace
{
	// 0 은 CC 필드 초기값과 같아 "안 걸렸다" 와 "지금 막 끝났다" 가 구분되지 않는다
	constexpr uint64 T0_US = 10'000'000;
	constexpr uint64 US_PER_MS = 1000;

	uint64 AtMs(uint64 ms) { return T0_US + ms * US_PER_MS; }

	// 행동 불능 = 이동 · 회전 · 평타 · 스킬 전부 불가.
	// 속박은 이동만 막으므로 여기에 들지 않는다 (skill-system.md § S1)
	bool IsIncapacitated(const Creature& c, uint64 nowUs)
	{
		return c.CanMove(nowUs) == false
			&& c.CanTurn(nowUs) == false
			&& c.CanAttack(nowUs) == false
			&& c.CanUseSkill(nowUs) == false;
	}

	struct CcHit
	{
		uint64				atMs;
		Protocol::CcType	type;
		uint32				baseMs;
	};

	// 1ms 씩 시각을 진행하며 예약된 시각에 CC 를 걸고, 행동 불능이었던 ms 를 센다.
	// 끝나는 시각을 비교하지 않고 매 순간 판정 함수를 부르므로 "합집합" 을 그대로 잰다
	uint32 SimulateIncapacitatedMs(Creature& target, const vector<CcHit>& hits, uint32 windowMs)
	{
		uint32 incapacitatedMs = 0;

		for (uint32 ms = 0; ms < windowMs; ms++)
		{
			const uint64 nowUs = AtMs(ms);

			for (const CcHit& hit : hits)
			{
				if (hit.atMs == ms)
					target.ApplyHardCc(hit.type, nowUs, target.ComputeCcDurationMs(hit.baseMs));
			}

			if (IsIncapacitated(target, nowUs))
				++incapacitatedMs;
		}

		return incapacitatedMs;
	}
}

// AC 19
TEST(CreatureCc, 기절은_이동_회전_평타_스킬을_전부_막는다)
{
	Creature target;
	target.ApplyHardCc(Protocol::CC_TYPE_STUN, AtMs(0), 1000);

	const uint64 during = AtMs(500);
	EXPECT_FALSE(target.CanMove(during));
	EXPECT_FALSE(target.CanTurn(during));
	EXPECT_FALSE(target.CanAttack(during));
	EXPECT_FALSE(target.CanUseSkill(during));

	const uint64 after = AtMs(1000);
	EXPECT_TRUE(target.CanMove(after));
	EXPECT_TRUE(target.CanUseSkill(after));
}

// AC 20 — 속박은 반격이 가능하다. 그래서 AC-5 의 행동 불능 예산에서 빠진다
TEST(CreatureCc, 속박은_이동만_막는다)
{
	Creature target;
	target.ApplyHardCc(Protocol::CC_TYPE_ROOT, AtMs(0), 1000);

	const uint64 during = AtMs(500);
	EXPECT_FALSE(target.CanMove(during));
	EXPECT_TRUE(target.CanTurn(during));
	EXPECT_TRUE(target.CanAttack(during));
	EXPECT_TRUE(target.CanUseSkill(during));
}

// AC 23 · 24 — 저항 만렙이어도 60% 는 반드시 걸린다
TEST(CreatureCc, 저항은_지속시간을_최대_40퍼센트까지만_줄인다)
{
	Creature target;

	target.resistance = 0.f;
	EXPECT_EQ(target.ComputeCcDurationMs(1000), 1000u);

	target.resistance = 1.f;
	EXPECT_EQ(target.ComputeCcDurationMs(1000), 600u);
}

// AC 25 = game-concept.md AC-5 — 4인이 1.25초 기절을 이어 걸면 정확히 5초
TEST(CreatureCc, AC5_4인_릴레이_기절은_정확히_5초다)
{
	Creature target;

	const vector<CcHit> relay = {
		{    0, Protocol::CC_TYPE_STUN, 1250 },
		{ 1250, Protocol::CC_TYPE_STUN, 1250 },
		{ 2500, Protocol::CC_TYPE_STUN, 1250 },
		{ 3750, Protocol::CC_TYPE_STUN, 1250 },
	};

	EXPECT_EQ(SimulateIncapacitatedMs(target, relay, 8000), 5000u);
}

// AC 26 — 겹친 CC 는 더하지 않는다
TEST(CreatureCc, 동시에_건_기절_두개는_더하지_않는다)
{
	Creature target;

	const vector<CcHit> simultaneous = {
		{ 0, Protocol::CC_TYPE_STUN, 1250 },
		{ 0, Protocol::CC_TYPE_STUN, 1250 },
	};

	EXPECT_EQ(SimulateIncapacitatedMs(target, simultaneous, 3000), 1250u);
}

// skill-system.md § S1 의 예시 그대로 — 유형이 달라도 합집합이다
//   t=0   넉백 0.375   t=0.3  벽 충돌 기절 0.75   →  1.05 초 (1.125 가 아니다)
TEST(CreatureCc, 넉백_중_벽_충돌_기절은_끝나는_시각만_늘린다)
{
	Creature target;

	const vector<CcHit> wallSlam = {
		{   0, Protocol::CC_TYPE_KNOCKBACK, 375 },
		{ 300, Protocol::CC_TYPE_STUN,      750 },
	};

	EXPECT_EQ(SimulateIncapacitatedMs(target, wallSlam, 3000), 1050u);
}

// AC 28 = game-concept.md AC-6 — 감속은 최대값 하나만. 합산하면 1.05 가 된다
TEST(CreatureCc, AC6_감속_4개가_겹치면_가장_큰_하나만_적용된다)
{
	Creature target;

	const uint64 nowUs = AtMs(0);
	target.ApplySoftCc(Protocol::CC_TYPE_SLOW, nowUs, 1500, 0.20f);
	target.ApplySoftCc(Protocol::CC_TYPE_SLOW, nowUs, 1500, 0.30f);
	target.ApplySoftCc(Protocol::CC_TYPE_SLOW, nowUs, 1500, 0.40f);
	target.ApplySoftCc(Protocol::CC_TYPE_SLOW, nowUs, 1500, 0.15f);

	EXPECT_FLOAT_EQ(target.activeSlow, 0.40f);
}

TEST(CreatureCc, 감속은_상한_40퍼센트를_넘지_않는다)
{
	Creature target;
	target.ApplySoftCc(Protocol::CC_TYPE_SLOW, AtMs(0), 1500, 0.50f);

	EXPECT_FLOAT_EQ(target.activeSlow, BaseStats::SOFT_CC_SLOW_CAP);
}

// 최대값만 들고 있으면 가장 센 감속이 끝나는 순간 0 으로 떨어진다.
// 남은 감속원 중 다음 최대값으로 내려와야 한다
TEST(CreatureCc, 가장_센_감속이_끝나면_다음_최대값으로_내려온다)
{
	Creature target;
	target.ApplySoftCc(Protocol::CC_TYPE_SLOW, AtMs(0), 500, 0.40f);
	target.ApplySoftCc(Protocol::CC_TYPE_SLOW, AtMs(0), 1500, 0.20f);

	target.RefreshActiveSlow(AtMs(1000));
	EXPECT_FLOAT_EQ(target.activeSlow, 0.20f);
	EXPECT_EQ(target.slowSources.size(), 1u);

	target.RefreshActiveSlow(AtMs(2000));
	EXPECT_FLOAT_EQ(target.activeSlow, 0.f);
	EXPECT_TRUE(target.slowSources.empty());
}

// ── 도발 (2026-10-10 사용자 요청) - 걸리면 시전자에게 끌려가고(Room::StepTaunts) 어떤 공격 · 스킬도 못 한다 ──

TEST(CreatureCc, 도발은_이동_회전_평타_스킬을_전부_막는다)
{
	Creature target;
	target.ApplyHardCc(Protocol::CC_TYPE_TAUNT, AtMs(0), 1000);

	EXPECT_FALSE(target.CanMove(AtMs(500)));
	EXPECT_FALSE(target.CanTurn(AtMs(500)));
	EXPECT_FALSE(target.CanAttack(AtMs(500)));
	EXPECT_FALSE(target.CanUseSkill(AtMs(500)));
	EXPECT_TRUE(target.IsTaunted(AtMs(999)));

	EXPECT_FALSE(target.IsTaunted(AtMs(1000)));
	EXPECT_TRUE(target.CanMove(AtMs(1000)));
	EXPECT_TRUE(target.CanAttack(AtMs(1000)));
	EXPECT_TRUE(target.CanUseSkill(AtMs(1000)));
}

TEST(CreatureCc, 도발은_겹치면_더_긴_쪽이_남고_저항이_깎는다)
{
	Creature target;
	target.resistance = 0.5f;		// × (1 - 0.5 × 0.4) = 0.8

	EXPECT_EQ(target.ComputeCcDurationMs(1000), 800u);

	target.ApplyHardCc(Protocol::CC_TYPE_TAUNT, AtMs(0), 800);
	target.ApplyHardCc(Protocol::CC_TYPE_TAUNT, AtMs(100), 400);		// 끝 500 < 800 - 갱신 안 됨
	EXPECT_TRUE(target.IsTaunted(AtMs(799)));
	EXPECT_FALSE(target.IsTaunted(AtMs(800)));
}

TEST(CreatureCc, 도발은_전투_상태_초기화로_풀린다)
{
	Creature target;
	target.ApplyHardCc(Protocol::CC_TYPE_TAUNT, AtMs(0), 1000);
	target.tauntSourceId = 42;

	target.ClearCombatState();

	EXPECT_FALSE(target.IsTaunted(AtMs(1)));
	EXPECT_EQ(target.tauntSourceId, 0u);
}

// escape-timer.md OQ-7 (2026-10-18) - 탈출 구역의 "밀려 나갔다" 판정은 넉백이 끝난 뒤 1 초를 본다.
// knockbackEndUs 는 CC 가 풀리면 Room::UpdateCc 가 0 으로 지운다 - 예전엔 그 값을 봐서 넉백 뒤 이탈이 거의 다 "자발" 로 셌다
TEST(Cc, 넉백이_끝나도_마지막으로_밀려난_시각은_남는다_OQ_7)
{
	Creature target;
	target.ApplyHardCc(Protocol::CC_TYPE_KNOCKBACK, AtMs(0), 300);
	EXPECT_EQ(target.lastDisplacedEndUs, AtMs(300));

	target.knockbackEndUs = 0;				// UpdateCc 가 CC 를 끝낸 뒤
	EXPECT_EQ(target.lastDisplacedEndUs, AtMs(300));

	target.ApplyHardCc(Protocol::CC_TYPE_LAUNCH, AtMs(1000), 500);
	EXPECT_EQ(target.lastDisplacedEndUs, AtMs(1500));

	target.ApplyHardCc(Protocol::CC_TYPE_STUN, AtMs(2000), 2000);	// 기절은 밀려난 것이 아니다
	EXPECT_EQ(target.lastDisplacedEndUs, AtMs(1500));
}
