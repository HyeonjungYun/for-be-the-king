// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

/** One row of Server/Data/basecamp.csv (design/quick-specs/basecamp-map-2026-10-17.md). Centimetres, +X north · +Y east. */
struct FS1CampPoint
{
	FString	Id;
	float	X = 0.f;
	float	Y = 0.f;
	float	Yaw = 0.f;
	float	HalfX = 0.f;
	float	HalfY = 0.f;
};

/** Which part of S3 an NPC opens. All = the old whole screen (no camp map: server without basecamp.csv). */
enum class ES1CampSection : uint8
{
	None = 0,
	All,
	Stash,
	Enhance,
	Gate,
	Sell,		// 매입 행수 (2026-10-17 economy) - a click sells the cell
};

/** What a dialogue option does (US1BaseCampSubsystem::ChooseDialogueOption). */
enum class ES1CampAction : uint8
{
	Close = 0,
	OpenStash,		// 창고지기 — the vault and carried grids, worn strip (S3 stash section)
	OpenEnhance,	// 대장장이 — pick an item, [강화 +1]
	OpenGate,		// 길드장 — mode row, party, [입장] → S4
	OpenParty,		// 길드장 — the party window straight away
	OpenSell,		// 매입 행수 — the sell window (2026-10-17 economy)
};

struct FS1CampOption
{
	const TCHAR*	LabelKey = nullptr;		// S1ItemStrings.csv
	ES1CampAction	Action = ES1CampAction::Close;
};

/** One NPC's lines — fixed per role, the words live in the String Table. */
struct FS1CampDialogue
{
	const TCHAR*			NameKey = nullptr;
	const TCHAR*			LineKey = nullptr;
	TArray<FS1CampOption>	Options;
};

/**
 * The base-camp map as the server lays it out — the client reads the server's own file (like entry_points.csv),
 * so the walls the server checks and the props drawn here can never disagree. Rows: area · spawn · dummy · npc · prop.
 * The server owns the body, the scarecrow and the walls; the client draws the ground, props and NPC bodies from this.
 */
class S1_API FS1CampLayout
{
public:
	bool Parse(const FString& Csv, FString& OutError);
	bool LoadFromDirectory(const FString& Directory, FString& OutError);

	bool IsLoaded() const { return bLoaded; }
	const FS1CampPoint& GetArea() const { return Area; }
	const FS1CampPoint& GetSpawn() const { return Spawn; }
	const TArray<FS1CampPoint>& GetNpcs() const { return Npcs; }
	const TArray<FS1CampPoint>& GetProps() const { return Props; }
	const FS1CampPoint* FindNpc(const FString& Id) const;

	/** Inside the walkable square (the server's coast). */
	bool Contains(float X, float Y) const;

	/** The NPC nearest to (X, Y) within MaxCm of its body edge, or nullptr. */
	const FS1CampPoint* NearestNpc(float X, float Y, float MaxCm) const;

	/** stash · enhance · gate — anything else gets an empty dialogue (no options). */
	static FS1CampDialogue DialogueFor(const FString& NpcId);

private:
	bool					bLoaded = false;
	FS1CampPoint			Area;
	FS1CampPoint			Spawn;
	TArray<FS1CampPoint>	Npcs;
	TArray<FS1CampPoint>	Props;
};
