#pragma once

/*----------------
	LonePath
----------------*/

// 고독한 길 (2026-10-17 사용자 결정 - design/quick-specs/lone-path-2026-10-17.md).
// 기존 장비에 낮은 확률로 붙는 옵션. 하나라도 착용하면 파티를 맺을 수 없고(착용 중에만), 착용 수만큼 다구대일에 강해진다:
// 주변에 적이 여럿이면 받는 피해 · CC 지속이 준다 + 공격력 · 마력이 조금 오른다.
// Room 을 모르는 순수 로직 - 착용 수 · 주변 적 수를 인자로 받는다 (LonePathTest.cpp)

struct LonePathRules
{
	uint32		rollPct = 5;					// 아이템이 만들어질 때 옵션이 붙을 확률 (%)
	float		damageReducePerPiece = 0.08f;	// 다구대일일 때 받는 피해 - 개당
	float		ccReducePerPiece = 0.10f;		// 다구대일일 때 CC 지속 - 개당
	float		attackPerPiece = 0.03f;			// 공격력 · 마력 - 개당, 상황 무관
	int32		maxPieces = 4;					// 효과가 쌓이는 상한 (4 개: 피해 -32 % · CC -40 % - 기획서의 저항 상한 40 % 와 같다)
	float		outnumberRadiusCm = 800.f;		// 이 거리 안의
	int32		outnumberEnemies = 2;			// 적이 이 수 이상이면 다구대일

	// lone_path.csv (key,value). 모르는 키 · 범위 밖은 error 를 남기고 false. 없는 키는 기본값
	bool		Parse(const string& csv, OUT string& error);

	// 파일이 없으면 기본값 그대로 true
	bool		Load(const string& dataDir, OUT string& error);

	static LonePathRules& Get();
};

namespace LonePath
{
	// 효과에 쓰는 착용 수 - 0 .. maxPieces
	int32		EffectivePieces(int32 worn, const LonePathRules& rules);

	// 다구대일인가 - 주변 적 수가 기준 이상
	bool		Outnumbered(int32 nearbyEnemies, const LonePathRules& rules);

	// 받는 피해 배율 (1 = 그대로). 다구대일이 아니면 1
	float		DamageTakenMult(int32 worn, int32 nearbyEnemies, const LonePathRules& rules);

	// CC 지속 배율 (1 = 그대로). 다구대일이 아니면 1
	float		CcDurationMult(int32 worn, int32 nearbyEnemies, const LonePathRules& rules);

	// 공격력 · 마력 가산 비율 (0 = 그대로)
	float		AttackBonus(int32 worn, const LonePathRules& rules);

	// 옵션을 굴린다 - roll 은 0 .. 99 의 균등 정수
	bool		Rolls(uint32 roll, const LonePathRules& rules);

	// 아이템 고유 번호 → 0 .. 99 (splitmix64). 아이템 롤의 난수열을 건드리지 않고 굴리려고
	uint32		RollOf(uint64 instanceId);
}
