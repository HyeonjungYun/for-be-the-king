// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Game/Level/S1LevelLayout.h"

/*
 * Level rules — design/gdd/level-map.md Core Rules 4–11 · F1–F8 · Acceptance Criteria § B, ported from
 * tools/level/check_layout.py and check_house.py so the editor exporter refuses a level that breaks them.
 *
 * The Python tools stay the reference: tests run both on the golden fixture tools/level/floor1.json and the
 * numbers must agree (S1.Level.Rules.*).
 */

/** One rule check: Value compared with Limit. Margin > 0 means room to spare. */
struct FS1LevelCheck
{
	FString	Name;
	double	Value = 0.0;
	double	Limit = 0.0;
	bool	bAtLeast = true;		// Value >= Limit, else Value <= Limit
	FString	Note;

	bool	Passed() const { return bAtLeast ? Value >= Limit : Value <= Limit; }
	double	Margin() const { return bAtLeast ? Value - Limit : Limit - Value; }
};

struct S1_API FS1LevelReport
{
	TArray<FS1LevelCheck> Checks;

	void	Add(const FString& Name, double Value, double Limit, bool bAtLeast, const FString& Note = FString());
	int32	NumFailed() const;

	/** First check whose name starts with Prefix, or null. */
	const FS1LevelCheck* Find(const FString& Prefix) const;

	/** One line per check ("PASS name value >= limit margin +x"), failures only if asked. */
	FString	ToString(bool bFailuresOnly) const;
};

/** Distances and counts the checker compares against — design/gdd/level-map.md § Tuning Knobs (metres). */
struct FS1LevelLimits
{
	double HouseShortMin = 52.0, HouseLongMax = 60.0, InnerChestWall = 24.0;
	double TowerSeparation = 100.0, TowerHouse = 30.0, TowerBoundary = 12.0;
	double NearTowerMin = 40.0, NearTowerMax = 80.0, FarTowerMin = 80.0;
	double EntryBandMin = 15.0, EntryBandMax = 30.0, EntryOuterSeparation = 80.0;
	double EntryHouseMin = 20.0, EntryHouseMax = 30.0, EntryTower = 40.0, EntryClear = 9.0;
	double ChestSeparation = 45.0, ChestHouse = 30.0, ChestTower = 40.0, ChestEntry = 20.0, ChestBoundary = 10.0;
	double MonsterEntry = 20.0;
	double CoverMaxGap = 20.0;
	double DCoverMax = 170.0;
	double HouseAreaMin = 0.10, HouseAreaMax = 0.14;
	double TowerRing = 12.0;
	int32  EntryCount = 9;
	int32  OpenChestCount = 12;
};

namespace S1LevelRules
{
	constexpr double FloorHalf = 140.0;		// registry floor_size 280 m

	// ── Geometry ──────────────────────────────────────────────────────────────

	S1_API double BoundaryGap(const FVector2D& P);
	S1_API double RectDist(const FVector2D& P, const FS1LevelHouse& House);
	S1_API double SegmentDist(const FVector2D& P, const FVector2D& A, const FVector2D& B);
	S1_API double PolylineDist(const FVector2D& P, const TArray<FVector2D>& Points);
	S1_API double PolylineLength(const TArray<FVector2D>& Points);

	/** Points every Step metres along the polyline between fractions T0..T1 of its length (check_layout.py sample). */
	S1_API TArray<FVector2D> Sample(const TArray<FVector2D>& Points, double Step, double T0 = 0.0, double T1 = 1.0);

	/**
	 * Is a point at (Dx, Dy) from a viewer on the viewer's screen (+ Margin on every side)?
	 * Measured footprint: 13.1 m ahead, 8.0 m behind, half width 15.5 + 0.2887·x (camera-footprint-2026-10-05).
	 */
	S1_API bool Sees(double Dx, double Dy, double Margin = 1.0);

	// ── Formulas (level-map.md § Formulas) ─────────────────────────────────────

	/** F1 — a_min = 2·(r_radar + m_safety) + w_band. */
	S1_API double HouseMinShortSide(double RadarRadius = 22.5, double Safety = 1.5, double Band = 4.0);

	/** F2 — how far the next lantern may be when walking on HeadingDeg (0 = north) with the HUD covering HudBottom of the screen. */
	S1_API double LanternSpacingMax(double HeadingDeg, double HudBottom = 0.0);

	/** F4 — clearance a detour needs from a lantern route running at BetaDeg off north–south. */
	S1_API double DetourClearance(double BetaDeg, double Margin = 1.0);

	/** F5 — mean blind search time for a tower (seconds). */
	S1_API double BlindSearchSeconds(double Area = 78400.0, int32 Towers = 3, double RadarRadius = 22.5, double Speed = 3.4);

	// ── Checks ────────────────────────────────────────────────────────────────

	/** Houses, towers, entries, chests, monsters, tower reach (Core Rules 4–7, 10, 11 · F5). */
	S1_API void CheckLayout(const FS1LevelLayout& Layout, FS1LevelReport& Report, const FS1LevelLimits& Limits = FS1LevelLimits());

	/** Trails, lantern routes, detours, cover (Core Rules 8–9 · F4 · F7). Brute-forces detour visibility. */
	S1_API void CheckRoutes(const FS1LevelLayout& Layout, FS1LevelReport& Report, const FS1LevelLimits& Limits = FS1LevelLimits());

	/** One house interior (Core Rule 5 · AC-LM-12): rooms, reachability with any outer opening blocked, chests. */
	S1_API void CheckHouseGrid(const FS1HouseGrid& Grid, const FS1LevelHouse& House, FS1LevelReport& Report,
		const FS1LevelLimits& Limits = FS1LevelLimits());

	/**
	 * Differences between two layouts, positions within ToleranceM — empty means the same floor.
	 * The graybox round trip: build from the layout, export the level, compare with the layout.
	 */
	S1_API TArray<FString> CompareLayouts(const FS1LevelLayout& Expected, const FS1LevelLayout& Actual, double ToleranceM = 0.01);
}

/** The rooms of a house grid: components of cells joined by open (' ') edges. FIntPoint(X = u, Y = v). */
struct S1_API FS1HouseRooms
{
	TMap<FIntPoint, int32>		CellRoom;
	TArray<TArray<FIntPoint>>	Cells;
	TArray<TCHAR>				Labels;		// the room letter, '.' for corridors

	void Build(const FS1HouseGrid& Grid);

	/** The room holding the centre cell (6, 6) — the inner room. */
	int32 InnerRoom() const { return CellRoom.FindRef(FIntPoint(6, 6)); }

	/** Rooms with an outer door frame ('D') — "first rooms". */
	TSet<int32> FirstRooms(const FS1HouseGrid& Grid) const;
};
