// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

/*
 * Floor layout data — design/gdd/level-map.md · design/levels/floor-1.md.
 *
 * Plain structs, no UObject: the same model feeds the rule checker (S1LevelRules), the graybox builder and the
 * exporter, and the automation tests run it headless against the golden fixture tools/level/floor1.json.
 *
 * Units are METRES on the floor plane. Origin = floor centre, +X = north (up on screen), +Y = east.
 */

/** A door or opening on a house's outer wall. Side N/S sits on x = X1 / X0, E/W on y = Y1 / Y0; At is the other coordinate. */
struct FS1LevelDoor
{
	TCHAR	Side = TEXT('N');
	double	At = 0.0;
	bool	bRubble = false;		// a collapsed wall gap rather than a door frame
};

struct FS1LevelHouse
{
	FString	Id;
	double	X0 = 0.0, X1 = 0.0, Y0 = 0.0, Y1 = 0.0;
	TArray<FS1LevelDoor> Doors;
	bool	bHasInnerChest = false;
	FVector2D InnerChest = FVector2D::ZeroVector;
	FString	GridFile;				// 29 x 29 interior grid, relative to the layout file (design/levels/floor-1.md § 4)
	TArray<FString> OpenAirRooms;	// room letters that are open air — field floor and ambience (H1 courtyard "Y")

	S1_API FVector2D DoorPoint(const FS1LevelDoor& Door) const;
	FVector2D Centre() const { return FVector2D((X0 + X1) * 0.5, (Y0 + Y1) * 0.5); }
};

struct FS1LevelTower
{
	FString		Id;
	FVector2D	P = FVector2D::ZeroVector;
};

struct FS1LevelEntry
{
	int32		Id = 0;
	bool		bOuter = true;		// false = a near-house point (Core Rule 7)
	FString		HouseId;
	FVector2D	P = FVector2D::ZeroVector;
};

struct FS1LevelChest
{
	int32		Id = 0;
	FString		HouseId;			// empty = open field
	FString		Room;
	FVector2D	P = FVector2D::ZeroVector;
};

struct FS1LevelMonster
{
	FString		Id;
	FString		HouseId;
	FVector2D	P = FVector2D::ZeroVector;
};

struct FS1LevelTrail
{
	FString				Id;
	TArray<FVector2D>	Points;
	bool				bLantern = false;
};

/** A lantern route or a detour: from a house door to a tower (Core Rule 8). */
struct FS1LevelRoute
{
	FString				HouseId;
	FString				TowerId;
	TArray<FVector2D>	Points;
};

struct S1_API FS1LevelLayout
{
	TArray<FS1LevelHouse>	Houses;
	TArray<FS1LevelTower>	Towers;
	TArray<FS1LevelEntry>	Entries;
	TArray<FS1LevelChest>	OpenChests;
	TArray<FS1LevelChest>	HouseChests;
	TArray<FS1LevelMonster>	Monsters;
	TArray<FS1LevelTrail>	Trails;
	TArray<FS1LevelRoute>	LanternRoutes;
	TArray<FS1LevelRoute>	Detours;
	TArray<FVector2D>		Cover;
	FString					BossHouse;

	/**
	 * The island coast (2026-10-11 map expansion, tools/level/gen_island.py) - a polygon in metres, clockwise from north.
	 * Empty = the old 280 m square. Every boundary rule measures to this line, and the builder raises the coast on it.
	 */
	TArray<FVector2D>		Outline;

	/** Open-field chests the rules ask for ("open_chest_count") - 0 = the limit's default (12). */
	int32					OpenChestCount = 0;

	/** Parses the tools/level/floor1.json format. */
	bool ParseJson(const FString& Json, FString& OutError);

	/** Square fallback half size when there is no outline (registry floor_size 280 m). */
	static constexpr double SquareHalf = 140.0;

	bool HasOutline() const { return Outline.Num() >= 3; }

	/** Distance to the play area's edge - the coast, or the square. Negative = outside (check_layout.py boundary_gap). */
	double BoundaryGap(const FVector2D& P) const;

	/** Inside the coast (ray casting) - always true for the square inside |x|,|y| <= 140. */
	bool IsInside(const FVector2D& P) const;

	/** Area of the play area in m² - the polygon, or 280². */
	double FloorArea() const;

	/** (X0, X1, Y0, Y1) of the play area. */
	void FloorBounds(double& OutX0, double& OutX1, double& OutY0, double& OutY1) const;

	/** Reads a layout file. Grid paths inside it stay relative to the file's directory. */
	bool LoadFromFile(const FString& Path, FString& OutError);

	const FS1LevelHouse* FindHouse(const FString& Id) const;
	const FS1LevelTower* FindTower(const FString& Id) const;
};

/**
 * One house interior on the 4 m module grid — 14 x 14 cells drawn as 29 x 29 characters
 * (design/levels/floor-1.md § 4). Cell (u, v): u from the south wall, v from the west wall.
 *   Edge characters: ' ' open · '-' '|' wall · 'D' door frame · '~' rubble gap
 *   Cell characters: room letter · '.' corridor · 'c' chest · 'B' boss spot
 *   Grid points:     '+' pillar · 'o' free-standing pillar
 */
struct S1_API FS1HouseGrid
{
	static constexpr int32	N = 14;
	static constexpr double	Cell = 4.0;
	static constexpr int32	Size = 2 * N + 1;

	TArray<FString> Rows;

	bool Parse(const FString& Text, FString& OutError);

	TCHAR At(int32 Row, int32 Col) const { return Rows[Row][Col]; }
	TCHAR CellChar(int32 U, int32 V) const { return At(27 - 2 * U, 2 * V + 1); }

	/** Character on the edge between cell (U, V) and its neighbour (U + DU, V + DV). */
	TCHAR Edge(int32 U, int32 V, int32 DU, int32 DV) const { return At(27 - 2 * U - DU, 2 * V + 1 + DV); }

	static bool IsPassable(TCHAR C) { return C == TEXT(' ') || C == TEXT('D') || C == TEXT('~'); }
	static bool IsWall(TCHAR C) { return C == TEXT('-') || C == TEXT('|'); }
};
