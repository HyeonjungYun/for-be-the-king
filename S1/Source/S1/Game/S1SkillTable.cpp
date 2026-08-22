// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/S1SkillTable.h"

namespace
{
	/**
	 * 🔴 Mirror of Server/GameServer/SkillTable.cpp — SkillTable::Init().
	 *    Every value here has a twin over there. Change one, change both.
	 *
	 *    These two skills are scaffolding, not content. skill-system.md Q6 (which 2~3 skills
	 *    P1.5 actually ships) is still open; when it closes this whole block is replaced.
	 */
	TMap<uint32, FS1SkillDef> BuildDefs()
	{
		TMap<uint32, FS1SkillDef> Defs;

		// [1001] 후려치기 — weapon, hard CC (stun) + physical damage
		{
			FS1SkillDef Def;
			Def.SkillId = 1001;
			Def.EffectId = 1;
			Def.CastMs = 300;
			Def.CooldownMs = 8000;
			Def.bCanMoveWhileCasting = false;
			Def.Shape = ES1SkillShape::Single;
			Def.AimType = ES1SkillAimType::Target;
			Def.RangeCm = 400.f;

			Defs.Add(Def.SkillId, Def);
		}

		// [2001] 짓쳐들기 — boots, movement
		{
			FS1SkillDef Def;
			Def.SkillId = 2001;
			Def.EffectId = 2;
			Def.CastMs = 60;
			Def.CooldownMs = 6000;
			Def.bCanMoveWhileCasting = false;

			// Hits nobody (radius 0) but still has to be pointed somewhere. This is the
			// skill that forced Shape and AimType apart — skill-system.md R12b.
			Def.Shape = ES1SkillShape::CircleSelf;
			Def.AimType = ES1SkillAimType::Direction;
			Def.RadiusCm = 0.f;

			// Not in the server table: the dash distance the client needs to draw the
			// aiming line. Kept in sync with the movement effect's dist_cm over there.
			Def.RangeCm = 400.f;

			Defs.Add(Def.SkillId, Def);
		}

		return Defs;
	}
}

const FS1SkillDef* FS1SkillTable::Find(uint32 SkillId)
{
	// Built once on first use. No explicit Init() to forget to call, and no dependency on
	// GameInstance startup order — the table is static data with no owner.
	static const TMap<uint32, FS1SkillDef> Defs = BuildDefs();

	return Defs.Find(SkillId);
}
