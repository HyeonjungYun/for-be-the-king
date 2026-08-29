// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

/**
 * Client-side mirror of the server's skill definitions.
 *
 * 🔴 THIS MUST MATCH Server/GameServer/SkillTable.cpp EXACTLY.
 *
 * The whole information-asymmetry design depends on it: the server sends skill_id only to
 * its owner and never tells anyone the cast time, range or cooldown
 * (design/gdd/equipment-skill-binding.md B8). The owner looks those up here instead. If the
 * two tables drift, the client draws a range circle that the server does not honour and the
 * player is told a lie about where their skill reaches.
 *
 * TODO: both sides should read one shared data file. Two hand-maintained copies is the
 * cheapest thing that works today and the first thing that will rot.
 */

/** Hit shape. The server decides targets with this; the client only displays it. */
enum class ES1SkillShape : uint8
{
	Single = 0,
	Line,
	CircleSelf,
	CirclePoint,
};

/**
 * How the skill is aimed. Independent of Shape — 짓쳐들기 hits nothing (CircleSelf with
 * radius 0) yet still needs a direction. skill-system.md R12b.
 */
enum class ES1SkillAimType : uint8
{
	// 🔴 Self-buffs only — a skill with no radius and no target, so there is genuinely
	// nothing to show. Anything with a hit radius uses SelfArea instead: not drawing the
	// radius means asking the player to guess it. skill-system.md R12b.
	None = 0,

	SelfArea,		// Aims, but ignores the cursor. Shows the hit radius around the caster
	Target,			// Lock whatever is under the cursor
	Direction,		// Cursor direction
	Point,			// Cursor position
};

struct FS1SkillDef
{
	uint32				SkillId = 0;
	uint32				EffectId = 0;

	uint32				CastMs = 0;
	uint32				CooldownMs = 0;
	bool				bCanMoveWhileCasting = false;

	ES1SkillShape		Shape = ES1SkillShape::Single;
	ES1SkillAimType		AimType = ES1SkillAimType::None;

	float				RangeCm = 0.f;
	float				WidthCm = 0.f;
	float				RadiusCm = 0.f;

	/**
	 * Dash travel, when the skill has a movement effect. Zero means it does not move you.
	 *
	 * Mirrors the server's movement effect (dist_cm / speed_cms) rather than the whole
	 * effect list — the client executes the displacement itself and needs nothing else
	 * from it. Damage and CC numbers stay server-side where they belong.
	 */
	float				MoveDistCm = 0.f;
	float				MoveSpeedCms = 0.f;

	bool HasMovement() const { return MoveDistCm > 0.f && MoveSpeedCms > 0.f; }

	/** True when pressing the key should open the aiming mode rather than fire immediately. */
	bool NeedsAiming() const { return AimType != ES1SkillAimType::None; }
};

class FS1SkillTable
{
public:
	/** Null when the id is unknown — treat that as "cannot aim, cannot fire". */
	static const FS1SkillDef* Find(uint32 SkillId);
};
