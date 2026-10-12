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
};

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
