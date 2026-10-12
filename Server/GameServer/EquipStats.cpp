#include "pch.h"
#include "EquipStats.h"

namespace
{
	int32 RoundStat(float value)
	{
		return static_cast<int32>(std::lround(value));
	}
}

EquipStatTotals EquipStats::Compute(const vector<WornItem>& worn, int32 nakedMaxHp)
{
	float sums[static_cast<int32>(ItemStat::Count)] = {};
	float weaponRange = 0.f;
	float weaponSpeed = 1.f;
	bool weaponRanged = false;
	bool magicWeapon = false;

	for (const WornItem& item : worn)
	{
		const ItemTypeDef* def = ItemTable::Find(item.typeId);
		if (def == nullptr)
			continue;

		const float gradeMultiplier = ItemTable::GradeMultiplier(item.grade);

		for (const ItemStatLine& line : def->stats)
		{
			float value = line.flatFloor + line.baseStat * gradeMultiplier + ItemLevel::Bonus(line.baseStat, item.level);		// D2 레벨 항 (2026-10-12)

			if (line.stat == ItemStat::PercentAsBonus)
				value = min(value, AS_BONUS_PER_ITEM_MAX);
			else if (line.stat == ItemStat::PercentMsBonus)
				value = min(value, MS_BONUS_PER_ITEM_MAX);

			sums[static_cast<int32>(line.stat)] += value;
		}

		if (def->equipSlot == Protocol::SLOT_WEAPON_PRIMARY)
		{
			weaponRange = def->attackRangeCm;
			weaponSpeed = def->attackSpeed;
			weaponRanged = def->rangedAttack;
			magicWeapon = (def->damage == ItemDamage::Magic);
		}
	}

	auto sum = [&sums](ItemStat stat) { return sums[static_cast<int32>(stat)]; };

	EquipStatTotals out;
	out.maxHp			= nakedMaxHp + RoundStat(sum(ItemStat::MaxHp));
	out.attackPower		= BaseStats::ATTACK_POWER + RoundStat(sum(ItemStat::AttackPower));

	// 마법 무기를 벗으면 오프셋도 사라진다 - 무기 없는 magic_power 는 0 (Edge Cases)
	out.magicPower		= BaseStats::MAGIC_POWER + RoundStat((magicWeapon ? MAGIC_WEAPON_OFFSET : 0.f) + sum(ItemStat::MagicPower));

	out.armor			= BaseStats::ARMOR + RoundStat(sum(ItemStat::Armor));
	out.magicResist		= BaseStats::MAGIC_RESIST + RoundStat(sum(ItemStat::MagicResist));
	out.critChance		= std::clamp(BaseStats::CRIT_CHANCE + sum(ItemStat::CritChance), 0.f, CRIT_CHANCE_MAX);
	out.critMultiplier	= std::clamp(BaseStats::CRIT_MULITPLIER + sum(ItemStat::CritMultiplierBonus), CRIT_MULTIPLIER_MIN, CRIT_MULTIPLIER_MAX);
	out.percentAsBonus	= sum(ItemStat::PercentAsBonus);
	out.percentMsBonus	= sum(ItemStat::PercentMsBonus);
	out.resistance		= std::clamp(BaseStats::RESISTANCE + sum(ItemStat::Resistance), 0.f, RESISTANCE_MAX);
	out.attackRange		= max(BaseStats::ATTACK_RANGE, weaponRange);
	out.attackSpeed		= BaseStats::ATTACK_SPEED * weaponSpeed;
	out.rangedAttack	= weaponRanged;
	out.autoAttackType	= magicWeapon ? Protocol::DAMAGE_TYPE_MAGIC : Protocol::DAMAGE_TYPE_PHYSICAL;

	return out;
}

int32 EquipStats::RescaleHp(int32 hp, int32 oldMaxHp, int32 newMaxHp)
{
	if (hp <= 0 || newMaxHp <= 0)
		return hp <= 0 ? hp : 1;

	if (oldMaxHp <= 0 || oldMaxHp == newMaxHp)
		return min(hp, newMaxHp);

	const int64 scaled = static_cast<int64>(hp) * newMaxHp / oldMaxHp;
	return static_cast<int32>(std::clamp<int64>(scaled, 1, newMaxHp));
}
