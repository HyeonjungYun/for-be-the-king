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
	const float effectiveBase = attackPower * (isCrit ? critMultiplier : 1.f);

	return ApplyMitigation(effectiveBase, target, type);
}

int32 Creature::ComputeSkillDamageTo(const Creature& target, float adRatio, float apRatio, bool& isCrit) const
{
	isCrit = (critChance > 0.f) && (Utils::GetRandom(0.f, 1.f) < critChance);
	const float critMul = isCrit ? critMultiplier : 1.f;

	int32 total = 0;

	if (adRatio > 0.f)
		total += ApplyMitigation(attackPower * adRatio * critMul, target, Protocol::DAMAGE_TYPE_PHYSICAL);
	if (apRatio > 0.f)
		total += ApplyMitigation(magicPower * apRatio * critMul, target, Protocol::DAMAGE_TYPE_MAGIC);

	return total;
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
		|| nowUs < launchEndUs;
}

bool Creature::CanMove(uint64 nowUs) const
{
	return IsHardCced(nowUs) == false;
}

bool Creature::CanTurn(uint64 nowUs) const
{
	return (nowUs >= stunEndUs) && (nowUs >= knockbackEndUs) && (nowUs >= launchEndUs);
}

bool Creature::CanAttack(uint64 nowUs) const
{
	return (nowUs >= stunEndUs) && (nowUs >= knockbackEndUs) && (nowUs >= launchEndUs);
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
