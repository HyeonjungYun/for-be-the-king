#pragma once
#include "Object.h"
#include "StatusEffects.h"

constexpr int32		SKILL_SLOT_COUNT = 6;

namespace BaseStats
{
	constexpr int32		MAX_HP						= 500;	
	constexpr int32		ATTACK_POWER				= 10;
	constexpr int32		MAGIC_POWER					= 0;
	constexpr float		ATTACK_SPEED				= 1.0f;
	constexpr float		RESISTANCE					= 0.f;
	constexpr int32		ARMOR						= 0;
	constexpr int32		MAGIC_RESIST				= 0;
	constexpr float		ATTACK_RANGE				= 150.f;
	constexpr float		CRIT_CHANCE					= 0.f;
	constexpr float		CRIT_MULITPLIER				= 1.75f;

	const float			K_MITIGATION				= 100.f;
	constexpr int32		MIN_DAMAGE					= 1;

	constexpr float		AS_MIN_RATIO				= 0.5;
	constexpr float		AS_MAX_RATIO				= 2.5;
	constexpr float		WINDUP_RATIO				= 0.3f;
	constexpr uint32	WINDUP_FLOOR_MS				= 150;
	constexpr float		RESISTANCE_MAX_REDUCTION	= 0.4f;
	constexpr float		SOFT_CC_SLOW_CAP			= 0.4f;
	constexpr uint64	CC_ACK_GRACE_US				= 200'000;
}

struct SlowSource
{
	float	magnitude = 0.f;
	uint64	endUs = 0;
};

struct SkillSlot
{
	uint32						skillId = 0;
	Protocol::SlotBindState		bindState = Protocol::BIND_UNBOUND;
	Protocol::EquipSlot			shadowedBy = Protocol::SLOT_NONE;

	uint64						cooldownEndUs = 0;
};

class Creature : public Object
{
public:
						Creature();
	virtual				~Creature();

public:
	bool				IsAlive() const { return hp > 0; }

	float				GetEffectiveAttackSpeed() const;
	uint32				GetAttackIntervalMs() const;
	uint32				GetAttackWindupMs() const;
	uint32				GetAttackRecoveryMs() const;

	int32				GetDefense(Protocol::DamageType type) const;

	int32				ApplyMitigation(float baseDamage, const Creature& target, Protocol::DamageType type) const;

	int32				ComputeDamageTo(const Creature& target, Protocol::DamageType type, OUT bool& isCrit) const;

	// 스킬 계수가 곱해지는 힘 - 무기의 주 능력치 하나 (물리 무기 = 공격력 · 마법 무기 = 마력 · 몬스터 = 공격력).
	// AD/AP 는 피해 종류(방어력 / 마법 저항 중 무엇이 막는가)만 가른다 - 단검의 AP 스킬이 마력 0 으로 0 이 되지 않게
	// (2026-10-10 에이전트 결정. 물리 무기에 magic_power 를 주는 길은 C3-b 로더 규칙이 막는다)
	int32				SkillPower() const { return autoAttackType == Protocol::DAMAGE_TYPE_MAGIC ? magicPower : attackPower; }

	int32				ComputeSkillDamageTo(const Creature& target, float adRatio, float apRatio, OUT bool& isCrit) const;

	int32				TakeDamage(int32 amount);
	uint32				ComputeCcDurationMs(uint32 baseMs) const;

	void				ApplyHardCc(Protocol::CcType type, uint64 nowUs, uint32 effectiveMs);
	void				ApplySoftCc(Protocol::CcType type, uint64 nowUs, uint32 effectiveMs, float magnitude);

	// 만료된 감속원을 걷어내고 activeSlow를 다시 뽑는다.
	void				RefreshActiveSlow(uint64 nowUs);

	bool				IsHardCced(uint64 nowUs) const;
	bool				CanMove(uint64 nowUs) const;
	bool				CanTurn(uint64 nowUs) const;
	bool				CanAttack(uint64 nowUs) const;
	bool				CanUseSkill(uint64 nowUs) const;

	// 버프가 걸리거나 끝났다 - 플레이어는 장비 합산 위에 버프를 다시 얹는다 (Player::RebuildCombatStats). 몬스터는 버프가 없다
	virtual void		OnBuffsChanged() {}

	// 전투 중 상태를 전부 지운다 - 입장 · 몬스터 복귀. CC · 상태 효과 · 재입력 · 시전
	void				ClearCombatState();

public:
	SkillSlot*			GetSlot(Protocol::EquipSlot slot);
	const SkillSlot*	GetSlot(Protocol::EquipSlot slot) const;

	void				RefreshBindStates();

public:
	bool IsCasting()	{ return castEndAtUs != -0; }

public:
	int32				maxHp			= BaseStats::MAX_HP;
	int32				hp				= BaseStats::MAX_HP;
	int32				attackPower		= BaseStats::ATTACK_POWER;
	int32				magicPower		= BaseStats::MAGIC_POWER;
	float				attackSpeed		= BaseStats::ATTACK_SPEED;
	float				percentAsBonus	= 0.f;
	float				resistance		= BaseStats::RESISTANCE;
	int32				armor				= BaseStats::ARMOR;
	int32				magicResist		= BaseStats::MAGIC_RESIST;
	float				attackRange		= BaseStats::ATTACK_RANGE;
	float				critChance		= BaseStats::CRIT_CHANCE;
	float				critMultiplier	= BaseStats::CRIT_MULITPLIER;
	Protocol::DamageType autoAttackType	= Protocol::DAMAGE_TYPE_PHYSICAL;	// 맨몸 · 몬스터는 물리. 플레이어는 무기가 정한다
	bool				rangedAutoAttack = false;	// 활 · 스태프 - 평타가 투사체로 날아간다 (필중 · 날아가는 중 벽에 막힘). 2 단계에서 쓴다
	float				outgoingDamagePct = 0.f;	// damage_pct 버프 합 - 평타 · 스킬 피해 × (1 + 이 값). 지속 피해는 걸 때 고정

public:
	uint64				stunEndUs = 0;
	uint64				rootEndUs = 0;
	uint64				knockbackEndUs = 0;
	uint64				launchEndUs = 0;
	uint64				silenceEndUs = 0;
	uint64				healReductionEndUs = 0;
	float				healReductionMagnitude = 0.f;
	uint64				blindEndUs = 0;				// 실명 - 소프트. 서버는 시간만 센다, 시야 축소는 클라 (skill_rules blind_view_scale)

public:
	// 상태 효과 - 지속 피해 · 버프 · 표식 (StatusEffects.h). Room 이 Flush 마다 틱 · 만료를 돌린다
	StatusSet			status;

	// 재입력 (kind recast) - 첫 발동 뒤 recast_window_ms 안의 같은 키. 두 번째 입력은 첫 조준 대상을 그대로 쓴다
	uint32				recastSkillId = 0;
	Protocol::EquipSlot	recastSlot = Protocol::SLOT_NONE;
	uint64				recastUntilUs = 0;
	uint64				recastTargetId = 0;
	float				recastAimX = 0.f;
	float				recastAimY = 0.f;

	vector<SlowSource>	slowSources;

	float				activeSlow = 0.f;

	uint64				speedGraceUntilUs = 0;
	float				preGraceSlow = 0.f;

public:
	uint64				attackTargetId	= 0;
	uint64				attackHitAtUs	= 0;
	uint64				attackReadyAtUs	= 0;

public:
	uint64				castEndAtUs = 0;
	uint32				castSkillId = 0;
	Protocol::EquipSlot castSlot = Protocol::SLOT_NONE;
	uint64				castTargetId = 0;
	float				castAimX = 0.f;
	float				castAimY = 0.f;
	uint32				castItemLevel = 0;			// 시전 슬롯 아이템의 강화 레벨 - 버프 크기 (사용자 결정: 스킬 레벨 = 아이템 강화). 몬스터 0

public:
	SkillSlot			skillSlots[SKILL_SLOT_COUNT];
};

