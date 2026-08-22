#include "pch.h"
#include "SkillTable.h"

unordered_map<uint32, SkillDef> SkillTable::s_defs;

bool SkillDef::HasHardCc() const
{
	for (const SkillEffect& e : effects)
	{
		if (e.type == SkillEffectType::Knockback)
			return true;

		if (e.type != SkillEffectType::HardCc)
			continue;

		if (e.ccType == Protocol::CC_TYPE_STUN
			|| e.ccType == Protocol::CC_TYPE_KNOCKBACK
			|| e.ccType == Protocol::CC_TYPE_LAUNCH
			|| e.ccType == Protocol::CC_TYPE_ROOT)
			return true;
	}

	return false;
}

bool SkillDef::HasMovement() const
{
	for (const SkillEffect& e : effects)
	{
		if (e.type == SkillEffectType::Movement)
			return true;
	}

	return false;
}

void SkillTable::Init()
{
	s_defs.clear();

	{
		SkillDef def;
		def.skillId = 1001;
		def.effectId = 1;
		def.castMs = 300;
		def.cooldownMs = 8000;
		def.canMoveWhileCasting = false;
		def.shape = SkillShape::Single;
		def.aimType = SkillAimType::Target;
		def.rangeCm = 400.f;

		SkillEffect dmg;
		dmg.type = SkillEffectType::Damage;
		dmg.adRatio = 2.0f;
		dmg.damageType = Protocol::DAMAGE_TYPE_PHYSICAL;
		def.effects.push_back(dmg);

		SkillEffect stun;
		stun.type = SkillEffectType::HardCc;
		stun.ccType = Protocol::CC_TYPE_STUN;
		stun.ccDurationMs = 1000;
		def.effects.push_back(stun);

		s_defs[def.skillId] = def;
	}

	{
		SkillDef def;
		def.skillId = 2001;
		def.effectId = 2;
		def.castMs = 60;
		def.cooldownMs = 6000;
		def.canMoveWhileCasting = false;
		def.shape = SkillShape::CircleSelf;
		def.aimType = SkillAimType::Direction;
		def.radiusCm = 0.f;

		SkillEffect move;
		move.type = SkillEffectType::Movement;
		move.distCm = 400.f;
		move.speedCms = 1200.f;
		move.ignoresWalls = false;
		def.effects.push_back(move);

		s_defs[def.skillId] = def;
	}
}

const SkillDef* SkillTable::Find(uint32 skillId)
{
	auto it = s_defs.find(skillId);
	if (it == s_defs.end())
		return nullptr;

	return &it->second;
}
