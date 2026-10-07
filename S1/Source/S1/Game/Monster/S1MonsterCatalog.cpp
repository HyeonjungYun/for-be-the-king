// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/Monster/S1MonsterCatalog.h"

#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	constexpr float PlayerRadiusCm = 42.f;		// S1Player capsule — ADR-0006 § ⑦
}

bool FS1MonsterCatalog::Parse(const FString& Csv, FString& OutError)
{
	Types.Reset();

	TArray<FString> Lines;
	Csv.ParseIntoArrayLines(Lines, /*InCullEmpty*/ true);
	if (Lines.Num() == 0)
	{
		OutError = TEXT("monster_types.csv is empty");
		return false;
	}

	TArray<FString> Header;
	Lines[0].TrimStartAndEnd().ParseIntoArray(Header, TEXT(","), /*InCullEmpty*/ false);
	const int32 IdCol = Header.IndexOfByKey(TEXT("monster_type_id"));
	const int32 NameCol = Header.IndexOfByKey(TEXT("name_key"));
	const int32 TierCol = Header.IndexOfByKey(TEXT("tier"));
	const int32 RadiusCol = Header.IndexOfByKey(TEXT("radius_cm"));
	const int32 HpCol = Header.IndexOfByKey(TEXT("max_hp"));

	if (IdCol == INDEX_NONE || NameCol == INDEX_NONE || TierCol == INDEX_NONE || RadiusCol == INDEX_NONE || HpCol == INDEX_NONE)
	{
		OutError = TEXT("monster_types.csv: header is missing monster_type_id, name_key, tier, radius_cm or max_hp");
		return false;
	}

	TMap<uint32, FS1MonsterTypeDef> Parsed;
	for (int32 Line = 1; Line < Lines.Num(); Line++)
	{
		TArray<FString> Cells;
		Lines[Line].TrimStartAndEnd().ParseIntoArray(Cells, TEXT(","), /*InCullEmpty*/ false);
		if (Cells.Num() != Header.Num())
		{
			OutError = FString::Printf(TEXT("monster_types.csv line %d: %d cells, header has %d"), Line + 1, Cells.Num(), Header.Num());
			return false;
		}

		FS1MonsterTypeDef Def;
		Def.Id = static_cast<uint32>(FCString::Atoi(*Cells[IdCol]));
		Def.NameKey = Cells[NameCol];
		Def.Tier = FName(*Cells[TierCol]);
		Def.RadiusCm = FCString::Atof(*Cells[RadiusCol]);
		Def.MaxHp = FCString::Atoi(*Cells[HpCol]);

		if (Def.Id == 0 || Def.NameKey.IsEmpty() || Def.RadiusCm <= 0.f || Def.MaxHp <= 0 || Parsed.Contains(Def.Id))
		{
			OutError = FString::Printf(TEXT("monster_types.csv line %d: id must be unique and > 0, name_key set, radius and max_hp > 0"), Line + 1);
			return false;
		}

		Parsed.Add(Def.Id, Def);
	}

	Types = MoveTemp(Parsed);
	return true;
}

bool FS1MonsterCatalog::LoadFromDirectory(const FString& Directory, FString& OutError)
{
	const FString Path = FPaths::Combine(Directory, TEXT("monster_types.csv"));
	FString Text;
	if (FFileHelper::LoadFileToString(Text, *Path) == false)
	{
		OutError = FString::Printf(TEXT("cannot read %s"), *Path);
		return false;
	}

	return Parse(Text, OutError);
}

const FS1MonsterCatalog& FS1MonsterCatalog::Get()
{
	static FS1MonsterCatalog Catalog;
	static bool bLoaded = false;

	if (bLoaded == false)
	{
		bLoaded = true;

		FString Relative;
		GConfig->GetString(TEXT("/Script/S1.S1ItemSubsystem"), TEXT("ItemDataDirectory"), Relative, GGameIni);
		const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), Relative));

		FString Error;
		if (Relative.IsEmpty() || Catalog.LoadFromDirectory(Directory, Error) == false)
		{
			UE_LOG(LogTemp, Warning, TEXT("[MON] monster catalog not loaded (%s) - monsters draw at player size"), Relative.IsEmpty() ? TEXT("ItemDataDirectory is not set") : *Error);
		}
		else
		{
			UE_LOG(LogTemp, Log, TEXT("[MON] monster catalog loaded - %d types from %s"), Catalog.Num(), *Directory);
		}
	}

	return Catalog;
}

float FS1MonsterCatalog::ScaleForRadius(float RadiusCm)
{
	return FMath::Clamp(RadiusCm / PlayerRadiusCm, 0.5f, 4.f);
}
