#include "pch.h"
#include "Player.h"

// 스킬 버프가 장비 합산 위에 얹히는 방식 (design/quick-specs/skill-roster-2026-10-10.md 1 단계).
// 비율 버프는 가산 뒤 1회 곱 - 곱연산 누적 금지 (entities.yaml 과 같은 원칙). 버프가 끝나면 장비 합산으로 돌아간다

namespace
{
	constexpr uint64 T0_US = 10'000'000;
	uint64 AtMs(uint64 ms) { return T0_US + ms * 1000; }

	using PlayerPtr = shared_ptr<Player>;

	// 장비 합산을 직접 넣는다 - ItemTable 없이 (EquipStats::Compute 는 EquipStatsTest 가 본다)
	PlayerPtr MakePlayer()
	{
		PlayerPtr p = make_shared<Player>();
		p->equipTotals.maxHp = 600;
		p->equipTotals.attackPower = 20;
		p->equipTotals.magicPower = 10;
		p->equipTotals.armor = 10;
		p->equipTotals.magicResist = 4;
		p->equipTotals.critChance = 0.10f;
		p->equipTotals.percentAsBonus = 0.10f;
		p->equipTotals.percentMsBonus = 0.05f;
		p->maxHp = 600;
		p->hp = 600;
		p->RebuildCombatStats();
		return p;
	}

	void AddBuff(Player& p, uint32 skillId, SkillBuffStat stat, float magnitude, uint64 endUs)
	{
		BuffInstance b;
		b.skillId = skillId;
		b.stat = stat;
		b.magnitude = magnitude;
		b.endUs = endUs;
		p.status.AddBuff(b);
		p.OnBuffsChanged();
	}
}

TEST(BuffStats, 버프가_없으면_장비_합산_그대로다)
{
	PlayerPtr p = MakePlayer();

	EXPECT_EQ(p->attackPower, 20);
	EXPECT_EQ(p->magicPower, 10);
	EXPECT_EQ(p->armor, 10);
	EXPECT_FLOAT_EQ(p->critChance, 0.10f);
	EXPECT_FLOAT_EQ(p->percentAsBonus, 0.10f);
	EXPECT_FLOAT_EQ(p->percentMsBonus, 0.05f);
	EXPECT_FLOAT_EQ(p->outgoingDamagePct, 0.f);
}

TEST(BuffStats, 공격력_버프는_비율로_곱해지고_치명_공속_이속은_더해진다)
{
	PlayerPtr p = MakePlayer();

	AddBuff(*p, 1010, SkillBuffStat::AttackPower, 0.15f, AtMs(5000));
	AddBuff(*p, 1010, SkillBuffStat::CritChance, 0.03f, AtMs(5000));
	AddBuff(*p, 1047, SkillBuffStat::AttackSpeed, 0.30f, AtMs(5000));
	AddBuff(*p, 1034, SkillBuffStat::MoveSpeed, 0.30f, AtMs(3000));

	EXPECT_EQ(p->attackPower, 23);						// 20 × 1.15 = 23
	EXPECT_FLOAT_EQ(p->critChance, 0.13f);
	EXPECT_FLOAT_EQ(p->percentAsBonus, 0.40f);
	EXPECT_FLOAT_EQ(p->percentMsBonus, 0.35f);
	EXPECT_EQ(p->magicPower, 10) << "안 건드린 스탯은 그대로";
}

TEST(BuffStats, 같은_스탯_두_버프는_더한_뒤_한_번_곱한다)
{
	PlayerPtr p = MakePlayer();

	AddBuff(*p, 1010, SkillBuffStat::AttackPower, 0.15f, AtMs(5000));
	AddBuff(*p, 1039, SkillBuffStat::AttackPower, 0.15f, AtMs(5000));

	EXPECT_EQ(p->attackPower, 26);						// 20 × 1.30 = 26 (곱 누적이면 26.45 → 26 이지만 원칙 확인)
}

TEST(BuffStats, 이속_버프는_서버_이동_상한에_들어간다)
{
	PlayerPtr p = MakePlayer();
	const float before = p->GetEffectiveMoveSpeed();

	AddBuff(*p, 1034, SkillBuffStat::MoveSpeed, 0.30f, AtMs(3000));

	EXPECT_GT(p->GetEffectiveMoveSpeed(), before);
	EXPECT_FLOAT_EQ(p->GetEffectiveMoveSpeed(), 340.f * (1.f + 0.35f));
}

TEST(BuffStats, 피해_증가_버프는_평타와_스킬_피해를_올린다)
{
	PlayerPtr p = MakePlayer();
	p->critChance = 0.f;
	p->equipTotals.critChance = 0.f;

	Creature dummy;
	dummy.armor = 0;
	dummy.magicResist = 0;

	bool crit = false;
	const int32 basicBefore = p->ComputeDamageTo(dummy, Protocol::DAMAGE_TYPE_PHYSICAL, OUT crit);
	const int32 skillBefore = p->ComputeSkillDamageTo(dummy, 2.f, 0.f, OUT crit);

	AddBuff(*p, 1073, SkillBuffStat::DamagePct, 0.25f, AtMs(5000));

	EXPECT_FLOAT_EQ(p->outgoingDamagePct, 0.25f);
	EXPECT_EQ(p->ComputeDamageTo(dummy, Protocol::DAMAGE_TYPE_PHYSICAL, OUT crit), 25);		// 20 × 1.25
	EXPECT_EQ(p->ComputeSkillDamageTo(dummy, 2.f, 0.f, OUT crit), 50);						// 20 × 2 × 1.25
	EXPECT_EQ(basicBefore, 20);
	EXPECT_EQ(skillBefore, 40);
}

TEST(BuffStats, 버프가_끝나면_장비_합산으로_돌아간다)
{
	PlayerPtr p = MakePlayer();
	AddBuff(*p, 1010, SkillBuffStat::AttackPower, 0.15f, AtMs(5000));
	EXPECT_EQ(p->attackPower, 23);

	const bool buffGone = p->status.Expire(AtMs(5000), [](Protocol::StatusKind, const string&, uint64, uint32) {});
	EXPECT_TRUE(buffGone);
	p->OnBuffsChanged();

	EXPECT_EQ(p->attackPower, 20);
}

TEST(BuffStats, 치명_확률은_1_을_넘지_않는다)
{
	PlayerPtr p = MakePlayer();
	AddBuff(*p, 1010, SkillBuffStat::CritChance, 2.f, AtMs(5000));

	EXPECT_FLOAT_EQ(p->critChance, 1.f);
}

TEST(BuffStats, 전투_상태_초기화는_버프와_상태_효과를_지운다)
{
	PlayerPtr p = MakePlayer();
	AddBuff(*p, 1010, SkillBuffStat::AttackPower, 0.15f, AtMs(5000));
	p->recastSkillId = 1038;
	p->blindEndUs = AtMs(2000);

	p->ClearCombatState();

	EXPECT_TRUE(p->status.Empty());
	EXPECT_EQ(p->attackPower, 20);
	EXPECT_EQ(p->recastSkillId, 0u);
	EXPECT_EQ(p->blindEndUs, 0u);
}

TEST(BuffStats, 보조_무기_슬롯의_아이템_레벨은_주무기의_것이다)
{
	PlayerPtr p = MakePlayer();
	p->equipped[Protocol::SLOT_WEAPON_PRIMARY - 1].instanceId = 11;
	p->equipped[Protocol::SLOT_WEAPON_PRIMARY - 1].level = 3;
	p->equipped[Protocol::SLOT_BOOTS - 1].instanceId = 12;
	p->equipped[Protocol::SLOT_BOOTS - 1].level = 1;

	EXPECT_EQ(p->ItemLevelOfSlot(Protocol::SLOT_WEAPON_PRIMARY), 3u);
	EXPECT_EQ(p->ItemLevelOfSlot(Protocol::SLOT_WEAPON_SECONDARY), 3u);
	EXPECT_EQ(p->ItemLevelOfSlot(Protocol::SLOT_BOOTS), 1u);
	EXPECT_EQ(p->ItemLevelOfSlot(Protocol::SLOT_HELMET), 0u) << "빈 슬롯은 0";
}

// 스킬 계수의 힘 - 무기의 주 능력치 하나. AD/AP 는 피해 종류만 가른다 (2026-10-10 결정: 단검 AP 스킬이 마력 0 으로 0 이 되지 않게)
TEST(SkillPower, 물리_무기의_AP_스킬은_공격력에_곱하고_마법_저항이_막는다)
{
	Creature caster;
	caster.attackPower = 20;
	caster.magicPower = 0;
	caster.critChance = 0.f;
	caster.autoAttackType = Protocol::DAMAGE_TYPE_PHYSICAL;

	Creature armored;
	armored.armor = 100;		// 물리 50 % 완화
	armored.magicResist = 0;

	Creature warded;
	warded.armor = 0;
	warded.magicResist = 100;	// 마법 50 % 완화

	bool crit = false;
	EXPECT_EQ(caster.SkillPower(), 20);
	EXPECT_EQ(caster.ComputeSkillDamageTo(armored, 0.f, 1.f, OUT crit), 20) << "AP 는 방어력을 무시한다";
	EXPECT_EQ(caster.ComputeSkillDamageTo(warded, 0.f, 1.f, OUT crit), 10) << "AP 는 마법 저항이 막는다";
	EXPECT_EQ(caster.ComputeSkillDamageTo(warded, 1.f, 0.f, OUT crit), 20) << "AD 는 마법 저항을 무시한다";
	EXPECT_EQ(caster.ComputeSkillDamageTo(warded, 1.f, 1.f, OUT crit), 30) << "하이브리드는 항마다 따로 완화";
}

TEST(SkillPower, 마법_무기는_마력에_곱한다)
{
	Creature caster;
	caster.attackPower = 10;	// 맨몸 공격력은 남아 있다 (EquipStats - 마법 무기는 마력만 더한다)
	caster.magicPower = 30;
	caster.critChance = 0.f;
	caster.autoAttackType = Protocol::DAMAGE_TYPE_MAGIC;

	Creature dummy;
	bool crit = false;

	EXPECT_EQ(caster.SkillPower(), 30);
	EXPECT_EQ(caster.ComputeSkillDamageTo(dummy, 0.f, 2.f, OUT crit), 60);
	EXPECT_EQ(caster.ComputeSkillDamageTo(dummy, 2.f, 0.f, OUT crit), 60) << "스태프의 AD 계수도 마력으로";
}
