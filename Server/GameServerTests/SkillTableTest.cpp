#include "pch.h"
#include "SkillTable.h"

// 🔴 하드 CC 는 무기 부위 전용이다. 캐릭터당 하드 CC 를 1개로 묶는 것이
//    AC-5(4인 체인 CC ≤ 5초) 예산의 전제다. 갑옷에도 붙으면 4인이 8회를 건다.
//
//    이 규칙을 지키는 곳은 착용이 아니라 SkillDef 정의다. 새 하드 CC 스킬을
//    다른 부위로 정의하면 이 테스트가 실패한다.
TEST(SkillTable, 하드CC_스킬은_무기_부위에만_붙는다)
{
	SkillTable::Init();

	int32 hardCcCount = 0;

	for (const auto& [skillId, def] : SkillTable::All())
	{
		if (def.HasHardCc() == false)
			continue;

		++hardCcCount;

		EXPECT_EQ(def.allowedSlot, Protocol::SLOT_WEAPON_PRIMARY)
			<< "skillId=" << skillId << " 는 하드 CC 인데 무기 전용이 아니다";
	}

	// 🔴 하드 CC 스킬이 하나도 없으면 위 루프는 아무것도 검사하지 않고 통과한다.
	//    "검증이 통과했다" 가 "검증할 대상이 없었다" 와 구분되지 않는 것을 막는다
	EXPECT_GT(hardCcCount, 0) << "하드 CC 스킬이 없다 — 이 테스트가 아무것도 검증하지 않았다";
}
