#pragma once
#include "Object.h"
#include "StatusEffects.h"
#include "PositionHistory.h"

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
	constexpr float		CRIT_MULTIPLIER				= 1.75f;

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

// 결속된 패시브 스킬이면 그 정의, 아니면 nullptr. "bindState != BOUND || skillId == 0 → continue · Find · kind != Passive → continue" 가
// Room*.cpp 16 곳에 복붙돼 있었다 (TD-011, 2026-10-21). 쿨다운 · 무기 조건 같은 추가 검사는 부르는 쪽이 한다
inline const SkillDef* BoundPassiveOf(const SkillSlot& slot)
{
	if (slot.bindState != Protocol::BIND_BOUND || slot.skillId == 0)
		return nullptr;

	const SkillDef* def = SkillTable::Find(slot.skillId);
	return (def != nullptr && def->kind == SkillKind::Passive) ? def : nullptr;
}

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

	// 장착(결속)된 스킬의 패시브 스탯 합 (passive_stat - 모래주머니 이속 · 날개 move_dist, 2026-10-11 3차 로스터)
	float				PassiveStatSum(SkillBuffStat stat) const;
	// 결속된 패시브의 CC 저항 (cc_resist param slow · knockback) - 가장 큰 것. 0 ~ 1
	float				CcResistOf(const string& kind) const;

	// 전투 중 상태를 전부 지운다 - 입장 · 몬스터 복귀. CC · 상태 효과 · 재입력 · 시전
	void				ClearCombatState();

public:
	SkillSlot*			GetSlot(Protocol::EquipSlot slot);
	const SkillSlot*	GetSlot(Protocol::EquipSlot slot) const;

	void				RefreshBindStates();

public:
	bool IsCasting() const	{ return castEndAtUs != 0; }

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
	float				critMultiplier	= BaseStats::CRIT_MULTIPLIER;
	Protocol::DamageType autoAttackType	= Protocol::DAMAGE_TYPE_PHYSICAL;	// 맨몸 · 몬스터는 물리. 플레이어는 무기가 정한다
	bool				rangedAutoAttack = false;	// 활 · 스태프 - 평타가 투사체로 날아간다 (필중 · 날아가는 중 벽에 막힘). 2 단계에서 쓴다
	float				outgoingDamagePct = 0.f;	// damage_pct 버프 합 - 평타 · 스킬 피해 × (1 + 이 값). 지속 피해는 걸 때 고정

public:
	uint64				stunEndUs = 0;
	uint64				rootEndUs = 0;
	uint64				knockbackEndUs = 0;
	uint64				launchEndUs = 0;
	uint64				lastDisplacedEndUs = 0;		// 마지막 넉백 · 띄움이 끝나는 시각 - CC 가 풀려도 지우지 않는다 (탈출 망설임 지표 OQ-7)
	uint64				silenceEndUs = 0;
	uint64				healReductionEndUs = 0;
	float				healReductionMagnitude = 0.f;
	uint64				blindEndUs = 0;				// 실명 - 소프트. 서버는 시간만 센다, 시야 축소는 클라 (skill_rules blind_view_scale)
	float				blindScale = 1.f;			// 실명 중 시야 배율 - 걸 때의 크기 (실명 스킬 = blind_view_scale · 연막 = 0.4). FoW 반경이 본다
	uint64				tauntEndUs = 0;				// 도발 - 하드. 시전자(tauntSourceId)에게 taunt_speed_cms 로 끌려간다 (Room::StepTaunts), 평타 · 스킬 · 회전 불가
	uint64				tauntSourceId = 0;

	bool				IsTaunted(uint64 nowUs) const { return nowUs < tauntEndUs; }

public:
	// 팀 (PVE 협동, ADR-0014) - 0 = 팀 없음 (모두에게 적대, PvP). 같은 0 아닌 값끼리 아군 - 피해 · CC · 대상 지정이 막힌다.
	// 협동 인스턴스에 들어갈 때 파티 id 를 받고 그 런 동안 바뀌지 않는다. 미끼 · 분신은 주인 것을 받는다
	uint64				teamId = 0;

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

	// ── 3 단계 ──
	// 유지형 (kind hold) - 같은 키를 다시 누르면 끝. holdEndUs 0 = 상한 없음 (빈쿨룸). 틱마다 UpdateHolds
	uint32				holdSkillId = 0;
	Protocol::EquipSlot	holdSlot = Protocol::SLOT_NONE;
	uint32				holdEffectIndex = 0;
	uint64				holdStartUs = 0;
	uint64				holdEndUs = 0;
	uint64				holdNextTickUs = 0;
	float				holdOriginX = 0.f;		// 회오리 - 시작 자리 (can_move 0 이라 그 자리)
	float				holdOriginY = 0.f;

	// 반격 (정좌) - 평타 · 대상 지정 · 직선 스킬을 받으면 무효 + 반격 피해, 한 번 받으면 끝
	uint64				counterUntilUs = 0;
	float				counterRatio = 0.f;
	uint32				counterSkillId = 0;

	// 받아넘기기 (유수) - 전방에서 오는 평타 · 투사체 무효
	uint64				parryUntilUs = 0;

	// 받아치기 (자업자득 #107) - 날아오는 스킬 투사체를 쏜 쪽으로 되돌려 보낸다. 평타 투사체는 못 받아친다
	uint64				reflectUntilUs = 0;

	// 경독 - 창이 열린 동안 내 단검 평타에 맞은 적을 모았다가 창이 닫힐 때 기절
	uint64				petrifyEndUs = 0;
	uint32				petrifyStunMs = 0;
	vector<uint64>		petrifyTargets;

	// 스태프 속성 - 지속 없음 · 교체 · clear 로 지움. stanceRatio = 속성이 깃든 동안 평타 추가 피해 계수(AP)
	string				stance;
	float				stanceRatio = 0.f;

	bool IsCountering(uint64 nowUs) const { return nowUs < counterUntilUs; }
	bool IsParrying(uint64 nowUs) const { return nowUs < parryUntilUs; }
	bool IsReflecting(uint64 nowUs) const { return nowUs < reflectUntilUs; }
	bool IsHolding() const { return holdSkillId != 0; }

	// ── 모자 로스터 (2026-10-10) ──
	uint64				focusTargetId = 0;			// 일편단심 - 이 적에게만 피해, 그 대신 × (1 + focusBonus). 표식이 끝나면 풀린다
	uint64				focusUntilUs = 0;
	float				focusBonus = 0.f;
	uint64				doomUntilUs = 0;			// 배수진 - 저주 중. 적 플레이어 처치로 해제, 아니면 끝에 죽는다
	uint32				doomSkillId = 0;
	bool IsFocusing(uint64 nowUs) const { return focusTargetId != 0 && nowUs < focusUntilUs; }
	bool IsDoomed(uint64 nowUs) const { return doomSkillId != 0 && nowUs < doomUntilUs; }

	// ── 4 단계 ──
	uint64				invulnUntilUs = 0;			// 니힐 - 피해 · CC · 밀기 전부 무효
	uint64				leapUntilUs = 0;			// 벽력일창 - 도약 중 (맞으면 추락 = 예고된 내리찍기 취소)
	uint32				leapSkillId = 0;

	bool IsInvulnerable(uint64 nowUs) const { return nowUs < invulnUntilUs; }
	bool IsLeaping(uint64 nowUs) const { return nowUs < leapUntilUs; }

	// ── 후딜 (2026-10-15 사용자) - 스킬이 나간 뒤 애니메이션이 끝날 때까지 이동 · 평타 · 다른 스킬이 막힌다 ──
	// 그 스킬 자신의 돌진 · 도약 창 안의 이동은 막지 않는다 (Room::HandleMove). 재입력의 두 번째 입력도 막지 않는다
	uint64				skillLockUntilUs = 0;
	bool IsSkillLocked(uint64 nowUs) const { return nowUs < skillLockUntilUs; }

	// ── FoW (2026-10-11) ──
	// 은신 (그림자 감추기 · 니힐) - 다른 플레이어에게 보내지 않는다 (탐지 시야원만 본다). 대상으로 지정할 수 없고 몬스터가 놓친다.
	// 공격 · 스킬을 쓰면 풀린다. stealthBreaksOnDamage 면 피해를 받아도 풀린다 (그림자 감추기 - 사용자 결정)
	uint64				stealthUntilUs = 0;
	uint32				stealthSkillId = 0;
	bool				stealthBreaksOnDamage = false;
	// 야생의 체취 - 몬스터가 표적으로 삼지 않는다 (공격해도)
	uint64				scentUntilUs = 0;

	bool IsStealthed(uint64 nowUs) const { return nowUs < stealthUntilUs; }
	bool IsScented(uint64 nowUs) const { return nowUs < scentUntilUs; }

	// 보호막 (전사의 다짐) - 받는 피해를 먼저 막는다. 다 깎이거나 시간이 다 되면 사라진다
	int32				shieldAmount = 0;
	uint64				shieldUntilUs = 0;
	uint32				shieldSkillId = 0;
	bool HasShield(uint64 nowUs) const { return shieldAmount > 0 && nowUs < shieldUntilUs; }

	// 테스트 모드 /t god - 체력이 1 아래로 내려가지 않는다 (피해 숫자는 그대로 나간다). 죽음 · 사망 커밋이 없다
	bool				testGod = false;

	// ── 3차 로스터 (2026-10-11) - RoomSkills4.cpp ──
	uint32				anchorSkillId = 0;			// 비창 · 포르타 - 재입력으로 돌아갈 자리 (그 스킬의 재입력 창 동안)
	bool				anchorSet = false;			// 비창은 던진 창이 멈춰야 자리가 생긴다
	float				anchorX = 0.f;
	float				anchorY = 0.f;
	uint64				anchorUntilUs = 0;
	uint64				anchorZoneId = 0;			// 자리 표시 (연출 장판) - 쓰면 일찍 지운다
	bool				parryAllProjectiles = false;	// 풍차 - 받아넘기기가 사방의 투사체만 (근접은 막지 않는다)
	float				shieldBurstRatio = 0.f;		// 스쿠툼 - 속성이 깃든 채 깨지면 둘레에 그 속성 피해 (AP 계수)
	float				shieldBurstRadiusCm = 0.f;
	uint64				lastCombatUs = 0;			// 마지막으로 피해를 주거나 받은 때 (행군)
	PositionHistory		history;					// 지난 8 초의 자리 (10 Hz) - 회귀 · 발자국 추적

	vector<SlowSource>	slowSources;

	float				activeSlow = 0.f;

	uint64				speedGraceUntilUs = 0;
	float				preGraceSlow = 0.f;

public:
	uint64				attackTargetId	= 0;
	uint64				attackHitAtUs	= 0;
	uint64				attackReadyAtUs	= 0;

	// 대상 없는 평타 (2026-10-11) - 1 = 일보일검 돌진 횡베기, 2 = 난무 사방 베기. 0 = 보통 평타 (attackTargetId)
	uint8				attackMode		= 0;
	float				attackDirX		= 1.f;
	float				attackDirY		= 0.f;

	// 건곤일척 - 쓸 때 서 있던 자리. 여기서 움직이면 망치 스킬 피해 증가가 사라진다 (0 = 없음)
	uint32				stillSkillId	= 0;
	float				stillAnchorX	= 0.f;
	float				stillAnchorY	= 0.f;

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

// Object 플래그로 거른 static 캐스트 - Creature 를 상속하는 것은 Player · Monster · Decoy 뿐이다 (dynamic_pointer_cast 의 RTTI 비용을 뺀다, 2026-10-17)
template<typename T>
shared_ptr<Creature> AsCreature(const shared_ptr<T>& object)
{
	return (object != nullptr && object->IsCreature()) ? static_pointer_cast<Creature>(object) : nullptr;
}

// 결속된 패시브마다 fn(slotIndex, SkillSlot&, const SkillDef&) - 슬롯 순서대로 (BoundPassiveOf 의 순회판, TD-011)
template<typename Fn>
void ForEachBoundPassive(Creature& owner, Fn&& fn)
{
	for (int32 i = 0; i < SKILL_SLOT_COUNT; i++)
	{
		if (const SkillDef* def = BoundPassiveOf(owner.skillSlots[i]))
			fn(i, owner.skillSlots[i], *def);
	}
}
