#pragma once
#include "Object.h"

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

	const float			K_MITIGATION					= 100.f;
	constexpr int32		MIN_DAMAGE					= 1;

	constexpr float		AS_MIN_RATIO				= 0.5;
	constexpr float		AS_MAX_RATIO				= 2.5;
	constexpr float		WINDUP_RATIO				= 0.3f;
	constexpr uint32	WINDUP_FLOOR_MS				= 150;
	constexpr float		RESISTANCE_MAX_REDUCTION	= 0.4f;
	constexpr float		SOFT_CC_SLOW_CAP			= 0.4f;
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

public:
	uint64				stunEndUs = 0;
	uint64				rootEndUs = 0;
	uint64				knockbackEndUs = 0;
	uint64				launchEndUs = 0;
	uint64				silenceEndUs = 0;
	uint64				healReductionEndUs = 0;
	float				healReductionMagnitude = 0.f;

	vector<SlowSource>	slowSources;

	float				activeSlow = 0.f;

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

public:
	SkillSlot			skillSlots[SKILL_SLOT_COUNT];
};

