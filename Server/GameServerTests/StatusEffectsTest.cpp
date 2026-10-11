#include "pch.h"
#include "StatusEffects.h"

// 상태 효과 자료구조 (design/quick-specs/skill-roster-2026-10-10.md 1 단계 · 사용자 결정 2026-10-10):
// 지속 피해는 같은 종류도 중첩(상한) · 표식은 본인 것만, 중첩 없이 갱신 · 버프는 같은 것이면 갱신

namespace
{
	constexpr uint64 T0_US = 10'000'000;
	constexpr uint64 US_PER_MS = 1000;
	constexpr uint32 TICK_MS = 500;
	constexpr uint32 MAX_STACKS = 5;

	uint64 AtMs(uint64 ms) { return T0_US + ms * US_PER_MS; }

	DotInstance Bleed(uint64 instigator, uint64 nowUs, uint32 durationMs, float tick = 2.f)
	{
		DotInstance d;
		d.skillId = 1015;
		d.kind = "bleed";
		d.instigatorId = instigator;
		d.baseDamage = tick;
		d.endUs = nowUs + durationMs * US_PER_MS;
		d.nextTickUs = nowUs + TICK_MS * US_PER_MS;
		return d;
	}

	BuffInstance Buff(uint32 skillId, SkillBuffStat stat, float magnitude, uint64 endUs)
	{
		BuffInstance b;
		b.skillId = skillId;
		b.stat = stat;
		b.magnitude = magnitude;
		b.endUs = endUs;
		return b;
	}

	MarkInstance Mark(const char* kind, uint64 instigator, uint64 endUs)
	{
		MarkInstance m;
		m.kind = kind;
		m.instigatorId = instigator;
		m.endUs = endUs;
		return m;
	}

	struct Expired
	{
		Protocol::StatusKind kind;
		string param;
		uint64 instigatorId;
		uint32 stacks;
	};

	vector<Expired> ExpireAll(StatusSet& s, uint64 nowUs, OUT bool& buffGone)
	{
		vector<Expired> out;
		buffGone = s.Expire(nowUs, [&](Protocol::StatusKind kind, const string& param, uint64 instigatorId, uint32 stacks)
			{
				out.push_back({ kind, param, instigatorId, stacks });
			});
		return out;
	}
}

// ── 지속 피해 ──

TEST(StatusDot, 같은_종류가_상한까지_중첩된다)
{
	StatusSet s;

	for (uint32 i = 1; i <= MAX_STACKS; i++)
		EXPECT_EQ(s.AddDot(Bleed(1, AtMs(i * 10), 4000), MAX_STACKS), i);

	EXPECT_EQ(s.DotStacks("bleed"), MAX_STACKS);
	EXPECT_EQ(s.Dots().size(), MAX_STACKS);
}

TEST(StatusDot, 상한에서는_가장_먼저_끝나는_것을_새_것이_대체한다)
{
	StatusSet s;

	// 1 번이 가장 먼저 끝난다 (1000 ms), 나머지는 4000
	s.AddDot(Bleed(1, AtMs(0), 1000, 1.f), MAX_STACKS);
	for (uint32 i = 2; i <= MAX_STACKS; i++)
		s.AddDot(Bleed(1, AtMs(0), 4000, static_cast<float>(i)), MAX_STACKS);

	EXPECT_EQ(s.AddDot(Bleed(1, AtMs(0), 4000, 99.f), MAX_STACKS), MAX_STACKS);
	EXPECT_EQ(s.Dots().size(), MAX_STACKS);

	bool sawReplacement = false;
	for (const DotInstance& d : s.Dots())
	{
		EXPECT_NE(d.baseDamage, 1.f) << "1000 ms 짜리가 남아 있다";
		if (d.baseDamage == 99.f)
			sawReplacement = true;
	}
	EXPECT_TRUE(sawReplacement);
}

TEST(StatusDot, 다른_종류는_따로_센다)
{
	StatusSet s;
	s.AddDot(Bleed(1, AtMs(0), 4000), MAX_STACKS);

	DotInstance poison = Bleed(1, AtMs(0), 4000);
	poison.kind = "poison";
	s.AddDot(poison, MAX_STACKS);

	EXPECT_EQ(s.DotStacks("bleed"), 1u);
	EXPECT_EQ(s.DotStacks("poison"), 1u);
	EXPECT_EQ(s.DotStacks("burn"), 0u);
}

TEST(StatusDot, 틱은_간격마다_한_번_그리고_밀려도_몰아_터지지_않는다)
{
	StatusSet s;
	s.AddDot(Bleed(1, AtMs(0), 4000), MAX_STACKS);

	int32 ticks = 0;
	auto count = [&](const DotInstance&) { ticks++; };

	s.TickDots(AtMs(499), TICK_MS, count);
	EXPECT_EQ(ticks, 0);

	s.TickDots(AtMs(500), TICK_MS, count);
	EXPECT_EQ(ticks, 1);

	s.TickDots(AtMs(510), TICK_MS, count);		// 다음 틱은 1000
	EXPECT_EQ(ticks, 1);

	// 1500 까지 한 번에 건너뛰어도 (Flush 가 밀림) 이 호출엔 한 틱만 - 다음 호출에 또 하나
	s.TickDots(AtMs(1500), TICK_MS, count);
	EXPECT_EQ(ticks, 2);
	s.TickDots(AtMs(1500), TICK_MS, count);
	EXPECT_EQ(ticks, 3);
	s.TickDots(AtMs(1500), TICK_MS, count);
	EXPECT_EQ(ticks, 3);
}

TEST(StatusDot, 지속이_끝나면_틱하지_않고_만료로_빠진다)
{
	StatusSet s;
	s.AddDot(Bleed(7, AtMs(0), 1000), MAX_STACKS);
	s.AddDot(Bleed(7, AtMs(0), 4000), MAX_STACKS);

	int32 ticks = 0;
	s.TickDots(AtMs(1000), TICK_MS, [&](const DotInstance&) { ticks++; });
	EXPECT_EQ(ticks, 1) << "1000 ms 에 끝나는 쪽은 틱하지 않는다";

	bool buffGone = false;
	const vector<Expired> gone = ExpireAll(s, AtMs(1000), OUT buffGone);

	ASSERT_EQ(gone.size(), 1u);
	EXPECT_EQ(gone[0].kind, Protocol::STATUS_DOT);
	EXPECT_EQ(gone[0].param, "bleed");
	EXPECT_EQ(gone[0].instigatorId, 7u);
	EXPECT_EQ(gone[0].stacks, 1u) << "남은 중첩 수를 알린다";
	EXPECT_FALSE(buffGone);
	EXPECT_EQ(s.DotStacks("bleed"), 1u);
}

// ── 버프 ──

TEST(StatusBuff, 같은_스킬_같은_스탯은_갱신되고_합산되지_않는다)
{
	StatusSet s;

	EXPECT_TRUE(s.AddBuff(Buff(1010, SkillBuffStat::AttackPower, 0.15f, AtMs(5000))));
	EXPECT_FALSE(s.AddBuff(Buff(1010, SkillBuffStat::AttackPower, 0.18f, AtMs(8000))));

	ASSERT_EQ(s.Buffs().size(), 1u);
	EXPECT_FLOAT_EQ(s.BuffSum(SkillBuffStat::AttackPower), 0.18f);
	EXPECT_EQ(s.Buffs()[0].endUs, AtMs(8000));
}

TEST(StatusBuff, 다른_스킬의_같은_스탯은_더해진다)
{
	StatusSet s;
	s.AddBuff(Buff(1010, SkillBuffStat::AttackPower, 0.15f, AtMs(5000)));
	s.AddBuff(Buff(1039, SkillBuffStat::AttackPower, 0.15f, AtMs(5000)));
	s.AddBuff(Buff(1010, SkillBuffStat::CritChance, 0.03f, AtMs(5000)));

	EXPECT_FLOAT_EQ(s.BuffSum(SkillBuffStat::AttackPower), 0.30f);
	EXPECT_FLOAT_EQ(s.BuffSum(SkillBuffStat::CritChance), 0.03f);
	EXPECT_FLOAT_EQ(s.BuffSum(SkillBuffStat::MoveSpeed), 0.f);
}

TEST(StatusBuff, 만료되면_스탯_재계산이_필요하다고_알린다)
{
	StatusSet s;
	s.AddBuff(Buff(1010, SkillBuffStat::AttackPower, 0.15f, AtMs(5000)));
	s.AddBuff(Buff(1047, SkillBuffStat::AttackSpeed, 0.30f, AtMs(9000)));

	bool buffGone = false;
	vector<Expired> gone = ExpireAll(s, AtMs(4999), OUT buffGone);
	EXPECT_TRUE(gone.empty());
	EXPECT_FALSE(buffGone);

	gone = ExpireAll(s, AtMs(5000), OUT buffGone);
	ASSERT_EQ(gone.size(), 1u);
	EXPECT_EQ(gone[0].kind, Protocol::STATUS_BUFF);
	EXPECT_EQ(gone[0].param, "attack_power");
	EXPECT_TRUE(buffGone);
	EXPECT_FLOAT_EQ(s.BuffSum(SkillBuffStat::AttackPower), 0.f);
	EXPECT_FLOAT_EQ(s.BuffSum(SkillBuffStat::AttackSpeed), 0.30f);
}

// ── 표식 ──

TEST(StatusMark, 본인_표식만_보인다)
{
	StatusSet s;
	EXPECT_TRUE(s.AddMark(Mark("spear", 1, AtMs(6000))));

	EXPECT_TRUE(s.HasMark("spear", 1));
	EXPECT_FALSE(s.HasMark("spear", 2)) << "남이 남긴 표식은 쓸 수 없다";
	EXPECT_FALSE(s.HasMark("arrow", 1));
}

TEST(StatusMark, 같은_주인의_같은_표식은_갱신되고_다른_주인은_따로_남는다)
{
	StatusSet s;
	s.AddMark(Mark("spear", 1, AtMs(6000)));
	EXPECT_FALSE(s.AddMark(Mark("spear", 1, AtMs(9000))));
	EXPECT_TRUE(s.AddMark(Mark("spear", 2, AtMs(6000))));

	ASSERT_EQ(s.Marks().size(), 2u);
	EXPECT_EQ(s.Marks()[0].endUs, AtMs(9000));
}

TEST(StatusMark, 소비하면_그_주인의_것만_사라진다)
{
	StatusSet s;
	s.AddMark(Mark("spear", 1, AtMs(6000)));
	s.AddMark(Mark("spear", 2, AtMs(6000)));

	EXPECT_TRUE(s.ConsumeMark("spear", 1));
	EXPECT_FALSE(s.ConsumeMark("spear", 1));
	EXPECT_FALSE(s.HasMark("spear", 1));
	EXPECT_TRUE(s.HasMark("spear", 2));
}

TEST(StatusMark, 만료는_종류와_주인을_알린다)
{
	StatusSet s;
	s.AddMark(Mark("rope", 3, AtMs(6000)));

	bool buffGone = false;
	const vector<Expired> gone = ExpireAll(s, AtMs(6000), OUT buffGone);

	ASSERT_EQ(gone.size(), 1u);
	EXPECT_EQ(gone[0].kind, Protocol::STATUS_MARK);
	EXPECT_EQ(gone[0].param, "rope");
	EXPECT_EQ(gone[0].instigatorId, 3u);
	EXPECT_TRUE(s.Empty());
}

TEST(StatusSet, Clear_는_전부_지운다)
{
	StatusSet s;
	s.AddDot(Bleed(1, AtMs(0), 4000), MAX_STACKS);
	s.AddBuff(Buff(1010, SkillBuffStat::AttackPower, 0.15f, AtMs(5000)));
	s.AddMark(Mark("spear", 1, AtMs(6000)));
	EXPECT_FALSE(s.Empty());

	s.Clear();
	EXPECT_TRUE(s.Empty());
}

// ── 모자 로스터 (2026-10-10) - 버프 param · 제거 · 지속 피해 연장/해제 ──

TEST(StatusBuff, param_이_다르면_다른_버프다_같으면_갱신이다)
{
	StatusSet s;
	BuffInstance fire = Buff(1100, SkillBuffStat::DamageMod, 0.2f, AtMs(5000));
	fire.param = "elem:fire";
	BuffInstance bow = Buff(1099, SkillBuffStat::DamageMod, 0.2f, AtMs(5000));
	bow.param = "tag:bow";

	EXPECT_TRUE(s.AddBuff(fire));
	EXPECT_TRUE(s.AddBuff(bow));
	EXPECT_FALSE(s.AddBuff(fire));

	EXPECT_FLOAT_EQ(s.BuffSum(SkillBuffStat::DamageMod), 0.4f);
	EXPECT_FLOAT_EQ(s.BuffSum(SkillBuffStat::DamageMod, "elem:fire"), 0.2f);
	EXPECT_TRUE(s.HasBuff(SkillBuffStat::DamageMod, "tag:bow"));
	EXPECT_FALSE(s.HasBuff(SkillBuffStat::DamageMod, "tag:sword"));
}

TEST(StatusBuff, 한_스킬의_버프를_전부_뺀다_배수진)
{
	StatusSet s;
	s.AddBuff(Buff(1102, SkillBuffStat::AttackPower, 0.3f, AtMs(10000)));
	s.AddBuff(Buff(1102, SkillBuffStat::AttackSpeed, 0.3f, AtMs(10000)));
	s.AddBuff(Buff(1010, SkillBuffStat::AttackPower, 0.15f, AtMs(5000)));

	EXPECT_EQ(s.RemoveBuffs(1102), 2u);
	EXPECT_FLOAT_EQ(s.BuffSum(SkillBuffStat::AttackPower), 0.15f);
	EXPECT_FLOAT_EQ(s.BuffSum(SkillBuffStat::AttackSpeed), 0.f);
}

TEST(StatusBuff, 음수_버프는_그대로_더해진다_광전사의_눈물)
{
	StatusSet s;
	s.AddBuff(Buff(1080, SkillBuffStat::Armor, -0.3f, AtMs(5000)));
	s.AddBuff(Buff(1079, SkillBuffStat::Armor, 0.3f, AtMs(5000)));
	EXPECT_FLOAT_EQ(s.BuffSum(SkillBuffStat::Armor), 0.f);
}

TEST(StatusBuff, 유령_신부의_축복은_param_없는_자기_버프로도_벽을_무시한다)
{
	// Room 의 SelfBuff 경로는 param 을 채우지 않는다. 예전 조회 HasBuff(WallPierce, "wall_pierce") 는 그래서 늘 false 였다
	StatusSet s;
	EXPECT_FALSE(s.IgnoresWalls());

	s.AddBuff(Buff(1078, SkillBuffStat::WallPierce, 1.f, AtMs(5000)));
	EXPECT_TRUE(s.IgnoresWalls());
	EXPECT_FALSE(s.HasBuff(SkillBuffStat::WallPierce, "wall_pierce")) << "예전 조회 - 이것으로 찾으면 안 된다";

	bool buffGone = false;
	ExpireAll(s, AtMs(5001), buffGone);
	EXPECT_TRUE(buffGone);
	EXPECT_FALSE(s.IgnoresWalls());
}

TEST(StatusDot, 종류별_해제와_마법_연장_째깍째깍)
{
	StatusSet s;
	DotInstance poison = Bleed(1, AtMs(0), 4000);
	poison.kind = "poison";
	poison.type = Protocol::DAMAGE_TYPE_MAGIC;
	DotInstance bleed = Bleed(1, AtMs(0), 4000);
	bleed.type = Protocol::DAMAGE_TYPE_PHYSICAL;
	s.AddDot(poison, MAX_STACKS);
	s.AddDot(poison, MAX_STACKS);
	s.AddDot(bleed, MAX_STACKS);

	EXPECT_EQ(s.ExtendDots(Protocol::DAMAGE_TYPE_MAGIC, 2000), 2u);
	for (const DotInstance& d : s.Dots())
		EXPECT_EQ(d.endUs, d.kind == "poison" ? AtMs(6000) : AtMs(4000));

	EXPECT_EQ(s.RemoveDots("poison"), 2u);
	EXPECT_EQ(s.DotStacks("poison"), 0u);
	EXPECT_EQ(s.DotStacks("bleed"), 1u);

	EXPECT_EQ(s.RemoveDots(""), 1u);
	EXPECT_TRUE(s.Dots().empty());
}
