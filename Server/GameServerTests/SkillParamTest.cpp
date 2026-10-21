#include "pch.h"
#include "SkillTable.h"
#include "ItemTable.h"

// skill_effects.csv param 의 로드 시 해석 (SkillTable::ParseParam, 2026-10-21 리뷰 후속).
// 예전엔 Room 이 발동 · 적중마다 "shape:cone:…" 을 가르고 "side" · "far" · ";still" 을 find 하고 "tag:" 선택자를 비교했다.
// 이제 로드 때 한 번 - 여기서는 해석 결과가 Room 이 보던 것과 같은지 본다

namespace
{
	SkillEffect Make(SkillEffectType type, const char* param)
	{
		SkillEffect e;
		e.type = type;
		e.param = param;
		SkillTable::ParseParam(e);
		return e;
	}

	const SkillEffect* FirstOf(uint32 skillId, SkillEffectType type)
	{
		const SkillDef* def = SkillTable::Find(skillId);
		return (def != nullptr) ? def->FindEffect(type) : nullptr;
	}
}

TEST(SkillParam, 재입력_모양_cone과_circle)
{
	const SkillEffect cone = Make(SkillEffectType::Damage, "shape:cone:150:300");
	EXPECT_EQ(cone.recastShape, SkillEffect::RecastShape::Cone);
	EXPECT_FLOAT_EQ(cone.recastAngleDeg, 150.f);
	EXPECT_FLOAT_EQ(cone.recastRadiusCm, 300.f);

	const SkillEffect circle = Make(SkillEffectType::Damage, "shape:circle:250");
	EXPECT_EQ(circle.recastShape, SkillEffect::RecastShape::Circle);
	EXPECT_FLOAT_EQ(circle.recastRadiusCm, 250.f);

	// 못 읽는 모양 · 모양이 아닌 param 은 None - 원래 판정 모양 그대로
	EXPECT_EQ(Make(SkillEffectType::Damage, "shape:bogus").recastShape, SkillEffect::RecastShape::None);
	EXPECT_EQ(Make(SkillEffectType::Damage, "shape:cone:150").recastShape, SkillEffect::RecastShape::None);
	EXPECT_EQ(Make(SkillEffectType::Movement, "dash").recastShape, SkillEffect::RecastShape::None);
}

TEST(SkillParam, 넉백_꼬리표_side_far_nostun)
{
	const SkillEffect sideNoStun = Make(SkillEffectType::Knockback, "side+nostun");
	EXPECT_TRUE(sideNoStun.flagSide);
	EXPECT_TRUE(sideNoStun.flagNoStun);
	EXPECT_FALSE(sideNoStun.flagFar);

	// ("far" · "near" 는 windows.h 매크로라 변수 이름으로 쓰지 않는다)
	const SkillEffect flyFar = Make(SkillEffectType::Knockback, "far");
	EXPECT_TRUE(flyFar.flagFar);
	EXPECT_FALSE(flyFar.flagSide);
	EXPECT_FALSE(flyFar.flagNoStun);

	const SkillEffect plain = Make(SkillEffectType::Knockback, "");
	EXPECT_FALSE(plain.flagSide || plain.flagFar || plain.flagNoStun || plain.flagStill || plain.isPuddle);
}

TEST(SkillParam, 선택자_tag_elem_skill_range_와_still_꼬리)
{
	const SkillEffect tag = Make(SkillEffectType::DamageMod, "tag:ghost");
	EXPECT_EQ(tag.selector, SkillEffect::Selector::Tag);
	EXPECT_EQ(tag.selectorValue, "ghost");
	EXPECT_FALSE(tag.flagStill);

	const SkillEffect still = Make(SkillEffectType::DamageMod, "tag:hammer;still");
	EXPECT_EQ(still.selector, SkillEffect::Selector::Tag);
	EXPECT_EQ(still.selectorValue, "hammer") << "';still' 꼬리는 선택자 값에서 뺀다 (Room 이 그렇게 비교했다)";
	EXPECT_TRUE(still.flagStill);

	const SkillEffect elem = Make(SkillEffectType::DamageMod, "elem:fire");
	EXPECT_EQ(elem.selector, SkillEffect::Selector::Elem);
	EXPECT_EQ(elem.selectorValue, "fire");

	const SkillEffect skill = Make(SkillEffectType::DamageMod, "skill:1050");
	EXPECT_EQ(skill.selector, SkillEffect::Selector::Skill);
	EXPECT_EQ(skill.selectorSkillId, 1050u);

	const SkillEffect range = Make(SkillEffectType::DamageMod, "range");
	EXPECT_EQ(range.selector, SkillEffect::Selector::Range);

	EXPECT_EQ(Make(SkillEffectType::DamageMod, "bogus").selector, SkillEffect::Selector::None);
	EXPECT_TRUE(Make(SkillEffectType::Zone, "puddle").isPuddle);
	EXPECT_FALSE(Make(SkillEffectType::Zone, "poison").isPuddle);
}

TEST(SkillParam, 실제_데이터에서_해석된다)
{
	string error;
	ASSERT_TRUE(SkillTable::Load(ItemTable::DefaultDataDir(), OUT error)) << error;

	// skill_effects.csv 의 실제 행 - 질풍노도 1108 side+nostun · 저 멀리 1109 far · 밀쳐내기 1110 nostun
	const SkillEffect* side = FirstOf(1108, SkillEffectType::Knockback);
	ASSERT_NE(side, nullptr);
	EXPECT_TRUE(side->flagSide && side->flagNoStun && side->flagFar == false);

	const SkillEffect* farKnockback = FirstOf(1109, SkillEffectType::Knockback);
	ASSERT_NE(farKnockback, nullptr);
	EXPECT_TRUE(farKnockback->flagFar && farKnockback->flagSide == false);

	const SkillEffect* noStun = FirstOf(1110, SkillEffectType::Knockback);
	ASSERT_NE(noStun, nullptr);
	EXPECT_TRUE(noStun->flagNoStun && noStun->flagSide == false);

	// 건곤일척 1111 tag:hammer;still · 원한 1083 tag:ghost · 1097 skill:1050 · 명사수 1098 range · 1100 elem:fire
	const SkillEffect* still = FirstOf(1111, SkillEffectType::DamageMod);
	ASSERT_NE(still, nullptr);
	EXPECT_TRUE(still->flagStill);
	EXPECT_EQ(still->selector, SkillEffect::Selector::Tag);
	EXPECT_EQ(still->selectorValue, "hammer");

	const SkillEffect* ghost = FirstOf(1083, SkillEffectType::DamageMod);
	ASSERT_NE(ghost, nullptr);
	EXPECT_EQ(ghost->selectorValue, "ghost");

	const SkillEffect* bySkill = FirstOf(1097, SkillEffectType::DamageMod);
	ASSERT_NE(bySkill, nullptr);
	EXPECT_EQ(bySkill->selector, SkillEffect::Selector::Skill);
	EXPECT_EQ(bySkill->selectorSkillId, 1050u);

	const SkillEffect* range = FirstOf(1098, SkillEffectType::DamageMod);
	ASSERT_NE(range, nullptr);
	EXPECT_EQ(range->selector, SkillEffect::Selector::Range);

	// 찌르고 베기 1118 - 재입력 효과의 shape:cone:150:300 · 명경지수 1130 puddle
	const SkillDef* stab = SkillTable::Find(1118);
	ASSERT_NE(stab, nullptr);
	bool foundCone = false;
	for (const SkillEffect& e : stab->effects)
	{
		if (e.condition == SkillRequire::Recast && e.recastShape == SkillEffect::RecastShape::Cone)
		{
			foundCone = true;
			EXPECT_FLOAT_EQ(e.recastAngleDeg, 150.f);
			EXPECT_FLOAT_EQ(e.recastRadiusCm, 300.f);
		}
	}
	EXPECT_TRUE(foundCone);

	const SkillEffect* puddle = FirstOf(1130, SkillEffectType::Zone);
	ASSERT_NE(puddle, nullptr);
	EXPECT_TRUE(puddle->isPuddle);
}
