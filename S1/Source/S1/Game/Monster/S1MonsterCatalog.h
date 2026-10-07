// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

/** The columns of Server/Data/monster_types.csv the client draws with (design/gdd/monster-ai.md Core Rule 2). */
struct FS1MonsterTypeDef
{
	uint32	Id = 0;
	FString	NameKey;		// String Table key in S1Items, e.g. monster.hound
	FName	Tier;			// field · house · boss
	float	RadiusCm = 0.f;
	int32	MaxHp = 0;
};

/**
 * The client's reading of the server's monster table — the same file, so size and names can never disagree
 * (the item catalog does the same with item_types.csv). The server still decides everything that matters.
 */
class S1_API FS1MonsterCatalog
{
public:
	/** Columns are found by header name. On the first bad line nothing is kept and OutError names the line. */
	bool Parse(const FString& Csv, FString& OutError);

	/** Reads monster_types.csv from Directory. */
	bool LoadFromDirectory(const FString& Directory, FString& OutError);

	const FS1MonsterTypeDef* Find(uint32 TypeId) const { return Types.Find(TypeId); }
	int32 Num() const { return Types.Num(); }

	/** Loaded once from the item data directory (DefaultGame.ini ItemDataDirectory). Empty if the file is missing. */
	static const FS1MonsterCatalog& Get();

	/** Actor scale for a server radius — the player capsule is 42 cm. Clamped so a bad row cannot make a giant. */
	static float ScaleForRadius(float RadiusCm);

private:
	TMap<uint32, FS1MonsterTypeDef> Types;
};
