// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/Item/S1Economy.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	/** "a,b,c" lines after the header. Empty lines skipped. */
	bool SplitCsv(const FString& Csv, TArray<FString>& OutHeader, TArray<TArray<FString>>& OutRows)
	{
		TArray<FString> Lines;
		Csv.ParseIntoArrayLines(Lines);
		if (Lines.Num() == 0)
			return false;
		Lines[0].TrimStartAndEnd().ParseIntoArray(OutHeader, TEXT(","), false);
		for (int32 i = 1; i < Lines.Num(); i++)
		{
			const FString Line = Lines[i].TrimStartAndEnd();
			if (Line.IsEmpty())
				continue;
			TArray<FString>& Row = OutRows.AddDefaulted_GetRef();
			Line.ParseIntoArray(Row, TEXT(","), false);
		}
		return true;
	}
}

bool FS1LootCatalog::Parse(const FString& Csv, FString& OutError)
{
	Types.Reset();
	TArray<FString> Header;
	TArray<TArray<FString>> Rows;
	if (SplitCsv(Csv, Header, Rows) == false)
	{
		OutError = TEXT("loot_types.csv is empty");
		return false;
	}

	const int32 CId = Header.IndexOfByKey(TEXT("id"));
	const int32 CName = Header.IndexOfByKey(TEXT("name_key"));
	const int32 CPrice = Header.IndexOfByKey(TEXT("sell_price"));
	const int32 CStack = Header.IndexOfByKey(TEXT("stack_max"));
	if (CId < 0 || CName < 0 || CPrice < 0 || CStack < 0)
	{
		OutError = TEXT("loot_types.csv needs id,name_key,sell_price,stack_max");
		return false;
	}

	for (const TArray<FString>& Row : Rows)
	{
		if (Row.Num() < Header.Num())
		{
			OutError = TEXT("loot_types.csv row is short");
			Types.Reset();
			return false;
		}
		FS1LootTypeDef T;
		T.Id = static_cast<uint32>(FCString::Atoi(*Row[CId]));
		T.NameKey = Row[CName].TrimStartAndEnd();
		T.SellPrice = static_cast<uint32>(FCString::Atoi(*Row[CPrice]));
		T.StackMax = FMath::Max(1, FCString::Atoi(*Row[CStack]));
		if (T.Id < 900 || T.Id > 999 || T.NameKey.IsEmpty() || Types.Contains(T.Id))
		{
			OutError = FString::Printf(TEXT("loot_types.csv bad row id=%u"), T.Id);
			Types.Reset();
			return false;
		}
		Types.Add(T.Id, T);
	}
	return true;
}

bool FS1LootCatalog::LoadFromDirectory(const FString& Directory, FString& OutError)
{
	FString Csv;
	if (FFileHelper::LoadFileToString(Csv, *FPaths::Combine(Directory, TEXT("loot_types.csv"))) == false)
	{
		Types.Reset();
		return true;		// no file - no loot (the server says the same)
	}
	return Parse(Csv, OutError);
}

bool FS1EconomyRules::Parse(const FString& Csv, FString& OutError)
{
	TArray<FString> Header;
	TArray<TArray<FString>> Rows;
	if (SplitCsv(Csv, Header, Rows) == false)
	{
		OutError = TEXT("economy.csv is empty");
		return false;
	}

	for (const TArray<FString>& Row : Rows)
	{
		if (Row.Num() < 2)
			continue;
		const FString Key = Row[0].TrimStartAndEnd();
		const FString Value = Row[1].TrimStartAndEnd();
		if (Key == TEXT("enhance_cost_base")) EnhanceCostBase = static_cast<uint32>(FCString::Atoi(*Value));
		else if (Key == TEXT("enhance_cost_growth")) EnhanceCostGrowth = FCString::Atof(*Value);
		else if (Key == TEXT("death_gold_loss_pct")) DeathGoldLossPct = static_cast<uint32>(FCString::Atoi(*Value));
		else if (Key == TEXT("sell_level_bonus")) SellLevelBonus = FCString::Atof(*Value);
		else if (Key.StartsWith(TEXT("sell_grade_")) && Key.Len() == 12 && Key[11] >= TEXT('1') && Key[11] <= TEXT('4'))
			SellGrade[Key[11] - TEXT('0')] = static_cast<uint32>(FCString::Atoi(*Value));
	}
	return true;
}

bool FS1EconomyRules::LoadFromDirectory(const FString& Directory, FString& OutError)
{
	*this = FS1EconomyRules();
	FString Csv;
	if (FFileHelper::LoadFileToString(Csv, *FPaths::Combine(Directory, TEXT("economy.csv"))) == false)
		return true;
	return Parse(Csv, OutError);
}

uint64 S1Economy::EnhanceCost(uint32 Level, const FS1EconomyRules& Rules)
{
	return static_cast<uint64>(FMath::RoundToDouble(static_cast<double>(Rules.EnhanceCostBase) * FMath::Pow(static_cast<double>(Rules.EnhanceCostGrowth), static_cast<double>(Level))));
}

uint64 S1Economy::SellPrice(const FS1ItemView& Item, const FS1LootCatalog& Loot, const FS1EconomyRules& Rules)
{
	if (const FS1LootTypeDef* Def = Loot.Find(Item.ItemTypeId))
		return static_cast<uint64>(Def->SellPrice) * FMath::Max<uint32>(Item.StackCount, 1);

	const int32 Grade = static_cast<int32>(Item.Grade);
	if (Grade < 1 || Grade > 4)
		return 0;
	return static_cast<uint64>(FMath::RoundToDouble(static_cast<double>(Rules.SellGrade[Grade]) * (1.0 + static_cast<double>(Rules.SellLevelBonus) * Item.Level)));
}
