// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

/**
 * Client-side reading of the server's skill definitions — Server/Data/skills.csv + skill_effects.csv.
 *
 * 🔴 The same three files the server's SkillTable loads. The whole information-asymmetry design
 * depends on the two sides agreeing: the server sends skill_id only to its owner and never tells
 * anyone the cast time, range or cooldown (design/gdd/equipment-skill-binding.md B8). The owner
 * looks those up here. If the server's file changes, this reads the change on the next launch —
 * there is no second copy to forget (2026-10-10, replaced the hand-written mirror).
 *
 * Only what the client needs is kept: aiming, cast/cooldown, the dash it executes locally, and
 * enough shape to draw the hit. Damage and CC numbers stay server-side.
 */

enum class ES1SkillKind : uint8
{
	Active = 0,
	Passive,		// Holds a key, does nothing when pressed
	Hold,			// Press to start, press again to stop
	Recast,			// Press again within the recast window for the second part
};

/** Hit shape. The server decides targets with this; the client only displays it. */
enum class ES1SkillShape : uint8
{
	None = 0,
	Single,
	Line,
	Cone,
	CircleSelf,
	CirclePoint,
	Projectile,
};

/**
 * How the skill is aimed. Independent of Shape — 질주 hits nothing (CircleSelf with
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

enum class ES1SkillPower : uint8
{
	None = 0,
	Ad,
	Ap,
	Hybrid,
};

struct FS1SkillDef
{
	uint32				SkillId = 0;
	uint32				EffectId = 0;
	ES1SkillPower		Power = ES1SkillPower::None;
	ES1SkillKind		Kind = ES1SkillKind::Active;

	uint32				CastMs = 0;
	uint32				CooldownMs = 0;
	bool				bCanMoveWhileCasting = false;

	ES1SkillShape		Shape = ES1SkillShape::None;
	ES1SkillAimType		AimType = ES1SkillAimType::None;

	float				RangeCm = 0.f;
	float				WidthCm = 0.f;
	float				RadiusCm = 0.f;
	float				AngleDeg = 0.f;
	float				ProjectileSpeedCms = 0.f;
	bool				bPierce = false;

	/**
	 * Dash travel, when the skill has a movement effect. Zero means it does not move you.
	 *
	 * The client executes the displacement itself and needs nothing else from the effect
	 * list — damage and CC numbers stay server-side where they belong.
	 */
	float				MoveDistCm = 0.f;
	float				MoveSpeedCms = 0.f;
	bool				bMoveIgnoresWalls = false;
	FName				MoveKind;			// dash · leap · back · forward · blink

	/** The recast's second-press dash (requires=recast). Zero when the second press does not move you (stage 1, 2026-10-10). */
	float				RecastMoveDistCm = 0.f;
	float				RecastMoveSpeedCms = 0.f;
	FName				RecastMoveKind;

	bool				bHasHardCc = false;	// For the detail pane — "this key stuns" is part of how a key is used

	bool HasMovement() const { return MoveDistCm > 0.f && (MoveSpeedCms > 0.f || MoveKind == TEXT("blink")); }

	/** True when pressing the key should open the aiming mode rather than fire immediately. */
	bool NeedsAiming() const { return AimType != ES1SkillAimType::None; }
};

class FS1SkillTable
{
public:
	/**
	 * Reads skills.csv and skill_effects.csv from Directory (the server's Server/Data). On the first
	 * bad line nothing is kept and OutError names the file and line. Until this succeeds Find()
	 * returns null for everything — "cannot aim, cannot fire".
	 */
	static bool LoadFromDirectory(const FString& Directory, FString& OutError);

	/** Parses the two CSV texts. Columns are found by header name. */
	static bool Parse(const FString& SkillsCsv, const FString& EffectsCsv, FString& OutError);

	/** skill_rules.csv — only the keys the client acts on (recast window). Unknown keys are ignored. */
	static bool ParseRules(const FString& RulesCsv, FString& OutError);

	/** Null when the id is unknown — treat that as "cannot aim, cannot fire". */
	static const FS1SkillDef* Find(uint32 SkillId);

	static int32 Num();

	/** How long after a recast skill fires the same key fires its second part (skill_rules recast_window_ms). 0 until rules load. */
	static uint32 RecastWindowMs();

private:
	static TMap<uint32, FS1SkillDef>& Defs();
	static uint32& RecastWindow();
};
