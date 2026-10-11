#include "pch.h"
#include "Creature.h"

Creature::Creature()
{
	objectInfo->set_object_type(Protocol::ObjectType::OBJECT_TYPE_CREATURE);
}

Creature::~Creature()
{
}

float Creature::GetEffectiveAttackSpeed() const
{
	const float eff = attackSpeed * (1.f + percentAsBonus);

	return std::clamp(eff,
		attackSpeed * BaseStats::AS_MIN_RATIO,
		attackSpeed * BaseStats::AS_MAX_RATIO);
}

uint32 Creature::GetAttackIntervalMs() const
{
	const float eff = GetEffectiveAttackSpeed();

	if (eff <= 0.f)
		return 0;

	return static_cast<uint32>(1000.f / eff);
}

uint32 Creature::GetAttackWindupMs() const
{
	const uint32 windup = static_cast<uint32>(BaseStats::WINDUP_RATIO * GetAttackIntervalMs());

	if (windup < BaseStats::WINDUP_FLOOR_MS)
		return BaseStats::WINDUP_FLOOR_MS;

	return windup;
}

uint32 Creature::GetAttackRecoveryMs() const
{
	const uint32 interval = GetAttackIntervalMs();
	const uint32 windup = GetAttackWindupMs();

	return (interval > windup) ? (interval - windup) : 0;
}

int32 Creature::GetDefense(Protocol::DamageType type) const
{
	return (type == Protocol::DAMAGE_TYPE_MAGIC) ? magicResist : armor;
}

int32 Creature::ApplyMitigation(float baseDamage, const Creature& target, Protocol::DamageType type) const
{
	if (baseDamage <= 0.f)
		return 0;

	const float defense = static_cast<float>(target.GetDefense(type));
	const float mitigationPct = defense / (defense + BaseStats::K_MITIGATION);

	const int32 dealt = static_cast<int32>(baseDamage * (1.f - mitigationPct));

	return (dealt < BaseStats::MIN_DAMAGE) ? BaseStats::MIN_DAMAGE : dealt;
}

int32 Creature::ComputeDamageTo(const Creature& target, Protocol::DamageType type, bool& isCrit) const
{
	isCrit = (critChance > 0.f) && (Utils::GetRandom(0.f, 1.f) < critChance);

	// 마법 무기 평타는 magic_power 로 때리고 magic_resist 로 막힌다 (combat-system.md Core Rule 2 · C3-b)
	const int32 power = (type == Protocol::DAMAGE_TYPE_MAGIC) ? magicPower : attackPower;
	const float effectiveBase = power * (isCrit ? critMultiplier : 1.f) * (1.f + outgoingDamagePct);

	return ApplyMitigation(effectiveBase, target, type);
}

int32 Creature::ComputeSkillDamageTo(const Creature& target, float adRatio, float apRatio, bool& isCrit) const
{
	isCrit = (critChance > 0.f) && (Utils::GetRandom(0.f, 1.f) < critChance);
	const float critMul = (isCrit ? critMultiplier : 1.f) * (1.f + outgoingDamagePct);

	// 두 항 모두 무기의 주 능력치에 곱한다 - 계수가 종류를, 능력치가 크기를 정한다 (S3: 항마다 따로 완화)
	const float power = static_cast<float>(SkillPower());
	int32 total = 0;

	if (adRatio > 0.f)
		total += ApplyMitigation(power * adRatio * critMul, target, Protocol::DAMAGE_TYPE_PHYSICAL);
	if (apRatio > 0.f)
		total += ApplyMitigation(power * apRatio * critMul, target, Protocol::DAMAGE_TYPE_MAGIC);

	return total;
}

void Creature::ClearCombatState()
{
	stunEndUs = rootEndUs = knockbackEndUs = launchEndUs = 0;
	silenceEndUs = healReductionEndUs = blindEndUs = 0;
	tauntEndUs = 0;
	tauntSourceId = 0;
	healReductionMagnitude = 0.f;
	slowSources.clear();
	activeSlow = 0.f;
	speedGraceUntilUs = 0;
	preGraceSlow = 0.f;

	attackTargetId = 0;
	attackHitAtUs = 0;
	attackMode = 0;
	stillSkillId = 0;

	castEndAtUs = 0;
	castSkillId = 0;
	castSlot = Protocol::SLOT_NONE;
	castTargetId = 0;
	castItemLevel = 0;

	recastSkillId = 0;
	recastSlot = Protocol::SLOT_NONE;
	recastUntilUs = 0;
	recastTargetId = 0;

	holdSkillId = 0;
	holdSlot = Protocol::SLOT_NONE;
	holdEndUs = holdStartUs = holdNextTickUs = 0;
	counterUntilUs = 0;
	counterRatio = 0.f;
	counterSkillId = 0;
	parryUntilUs = 0;
	reflectUntilUs = 0;
	petrifyEndUs = 0;
	petrifyStunMs = 0;
	petrifyTargets.clear();
	stance.clear();
	stanceRatio = 0.f;
	invulnUntilUs = 0;
	leapUntilUs = 0;
	leapSkillId = 0;
	focusTargetId = 0;
	focusUntilUs = 0;
	focusBonus = 0.f;
	doomUntilUs = 0;
	doomSkillId = 0;
	stealthUntilUs = 0;
	stealthSkillId = 0;
	stealthBreaksOnDamage = false;
	scentUntilUs = 0;
	shieldAmount = 0;
	shieldUntilUs = 0;
	shieldSkillId = 0;

	status.Clear();
	OnBuffsChanged();
}

int32 Creature::TakeDamage(int32 amount)
{
	if (amount < 0)
		amount = 0;

	hp -= amount;
	if (hp < 0)
		hp = 0;

	return hp;
}

uint32 Creature::ComputeCcDurationMs(uint32 baseMs) const
{
	const float factor = 1.f - resistance * BaseStats::RESISTANCE_MAX_REDUCTION;

	return static_cast<uint32>(baseMs * factor);
}

void Creature::ApplyHardCc(Protocol::CcType type, uint64 nowUs, uint32 effectiveMs)
{
	const uint64 endUs = nowUs + static_cast<uint64>(effectiveMs) * 1000;

	uint64* slot = nullptr;
	switch (type)
	{
	case Protocol::CC_TYPE_STUN:
		slot = &stunEndUs;
		break;
	case Protocol::CC_TYPE_ROOT:
		slot = &rootEndUs;
		break;
	case Protocol::CC_TYPE_KNOCKBACK:
		slot = &knockbackEndUs;
		break;
	case Protocol::CC_TYPE_LAUNCH:
		slot = &launchEndUs;
		break;
	case Protocol::CC_TYPE_TAUNT:
		slot = &tauntEndUs;
		break;
	default:
		return;
	}

	if (endUs > *slot)
		*slot = endUs;
}

void Creature::ApplySoftCc(Protocol::CcType type, uint64 nowUs, uint32 effectiveMs, float magnitude)
{
	const uint64 endUs = nowUs + static_cast<uint64>(effectiveMs) * 1000;

	switch (type)
	{
	case Protocol::CC_TYPE_SLOW:
	{
		float clamped = magnitude;
		if (clamped < 0.f)
			clamped = 0.f;
		if (clamped > BaseStats::SOFT_CC_SLOW_CAP)
			clamped = BaseStats::SOFT_CC_SLOW_CAP;

		if (nowUs >= speedGraceUntilUs)
			preGraceSlow = activeSlow;

		speedGraceUntilUs = nowUs + BaseStats::CC_ACK_GRACE_US;

		SlowSource& src = slowSources.emplace_back();
		src.magnitude = clamped;
		src.endUs = endUs;

		RefreshActiveSlow(nowUs);
		break;
	}
	case Protocol::CC_TYPE_SILENCE:
		if (endUs > silenceEndUs)
			silenceEndUs = endUs;
		break;
	case Protocol::CC_TYPE_HEAL_REDUCTION:
		if (endUs > healReductionEndUs)
			healReductionEndUs = endUs;
		if (magnitude > healReductionMagnitude)
			healReductionMagnitude = magnitude;
		break;
	case Protocol::CC_TYPE_BLIND:
		// 갱신 - 더 긴 쪽. 시야 배율은 규칙 하나뿐이라 들고 있지 않는다
		if (endUs > blindEndUs)
			blindEndUs = endUs;
		break;
	default:
		break;
	}
}

void Creature::RefreshActiveSlow(uint64 nowUs)
{
	// 만료된 소스를 앞으로 당기면서 동시에 최대값을 뽑는다. 합산X
	float maxMagnitude = 0.f;
	size_t writeIdx = 0;

	for (size_t readIdx = 0; readIdx < slowSources.size(); readIdx++)
	{
		if (slowSources[readIdx].endUs <= nowUs)
			continue;

		if (slowSources[readIdx].magnitude > maxMagnitude)
			maxMagnitude = slowSources[readIdx].magnitude;

		slowSources[writeIdx++] = slowSources[readIdx];

		activeSlow = maxMagnitude;
	}
	slowSources.resize(writeIdx);

	activeSlow = maxMagnitude;
}

bool Creature::IsHardCced(uint64 nowUs) const
{
	return nowUs < stunEndUs
		|| nowUs < rootEndUs
		|| nowUs < knockbackEndUs
		|| nowUs < launchEndUs
		|| nowUs < tauntEndUs;
}

bool Creature::CanMove(uint64 nowUs) const
{
	return IsHardCced(nowUs) == false;
}

bool Creature::CanTurn(uint64 nowUs) const
{
	// 도발은 시전자를 바라본다 - 서버가 yaw 를 돌린다
	return (nowUs >= stunEndUs) && (nowUs >= knockbackEndUs) && (nowUs >= launchEndUs) && (nowUs >= tauntEndUs);
}

bool Creature::CanAttack(uint64 nowUs) const
{
	// 도발 - 어떤 공격도 스킬도 못 한다 (사용자 결정 2026-10-10). CanUseSkill 은 이 위에 침묵을 더한다
	return (nowUs >= stunEndUs) && (nowUs >= knockbackEndUs) && (nowUs >= launchEndUs) && (nowUs >= tauntEndUs);
}

bool Creature::CanUseSkill(uint64 nowUs) const
{
	return CanAttack(nowUs) && (nowUs >= silenceEndUs);
}

SkillSlot* Creature::GetSlot(Protocol::EquipSlot slot)
{
	const int32 index = static_cast<int32>(slot) - 1;
	if (index < 0 || index >= SKILL_SLOT_COUNT)
		return nullptr;

	return &skillSlots[index];
}

const SkillSlot* Creature::GetSlot(Protocol::EquipSlot slot) const
{
	const int32 index = static_cast<int32>(slot) - 1;
	if (index < 0 || index >= SKILL_SLOT_COUNT)
		return nullptr;

	return &skillSlots[index];
}

void Creature::RefreshBindStates()
{
	// 배열 순서가 곧 우선순위다.
	// 무기(shift) > 무기(Q) > 투구 > 갑옷 > 신발 > 장신구

	unordered_map<uint32, Protocol::EquipSlot> seen;

	for (int32 i = 0; i < SKILL_SLOT_COUNT; i++)
	{
		SkillSlot& s = skillSlots[i];
		const Protocol::EquipSlot slotEnum = static_cast<Protocol::EquipSlot>(i + 1);

		if (s.skillId == 0)
		{
			s.bindState = Protocol::BIND_UNBOUND;
			s.shadowedBy = Protocol::SLOT_NONE;
			continue;
		}

		auto found = seen.find(s.skillId);
		if (found == seen.end())
		{
			s.bindState = Protocol::BIND_BOUND;
			s.shadowedBy = Protocol::SLOT_NONE;
			seen[s.skillId] = slotEnum;
		}
		else
		{
			s.bindState = Protocol::BIND_SHADOWED;
			s.shadowedBy = found->second;
		}
	}
}
