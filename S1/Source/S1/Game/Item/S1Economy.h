// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Game/Item/S1ItemTypes.h"

/**
 * Economy, first cut (2026-10-17, design/quick-specs/economy-loot-2026-10-17.md) — the client reads the server's own
 * economy.csv and loot_types.csv to SHOW prices and costs. The server decides; these numbers only label buttons.
 */

/** One sell-only loot type (loot_types.csv). */
struct FS1LootTypeDef
{
	uint32	Id = 0;
	FString	NameKey;
	uint32	SellPrice = 0;
	uint32	StackMax = 1;
};

class S1_API FS1LootCatalog
{
public:
	bool Parse(const FString& Csv, FString& OutError);
	bool LoadFromDirectory(const FString& Directory, FString& OutError);

	const FS1LootTypeDef* Find(uint32 Id) const { return Types.Find(Id); }
	bool IsLoot(uint32 TypeId) const { return Types.Contains(TypeId); }
	int32 Num() const { return Types.Num(); }

private:
	TMap<uint32, FS1LootTypeDef> Types;
};

struct S1_API FS1EconomyRules
{
	uint32	EnhanceCostBase = 20;
	float	EnhanceCostGrowth = 1.25f;
	uint32	DeathGoldLossPct = 10;
	uint32	SellGrade[5] = { 0, 20, 60, 180, 500 };
	float	SellLevelBonus = 0.10f;

	/** Unknown keys are skipped (the server refuses them at boot). Missing file = defaults. */
	bool Parse(const FString& Csv, FString& OutError);
	bool LoadFromDirectory(const FString& Directory, FString& OutError);
};

namespace S1Economy
{
	/** round(base × growth^Level) — the server's Economy::EnhanceCost. */
	S1_API uint64 EnhanceCost(uint32 Level, const FS1EconomyRules& Rules);

	/** One cell's price: loot = price × count, gear = round(grade base × (1 + bonus × level)). */
	S1_API uint64 SellPrice(const FS1ItemView& Item, const FS1LootCatalog& Loot, const FS1EconomyRules& Rules);
}
