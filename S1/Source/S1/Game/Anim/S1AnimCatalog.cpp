// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/Anim/S1AnimCatalog.h"
#include "Game/Item/S1ItemSubsystem.h"
#include "Game/Item/S1ItemCatalog.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/Skeleton.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ConfigCacheIni.h"

namespace
{
	// Minimal CSV split — the tables have no quoted commas (asset paths never contain one)
	void SplitRow(const FString& Line, TArray<FString>& Out)
	{
		Out.Reset();
		Line.ParseIntoArray(Out, TEXT(","), false);
		for (FString& Cell : Out)
			Cell.TrimStartAndEndInline();
	}

	bool ReadLines(const FString& Csv, TArray<FString>& OutLines)
	{
		Csv.ParseIntoArrayLines(OutLines, true);
		return OutLines.Num() > 0;
	}
}

FS1AnimCatalog& FS1AnimCatalog::Get()
{
	static FS1AnimCatalog Catalog;
	if (Catalog.bLoaded)
		return Catalog;
	Catalog.bLoaded = true;		// one attempt per process - a broken table logs once, the game keeps the template locomotion

	FString Relative;
	GConfig->GetString(TEXT("/Script/S1.S1ItemSubsystem"), TEXT("ItemDataDirectory"), Relative, GGameIni);
	const FString ServerData = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), Relative));
	const FString Clips = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Data"), TEXT("anim_clips.csv"));
	const FString Motions = FPaths::Combine(ServerData, TEXT("motions.csv"));

	FString Error;
	if (Relative.IsEmpty() || Catalog.LoadFromFiles(Clips, Motions, Error) == false)
	{
		UE_LOG(LogTemp, Error, TEXT("[ANIM] catalog not loaded - characters keep the unarmed template animations. %s"),
			Relative.IsEmpty() ? TEXT("ItemDataDirectory is not set") : *Error);
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("[ANIM] catalog loaded - %d clips, %d motions"), Catalog.Num(), Catalog.MotionKeys.Num());
	}
	return Catalog;
}

bool FS1AnimCatalog::LoadFromFiles(const FString& ClipsCsvPath, const FString& MotionsCsvPath, FString& OutError)
{
	FString ClipsCsv, MotionsCsv;
	if (FFileHelper::LoadFileToString(ClipsCsv, *ClipsCsvPath) == false)
	{
		OutError = FString::Printf(TEXT("cannot read %s"), *ClipsCsvPath);
		return false;
	}
	if (FFileHelper::LoadFileToString(MotionsCsv, *MotionsCsvPath) == false)
	{
		OutError = FString::Printf(TEXT("cannot read %s"), *MotionsCsvPath);
		return false;
	}
	return ParseClips(ClipsCsv, OutError) && ParseMotions(MotionsCsv, OutError);
}

bool FS1AnimCatalog::ParseClips(const FString& Csv, FString& OutError)
{
	Clips.Reset();
	TArray<FString> Lines;
	if (ReadLines(Csv, Lines) == false)
	{
		OutError = TEXT("anim_clips.csv is empty");
		return false;
	}

	TArray<FString> Header;
	SplitRow(Lines[0], Header);
	const int32 KeyCol = Header.IndexOfByKey(TEXT("key"));
	const int32 WeaponCol = Header.IndexOfByKey(TEXT("weapon"));
	const int32 AssetCol = Header.IndexOfByKey(TEXT("asset"));
	const int32 RateCol = Header.IndexOfByKey(TEXT("rate"));
	const int32 LoopCol = Header.IndexOfByKey(TEXT("loop"));
	const int32 HitCol = Header.IndexOfByKey(TEXT("hit_pct"));
	const int32 SpeedCol = Header.IndexOfByKey(TEXT("speed_cms"));		// optional (2026-10-15 locomotion)
	const int32 StartCol = Header.IndexOfByKey(TEXT("start_pct"));		// optional (2026-10-15 per-skill clips)
	if (KeyCol < 0 || WeaponCol < 0 || AssetCol < 0 || RateCol < 0 || LoopCol < 0 || HitCol < 0)
	{
		OutError = TEXT("anim_clips.csv needs key,weapon,asset,rate,loop,hit_pct");
		return false;
	}

	TArray<FString> Cells;
	for (int32 i = 1; i < Lines.Num(); i++)
	{
		SplitRow(Lines[i], Cells);
		if (Cells.Num() <= HitCol)
		{
			OutError = FString::Printf(TEXT("anim_clips.csv line %d: too few columns"), i + 1);
			return false;
		}
		FS1AnimClip Clip;
		Clip.Key = Cells[KeyCol];
		Clip.Weapon = FName(*Cells[WeaponCol]);
		Clip.Asset = FSoftObjectPath(Cells[AssetCol]);
		Clip.Rate = FCString::Atof(*Cells[RateCol]);
		Clip.bLoop = Cells[LoopCol] == TEXT("1");
		Clip.HitPct = FCString::Atoi(*Cells[HitCol]);
		if (SpeedCol >= 0 && Cells.IsValidIndex(SpeedCol) && Cells[SpeedCol].IsEmpty() == false)
			Clip.SpeedCms = FCString::Atof(*Cells[SpeedCol]);
		if (StartCol >= 0 && Cells.IsValidIndex(StartCol) && Cells[StartCol].IsEmpty() == false)
			Clip.StartPct = FCString::Atoi(*Cells[StartCol]);
		if (Clip.Key.IsEmpty() || Clip.Weapon.IsNone() || Clip.Asset.IsNull() || Clip.Rate <= 0.f || Clip.HitPct < 0 || Clip.HitPct > 100 || Clip.SpeedCms < 0.f
			|| Clip.StartPct < 0 || Clip.StartPct > Clip.HitPct)
		{
			OutError = FString::Printf(TEXT("anim_clips.csv line %d: key, weapon, asset, rate > 0, hit_pct 0..100, speed_cms >= 0, start_pct 0..hit_pct"), i + 1);
			return false;
		}
		const FString MapKey = MakeKey(Clip.Key, Clip.Weapon);
		if (Clips.Contains(MapKey))
		{
			OutError = FString::Printf(TEXT("anim_clips.csv line %d: duplicate %s"), i + 1, *MapKey);
			return false;
		}
		Clips.Add(MapKey, Clip);
	}
	return true;
}

bool FS1AnimCatalog::ParseMotions(const FString& Csv, FString& OutError)
{
	MotionKeys.Reset();
	TArray<FString> Lines;
	if (ReadLines(Csv, Lines) == false)
	{
		OutError = TEXT("motions.csv is empty");
		return false;
	}
	TArray<FString> Header, Cells;
	SplitRow(Lines[0], Header);
	const int32 IdCol = Header.IndexOfByKey(TEXT("motion_id"));
	const int32 KeyCol = Header.IndexOfByKey(TEXT("key"));
	if (IdCol < 0 || KeyCol < 0)
	{
		OutError = TEXT("motions.csv needs motion_id,key");
		return false;
	}
	for (int32 i = 1; i < Lines.Num(); i++)
	{
		SplitRow(Lines[i], Cells);
		if (Cells.Num() <= FMath::Max(IdCol, KeyCol))
			continue;
		const uint32 Id = static_cast<uint32>(FCString::Atoi(*Cells[IdCol]));
		if (Id == 0 || Cells[KeyCol].IsEmpty() || MotionKeys.Contains(Id))
		{
			OutError = FString::Printf(TEXT("motions.csv line %d: motion_id > 0, key, no duplicates"), i + 1);
			return false;
		}
		MotionKeys.Add(Id, Cells[KeyCol]);
	}
	return true;
}

const FS1AnimClip* FS1AnimCatalog::Find(const FString& Key, FName Weapon) const
{
	static const FName AnyWeapon(TEXT("any"));
	if (const FS1AnimClip* Clip = Clips.Find(MakeKey(Key, Weapon)))
		return Clip;
	if (const FS1AnimClip* Clip = Clips.Find(MakeKey(Key, AnyWeapon)))
		return Clip;
	if (Key != TEXT("attack"))
	{
		if (const FS1AnimClip* Clip = Clips.Find(MakeKey(TEXT("attack"), Weapon)))
			return Clip;
		if (const FS1AnimClip* Clip = Clips.Find(MakeKey(TEXT("attack"), AnyWeapon)))
			return Clip;
	}
	return nullptr;
}

const FS1AnimClip* FS1AnimCatalog::FindExact(const FString& Key, FName Weapon) const
{
	static const FName AnyWeapon(TEXT("any"));
	if (const FS1AnimClip* Clip = Clips.Find(MakeKey(Key, Weapon)))
		return Clip;
	return Clips.Find(MakeKey(Key, AnyWeapon));
}

int32 S1LocoLogic::DirectionSector(float RelYawDeg, int32 PrevSector, float HysteresisDeg)
{
	const float Angle = FRotator::NormalizeAxis(RelYawDeg);		// (-180, 180]
	if (PrevSector >= 0 && PrevSector < 8)
	{
		const float Center = PrevSector <= 4 ? PrevSector * 45.f : (PrevSector - 8) * 45.f;
		if (FMath::Abs(FRotator::NormalizeAxis(Angle - Center)) <= 22.5f + HysteresisDeg)
			return PrevSector;
	}
	int32 Sector = FMath::RoundToInt(Angle / 45.f);				// -4 .. 4
	if (Sector < 0)
		Sector += 8;
	return Sector % 8;
}

const TCHAR* S1LocoLogic::SectorSuffix(int32 Sector)
{
	static const TCHAR* Suffix[] = { TEXT("f"), TEXT("fr"), TEXT("r"), TEXT("br"), TEXT("b"), TEXT("bl"), TEXT("l"), TEXT("fl") };
	return Suffix[((Sector % 8) + 8) % 8];
}

bool S1LocoLogic::IsRunGait(float SpeedCms, bool bWasRun, float EnterRunCms, float ExitRunCms)
{
	return bWasRun ? SpeedCms >= ExitRunCms : SpeedCms >= EnterRunCms;
}

float S1LocoLogic::LoopPhase(float Position, float Length)
{
	if (Length <= UE_KINDA_SMALL_NUMBER || Position < 0.f)
		return 0.f;
	return FMath::Fmod(Position, Length) / Length;
}

const FS1AnimClip* FS1AnimCatalog::FindMotion(uint32 MotionId, FName Weapon) const
{
	const FString* Key = MotionKeys.Find(MotionId);
	if (Key == nullptr || *Key == TEXT("m_none"))
		return nullptr;
	return Find(*Key, Weapon);
}

UAnimSequenceBase* FS1AnimCatalog::Load(const FS1AnimClip& Clip, USkeleton* TargetSkeleton)
{
	const FString PathKey = Clip.Asset.ToString();
	UAnimSequenceBase* Sequence = nullptr;
	if (const TWeakObjectPtr<UAnimSequenceBase>* Cached = LoadedAssets.Find(PathKey))
		Sequence = Cached->Get();
	if (Sequence == nullptr)
	{
		Sequence = Cast<UAnimSequenceBase>(Clip.Asset.TryLoad());
		if (Sequence == nullptr)
		{
			if (MissingLogged.Contains(PathKey) == false)
			{
				MissingLogged.Add(PathKey);
				UE_LOG(LogTemp, Warning, TEXT("[ANIM] clip missing %s (%s/%s) - the Fab pack is not in this project"), *PathKey, *Clip.Key, *Clip.Weapon.ToString());
			}
			return nullptr;
		}
		LoadedAssets.Add(PathKey, Sequence);
	}

	// The pack's own skeleton copy → compatible with ours, once per pair. AddCompatibleSkeleton refreshes the bone remapping
	if (TargetSkeleton != nullptr && Sequence->GetSkeleton() != nullptr && Sequence->GetSkeleton() != TargetSkeleton)
	{
		const FString Pair = TargetSkeleton->GetPathName() + TEXT("|") + Sequence->GetSkeleton()->GetPathName();
		if (CompatiblePairs.Contains(Pair) == false)
		{
			CompatiblePairs.Add(Pair);
			TargetSkeleton->AddCompatibleSkeleton(Sequence->GetSkeleton());
			UE_LOG(LogTemp, Log, TEXT("[ANIM] skeleton %s now plays clips of %s"), *TargetSkeleton->GetName(), *Sequence->GetSkeleton()->GetPathName());
		}
	}
	return Sequence;
}

FName FS1AnimCatalog::WeaponKeyOf(uint32 ItemTypeId)
{
	static const FName AnyWeapon(TEXT("any"));
	if (ItemTypeId == 0)
		return AnyWeapon;
	const US1ItemSubsystem* Items = US1ItemSubsystem::Get();
	const FS1ItemTypeDef* Type = Items != nullptr ? Items->GetCatalog().Find(ItemTypeId) : nullptr;
	if (Type == nullptr || Type->IsWeapon() == false)
		return AnyWeapon;
	FString Arch = Type->Archetype.ToString();		// weapon_sword → sword
	if (Arch.RemoveFromStart(TEXT("weapon_")) == false)
		return AnyWeapon;
	return FName(*Arch);
}
