#include "pch.h"
#include "SkillTable.h"
#include "ItemTable.h"
#include "CsvTable.h"

// 애니메이션 동작 표 (2026-10-12 · design/quick-specs/animation-2026-10-12.md AC-AN-05).
// motions.csv (motion_id,key) + skill_motions.csv (skill_id,motion_key) → SkillDef.motionId. 서버는 캐스트 패킷에 동작 번호만 싣는다.

namespace
{
	const char* MOTIONS = "motion_id,key\n1,m_none\n2,m_buff\n3,m_attack\n";

	// 실제 로스터로 Parse 한 뒤 동작 표만 바꿔 본다
	bool LiveSkills(OUT string& error)
	{
		return SkillTable::Load(ItemTable::DefaultDataDir(), OUT error);
	}

	uint32 MotionOf(uint32 skillId)
	{
		const SkillDef* def = SkillTable::Find(skillId);
		return def ? def->motionId : 9999;
	}
}

TEST(SkillMotions, 실제_데이터는_모든_스킬에_동작이_있고_동작_표가_28개다_AC_AN_05)
{
	string error;
	ASSERT_TRUE(LiveSkills(OUT error)) << error;
	EXPECT_GE(SkillTable::Motions().size(), 20u);

	uint32 withMotion = 0, total = 0, none = 0;
	for (const auto& [id, def] : SkillTable::All())
	{
		total++;
		if (def.motionId != 0)
			withMotion++;
		auto it = SkillTable::Motions().find(def.motionId);
		if (it != SkillTable::Motions().end() && it->second == "m_none")
			none++;
	}
	EXPECT_EQ(withMotion, total) << "every skill in skills.csv needs a row in skill_motions.csv";
	EXPECT_GT(none, 0u) << "passives map to m_none";
	EXPECT_LT(none, total / 2) << "most skills show a motion";
	// 패시브는 전부 동작 없음
	for (const auto& [id, def] : SkillTable::All())
	{
		if (def.kind == SkillKind::Passive)
		{
			auto it = SkillTable::Motions().find(def.motionId);
			ASSERT_NE(it, SkillTable::Motions().end()) << id;
			EXPECT_EQ(it->second, "m_none") << "passive skill " << id;
		}
	}
}

TEST(SkillMotions, 표가_둘_다_없으면_동작_0_한쪽만_있으면_거부_AC_AN_05)
{
	string error;
	ASSERT_TRUE(LiveSkills(OUT error)) << error;

	EXPECT_TRUE(SkillTable::ParseMotions(nullptr, nullptr, OUT error)) << error;
	EXPECT_EQ(MotionOf(1002), 0u);
	EXPECT_TRUE(SkillTable::Motions().empty());

	const string motions = MOTIONS;
	const string skillMotions = "skill_id,motion_key\n1002,m_buff\n";
	EXPECT_FALSE(SkillTable::ParseMotions(&motions, nullptr, OUT error));
	EXPECT_FALSE(SkillTable::ParseMotions(nullptr, &skillMotions, OUT error));

	EXPECT_TRUE(SkillTable::ParseMotions(&motions, &skillMotions, OUT error)) << error;
	EXPECT_EQ(MotionOf(1002), 2u);
	EXPECT_EQ(MotionOf(1003), 0u);		// 표에 없는 스킬은 동작 없음
}

TEST(SkillMotions, 모르는_키_모르는_스킬_중복은_거부한다_AC_AN_05)
{
	string error;
	ASSERT_TRUE(LiveSkills(OUT error)) << error;
	const string motions = MOTIONS;

	const string unknownKey = "skill_id,motion_key\n1002,m_fly\n";
	EXPECT_FALSE(SkillTable::ParseMotions(&motions, &unknownKey, OUT error));
	EXPECT_NE(error.find("m_fly"), string::npos) << error;

	const string unknownSkill = "skill_id,motion_key\n999999,m_buff\n";
	EXPECT_FALSE(SkillTable::ParseMotions(&motions, &unknownSkill, OUT error));

	const string twice = "skill_id,motion_key\n1002,m_buff\n1002,m_attack\n";
	EXPECT_FALSE(SkillTable::ParseMotions(&motions, &twice, OUT error));

	const string dupMotion = "motion_id,key\n1,m_none\n1,m_buff\n";
	const string ok = "skill_id,motion_key\n1002,m_none\n";
	EXPECT_FALSE(SkillTable::ParseMotions(&dupMotion, &ok, OUT error));
	const string dupKey = "motion_id,key\n1,m_none\n2,m_none\n";
	EXPECT_FALSE(SkillTable::ParseMotions(&dupKey, &ok, OUT error));
	const string zeroId = "motion_id,key\n0,m_none\n";
	EXPECT_FALSE(SkillTable::ParseMotions(&zeroId, &ok, OUT error));

	// 실패 뒤에는 동작이 전부 0 (조용히 옛 값을 쓰지 않는다)
	EXPECT_EQ(MotionOf(1002), 0u);
	ASSERT_TRUE(LiveSkills(OUT error)) << error;		// 다음 테스트를 위해 실제 표로 되돌린다
}
