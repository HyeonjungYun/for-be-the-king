// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPath.h"

class UAnimSequenceBase;
class USkeleton;

/**
 * One row of S1/Content/Data/anim_clips.csv — a clip the character can play, by (key, weapon).
 * The clips come from the Fab animation packs (untracked, see .gitignore); only their paths live here.
 */
struct FS1AnimClip
{
	FString				Key;				// idle run walk hit death attack ... and the motion keys m_*
	FName				Weapon;				// sword hammer spear dagger bow knuckle staff monster | any
	FSoftObjectPath		Asset;				// /Game/... AnimSequence
	float				Rate = 1.f;
	bool				bLoop = false;		// idle / run / channel loops
	int32				HitPct = 40;		// where the strike lands in the clip (0..100 %) - the driver stretches the clip so this meets the server windup
	float				SpeedCms = 0.f;		// natural ground speed of a locomotion clip (optional speed_cms column, 0 = unknown) - played at actual speed / this
	int32				StartPct = 0;		// skill clips start here (optional start_pct column, 0..hit_pct) - skips a windup the skill does not have (기습's crouch before the lunge)
};

/**
 * 8-direction locomotion math (2026-10-15 — sword = CoolKatana V1 walk_<dir> / run_<dir>). Pure, so it is unit-tested (S1.Anim.Loco.*).
 * Sectors: 0 F · 1 FR · 2 R · 3 BR · 4 B · 5 BL · 6 L · 7 FL. RelYawDeg = movement yaw − facing yaw (UE yaw: positive = to the right).
 */
namespace S1LocoLogic
{
	/** The sector for a relative movement angle. Keeps PrevSector (0..7) until the angle is more than HysteresisDeg past its 22.5° edge, so a diagonal held on the line does not flicker. PrevSector < 0 = none yet. */
	S1_API int32 DirectionSector(float RelYawDeg, int32 PrevSector, float HysteresisDeg);

	/** "f" "fr" "r" "br" "b" "bl" "l" "fl" for sectors 0..7 (the anim_clips.csv key suffix). */
	S1_API const TCHAR* SectorSuffix(int32 Sector);

	/** Walk / run with hysteresis: a walker starts running at EnterRunCms, a runner drops to a walk below ExitRunCms. */
	S1_API bool IsRunGait(float SpeedCms, bool bWasRun, float EnterRunCms, float ExitRunCms);

	/** Where in the new loop to start so the step phase carries over (0..1 of the cycle). Position may be past one cycle (looping montage). */
	S1_API float LoopPhase(float Position, float Length);
}

/**
 * Animation catalog (2026-10-12 animation task — design/quick-specs/animation-2026-10-12.md).
 *
 * The player, every other player and every monster share one mannequin and one AnimBlueprint (ABP_Unarmed: locomotion + a
 * DefaultSlot). Everything weapon-specific is a clip from the packs played through that slot as a dynamic montage, chosen here:
 *   key + weapon  →  clip                  ("attack" + "sword" → Cool Sword V2 slash)
 *   motion id     →  motion key → clip     (S_SKILL_CAST.motion_id 7 → "m_dash" → the weapon's dodge clip)
 * Fallbacks: (key, weapon) → (key, any) → ("attack", weapon) → ("attack", any). A missing clip is logged once, never fatal.
 *
 * The packs reference their own skeleton copies. The first time a clip from a pack plays, its skeleton is registered as
 * compatible with the player's skeleton (USkeleton::AddCompatibleSkeleton) — no asset edit, works in -game and packaged.
 *
 * Data: Content/Data/anim_clips.csv (built by tools/anim/build_anim_clips.py), Server/Data/motions.csv (motion_id,key — the
 * server sends the id, both sides read the same file).
 */
class FS1AnimCatalog
{
public:
	/** Loads once per process from the project (Content/Data + the server data directory of [/Script/S1.S1ItemSubsystem] ItemDataDirectory). */
	static FS1AnimCatalog& Get();

	bool LoadFromFiles(const FString& ClipsCsvPath, const FString& MotionsCsvPath, FString& OutError);
	bool ParseClips(const FString& Csv, FString& OutError);
	bool ParseMotions(const FString& Csv, FString& OutError);

	/** Clip for a key and weapon with the fallback chain. Null only when the table has nothing at all for the key. */
	const FS1AnimClip* Find(const FString& Key, FName Weapon) const;

	/** (key, weapon) → (key, any) only - no "attack" fallback. For optional sets like walk_<dir> / run_<dir>: null = this weapon has none. */
	const FS1AnimClip* FindExact(const FString& Key, FName Weapon) const;

	/** Clip for a server motion id (motions.csv). 0 / unknown → null (no motion). */
	const FS1AnimClip* FindMotion(uint32 MotionId, FName Weapon) const;

	/** Loads the AnimSequence of a clip (cached) and makes its skeleton compatible with TargetSkeleton. Null if the asset is missing. */
	UAnimSequenceBase* Load(const FS1AnimClip& Clip, USkeleton* TargetSkeleton);

	/** "sword", "hammer", ... from an item_types.csv weapon id (archetype weapon_<x>); 0 or unknown → "any". */
	static FName WeaponKeyOf(uint32 ItemTypeId);

	int32 Num() const { return Clips.Num(); }
	const TMap<uint32, FString>& Motions() const { return MotionKeys; }
	const TMap<FString, FS1AnimClip>& All() const { return Clips; }
	bool IsLoaded() const { return bLoaded; }

	static FString MakeKey(const FString& Key, FName Weapon) { return Key + TEXT("|") + Weapon.ToString(); }

private:
	TMap<FString, FS1AnimClip>			Clips;				// "key|weapon" → clip
	TMap<uint32, FString>				MotionKeys;			// motion_id → key
	TMap<FString, TWeakObjectPtr<UAnimSequenceBase>>	LoadedAssets;
	TSet<FString>						CompatiblePairs;	// "target|source" skeleton pairs already registered
	TSet<FString>						MissingLogged;
	bool								bLoaded = false;
};
