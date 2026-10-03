// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/Item/S1ItemCatalog.h"
#include "Misc/DefaultValueHelper.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	/** A CSV split into cells. No quoting — the data files never need commas inside a value. */
	struct FCsv
	{
		TArray<FString>			Header;
		TArray<TArray<FString>>	Rows;
		TArray<int32>			LineNumbers;	// 1-based, for error messages

		int32 Column(const TCHAR* Name) const { return Header.IndexOfByKey(FString(Name)); }
	};

	void SplitCells(const FString& Line, TArray<FString>& OutCells)
	{
		Line.ParseIntoArray(OutCells, TEXT(","), /*InCullEmpty*/ false);
		for (FString& Cell : OutCells)
			Cell.TrimStartAndEndInline();
	}

	FCsv ReadCsv(const FString& Text)
	{
		FCsv Out;

		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, /*InCullEmpty*/ false);

		for (int32 i = 0; i < Lines.Num(); i++)
		{
			if (Lines[i].TrimStartAndEnd().IsEmpty())
				continue;

			TArray<FString> Cells;
			SplitCells(Lines[i], Cells);

			if (Out.Header.Num() == 0)
			{
				Out.Header = MoveTemp(Cells);
				continue;
			}

			Out.Rows.Add(MoveTemp(Cells));
			Out.LineNumbers.Add(i + 1);
		}

		return Out;
	}

	bool IsItemSlot(int32 Slot)
	{
		// SLOT_WEAPON_SECONDARY (2) is a key, not a place to wear an item.
		return Slot == 1 || (Slot >= 3 && Slot <= 6);
	}
}

bool FS1ItemCatalog::Parse(const FString& TypesCsv, const FString& StatsCsv, FString& OutError)
{
	TMap<uint32, FS1ItemTypeDef> Parsed;

	// ── item_types.csv ──
	{
		const FCsv Csv = ReadCsv(TypesCsv);

		const int32 ColId		= Csv.Column(TEXT("id"));
		const int32 ColName		= Csv.Column(TEXT("name_key"));
		const int32 ColSlot		= Csv.Column(TEXT("equip_slot"));
		const int32 ColArch		= Csv.Column(TEXT("archetype"));
		const int32 ColDamage	= Csv.Column(TEXT("damage_type"));
		const int32 ColRange	= Csv.Column(TEXT("attack_range_cm"));
		const int32 ColPool		= Csv.Column(TEXT("skill_pool_id"));

		for (int32 Col : { ColId, ColName, ColSlot, ColArch, ColDamage, ColRange, ColPool })
		{
			if (Col == INDEX_NONE)
			{
				OutError = TEXT("item_types.csv: header is missing a required column");
				return false;
			}
		}

		for (int32 r = 0; r < Csv.Rows.Num(); r++)
		{
			const TArray<FString>& Cells = Csv.Rows[r];
			const int32 Line = Csv.LineNumbers[r];

			if (Cells.Num() < Csv.Header.Num())
			{
				OutError = FString::Printf(TEXT("item_types.csv line %d: %d cells, header has %d"), Line, Cells.Num(), Csv.Header.Num());
				return false;
			}

			int32 Id = 0;
			int32 Slot = 0;
			float Range = 0.f;

			if (FDefaultValueHelper::ParseInt(Cells[ColId], Id) == false || Id <= 0)
			{
				OutError = FString::Printf(TEXT("item_types.csv line %d: id '%s' is not a positive integer"), Line, *Cells[ColId]);
				return false;
			}
			if (Parsed.Contains(static_cast<uint32>(Id)))
			{
				OutError = FString::Printf(TEXT("item_types.csv line %d: id %d appears twice"), Line, Id);
				return false;
			}
			if (FDefaultValueHelper::ParseInt(Cells[ColSlot], Slot) == false || IsItemSlot(Slot) == false)
			{
				OutError = FString::Printf(TEXT("item_types.csv line %d: equip_slot '%s' is not 1, 3, 4, 5 or 6"), Line, *Cells[ColSlot]);
				return false;
			}
			if (FDefaultValueHelper::ParseFloat(Cells[ColRange], Range) == false || Range < 0.f)
			{
				OutError = FString::Printf(TEXT("item_types.csv line %d: attack_range_cm '%s' is not a number >= 0"), Line, *Cells[ColRange]);
				return false;
			}
			if (Cells[ColName].IsEmpty())
			{
				OutError = FString::Printf(TEXT("item_types.csv line %d: name_key is empty"), Line);
				return false;
			}

			FS1ItemTypeDef Def;
			Def.Id				= static_cast<uint32>(Id);
			Def.NameKey			= Cells[ColName];
			Def.EquipSlot		= Slot;
			Def.Archetype		= FName(*Cells[ColArch]);
			Def.DamageType		= FName(*Cells[ColDamage]);
			Def.AttackRangeCm	= Range;
			Def.SkillPoolId		= FName(*Cells[ColPool]);

			Parsed.Add(Def.Id, MoveTemp(Def));
		}
	}

	// ── item_type_stats.csv ──
	{
		const FCsv Csv = ReadCsv(StatsCsv);

		const int32 ColId		= Csv.Column(TEXT("id"));
		const int32 ColStat		= Csv.Column(TEXT("stat"));
		const int32 ColBase		= Csv.Column(TEXT("base_stat"));
		const int32 ColFloor	= Csv.Column(TEXT("flat_floor"));

		for (int32 Col : { ColId, ColStat, ColBase, ColFloor })
		{
			if (Col == INDEX_NONE)
			{
				OutError = TEXT("item_type_stats.csv: header is missing a required column");
				return false;
			}
		}

		for (int32 r = 0; r < Csv.Rows.Num(); r++)
		{
			const TArray<FString>& Cells = Csv.Rows[r];
			const int32 Line = Csv.LineNumbers[r];

			if (Cells.Num() < Csv.Header.Num())
			{
				OutError = FString::Printf(TEXT("item_type_stats.csv line %d: %d cells, header has %d"), Line, Cells.Num(), Csv.Header.Num());
				return false;
			}

			int32 Id = 0;
			FS1ItemStat Stat;

			if (FDefaultValueHelper::ParseInt(Cells[ColId], Id) == false)
			{
				OutError = FString::Printf(TEXT("item_type_stats.csv line %d: id '%s' is not an integer"), Line, *Cells[ColId]);
				return false;
			}

			FS1ItemTypeDef* Def = Parsed.Find(static_cast<uint32>(Id));
			if (Def == nullptr)
			{
				OutError = FString::Printf(TEXT("item_type_stats.csv line %d: id %d is not in item_types.csv"), Line, Id);
				return false;
			}
			if (Cells[ColStat].IsEmpty())
			{
				OutError = FString::Printf(TEXT("item_type_stats.csv line %d: stat is empty"), Line);
				return false;
			}
			if (FDefaultValueHelper::ParseFloat(Cells[ColBase], Stat.BaseStat) == false
				|| FDefaultValueHelper::ParseFloat(Cells[ColFloor], Stat.FlatFloor) == false)
			{
				OutError = FString::Printf(TEXT("item_type_stats.csv line %d: base_stat or flat_floor is not a number"), Line);
				return false;
			}

			Stat.Stat = FName(*Cells[ColStat]);
			if (Def->Stats.ContainsByPredicate([&Stat](const FS1ItemStat& S) { return S.Stat == Stat.Stat; }))
			{
				OutError = FString::Printf(TEXT("item_type_stats.csv line %d: %d %s appears twice"), Line, Id, *Cells[ColStat]);
				return false;
			}

			Def->Stats.Add(Stat);
		}
	}

	Types = MoveTemp(Parsed);
	return true;
}

bool FS1ItemCatalog::LoadFromDirectory(const FString& Directory, FString& OutError)
{
	const FString TypesPath = FPaths::Combine(Directory, TEXT("item_types.csv"));
	const FString StatsPath = FPaths::Combine(Directory, TEXT("item_type_stats.csv"));

	FString TypesCsv;
	FString StatsCsv;

	if (FFileHelper::LoadFileToString(TypesCsv, *TypesPath) == false)
	{
		OutError = FString::Printf(TEXT("cannot read %s"), *TypesPath);
		return false;
	}
	if (FFileHelper::LoadFileToString(StatsCsv, *StatsPath) == false)
	{
		OutError = FString::Printf(TEXT("cannot read %s"), *StatsPath);
		return false;
	}

	return Parse(TypesCsv, StatsCsv, OutError);
}
