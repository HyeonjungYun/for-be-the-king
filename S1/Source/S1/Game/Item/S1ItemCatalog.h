// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

/**
 * One row of Server/Data/item_type_stats.csv.
 * Value = FlatFloor + BaseStat × grade multiplier. The level term is 🔒T3 — the MVP is level 0.
 */
struct FS1ItemStat
{
	FName	Stat;
	float	BaseStat = 0.f;
	float	FlatFloor = 0.f;
};

/** One row of Server/Data/item_types.csv, with its stats attached. */
struct FS1ItemTypeDef
{
	uint32				Id = 0;
	FString				NameKey;			// String Table key, e.g. item.101
	int32				EquipSlot = 0;		// Protocol::EquipSlot — 1 weapon · 3 helmet · 4 armor · 5 boots · 6 trinket
	FName				Archetype;			// heavy_weapon, armor_light, ...
	FName				DamageType;			// physical · magic · none
	float				AttackRangeCm = 0.f;
	FName				SkillPoolId;
	TArray<FS1ItemStat>	Stats;

	bool IsWeapon() const { return EquipSlot == 1; }
};

/**
 * Item type definitions — the client's reading of Server/Data (quick-spec S-1).
 *
 * 🔴 An item's part comes from this table, never from the digits of its id.
 * design/quick-specs/floor-item-pool-mvp-2026-10-02.md § 1.
 */
class FS1ItemCatalog
{
public:

	/**
	 * Parses both CSVs. Columns are found by header name, so their order may change.
	 * On the first bad line nothing is kept and OutError names the file and line.
	 */
	bool Parse(const FString& TypesCsv, const FString& StatsCsv, FString& OutError);

	/**
	 * Parses item_grades.csv (grade,multiplier) — the same file the server's ItemTable reads, so a
	 * tooltip number is the number the server fights with. All four grades must be there.
	 */
	bool ParseGrades(const FString& GradesCsv, FString& OutError);

	/** Reads item_types.csv, item_type_stats.csv and item_grades.csv from Directory. */
	bool LoadFromDirectory(const FString& Directory, FString& OutError);

	const FS1ItemTypeDef* Find(uint32 TypeId) const { return Types.Find(TypeId); }
	const TMap<uint32, FS1ItemTypeDef>& GetTypes() const { return Types; }
	int32 Num() const { return Types.Num(); }

	/** Common, Rare, Epic, Legendary — indexed by grade − 1. Empty until ParseGrades succeeds. */
	TArrayView<const float> GetGradeMultipliers() const { return GradeMultipliers; }

private:

	TMap<uint32, FS1ItemTypeDef> Types;
	TArray<float> GradeMultipliers;
};
