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

// 🔴 skill-system.md § S1 — 하드 CC 유형별 지속시간 상한. 표의 값은 확정값이 아니라 천장이다.
//    AC-5(4인 체인 CC 행동 불능 ≤ 5초) 예산은 전부 이 천장으로 계산했으므로
//    스킬 하나라도 넘으면 예산이 깨진다
TEST(SkillTable, AC5_하드CC_지속시간은_유형별_상한을_넘지_않는다)
{
	SkillTable::Init();

	const unordered_map<Protocol::CcType, uint32> capMs = {
		{ Protocol::CC_TYPE_STUN,		1250 },
		{ Protocol::CC_TYPE_LAUNCH,		1000 },
		{ Protocol::CC_TYPE_KNOCKBACK,	 375 },
		{ Protocol::CC_TYPE_ROOT,		1750 },
	};

	int32 hardCcEffectCount = 0;
	uint32 worstIncapacitationMs = 0;

	for (const auto& [skillId, def] : SkillTable::All())
	{
		// 한 스킬 안의 CC 는 끝나는 시각의 max 로 겹친다 — 합이 아니다
		uint32 incapacitationMs = 0;

		for (const SkillEffect& e : def.effects)
		{
			if (e.type != SkillEffectType::HardCc)
				continue;

			++hardCcEffectCount;

			auto it = capMs.find(e.ccType);
			ASSERT_NE(it, capMs.end()) << "skillId=" << skillId << " 상한이 정의되지 않은 하드 CC 유형 " << e.ccType;

			EXPECT_LE(e.ccDurationMs, it->second)
				<< "skillId=" << skillId << " ccType=" << e.ccType << " 가 상한을 넘는다";

			// 속박은 회전 · 평타 · 스킬이 되므로 행동 불능에 넣지 않는다
			if (e.ccType != Protocol::CC_TYPE_ROOT)
				incapacitationMs = max(incapacitationMs, e.ccDurationMs);
		}

		worstIncapacitationMs = max(worstIncapacitationMs, incapacitationMs);
	}

	EXPECT_GT(hardCcEffectCount, 0) << "하드 CC 효과가 없다 — 이 테스트가 아무것도 검증하지 않았다";

	// 하드 CC 는 무기 전용 → 캐릭터당 1개 → 4인이면 최대 4회 (위 테스트가 전제를 지킨다)
	EXPECT_LE(worstIncapacitationMs * 4, 5000u) << "가장 긴 하드 CC 로 4인 릴레이하면 5초를 넘는다";
}
