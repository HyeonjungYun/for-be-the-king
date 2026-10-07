// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

/**
 * design/gdd/radar.md — pure functions. The widget draws what CollectBlips returns and nothing else, so the
 * allow-list (towers · sealed chests · party) is enforced in one place the tests can see.
 *
 * 🔴 Enemy players and monsters have no Kind. They cannot be drawn because they cannot be expressed —
 *    a client filter, not a security boundary (the client still receives every position; AOI is Tier 2).
 */
enum class ES1RadarBlip : uint8
{
	Tower,
	Chest,
	Party,
};

struct FS1RadarBlip
{
	ES1RadarBlip	Kind = ES1RadarBlip::Tower;
	uint64			Id = 0;				// tower wire id · chest object id · party member id
	FVector2D		OffsetPx = FVector2D::ZeroVector;	// from the ring centre, screen axes (+x right, +y down)
	float			DistanceCm = 0.f;
	float			Alpha = 1.f;
};

struct FS1RadarChest
{
	uint64		Id = 0;
	FVector2D	Position = FVector2D::ZeroVector;
	bool		bKnownLooted = false;
};

struct FS1RadarInput
{
	FVector2D				Self = FVector2D::ZeroVector;
	TArray<TPair<uint32, FVector2D>>	Towers;		// wire id · position
	TArray<FS1RadarChest>	Chests;
	TArray<TPair<uint64, FVector2D>>	Party;		// Tier 2 — always empty in the MVP
	float					RadiusCm = 2250.f;
	float					FadeCm = 150.f;
	float					RingRadiusPx = 64.f;
};

struct FS1RadarLayout
{
	float	Scale = 1.f;			// viewport height / 1080
	float	RingDiameterPx = 0.f;
	float	WidgetSizePx = 0.f;
	float	MarginPx = 0.f;
	float	BottomPx = 0.f;			// margin + widget, from the top of the screen
	bool	bFitsFreeBand = false;	// the widget stays inside the top 15 % above the item panels (F4)
};

namespace S1RadarLogic
{
	constexpr uint64 ChestIdBase = 3'000'000'000ull;		// server CHEST_ID_BASE
	constexpr uint64 ChestIdEnd = 4'000'000'000ull;		// MONSTER_ID_BASE — monster ids start here

	/** F2 · AC-RD-01 — X, Y only, inclusive. NaN · Inf are outside. */
	bool IsInRadarRange(const FVector2D& Self, const FVector2D& Target, float RadiusCm);

	/** F1 · AC-RD-02 — north-up (+X world is up on screen): u = Δy / R · r, v = −Δx / R · r. */
	FVector2D WorldToRadarOffset(const FVector2D& Self, const FVector2D& Target, float RadiusCm, float RingRadiusPx);

	/** F5 · AC-RD-03 — 1 well inside, 0 at the rim, linear across the fade band. Fade 0 is a step. */
	float EdgeAlpha(float DistanceCm, float RadiusCm, float FadeCm);

	/** AC-RD-05 — the closed band [base, end). Bags (2e9) and monsters (4e9+) are not chests. */
	bool IsChestObjectId(uint64 ObjectId, uint64 Base = ChestIdBase, uint64 End = ChestIdEnd);

	/** AC-RD-07 — before S_CHEST_STATE: a chest we opened that was empty or shrank. Previous −1 = unknown. */
	bool ChestLootedFromContents(int32 PreviousCount, int32 NewCount);

	/** AC-RD-04 · 06 — what to draw, in drawing order (towers, chests, party; by id within a kind). */
	TArray<FS1RadarBlip> CollectBlips(const FS1RadarInput& Input);

	/** F4 · AC-RD-10 — ring and widget size for a viewport height, and whether it clears the item panels. */
	FS1RadarLayout ComputeLayout(float ViewportHeightPx, float RingDiameterRef = 128.f, float PaddingRef = 9.f, float MarginRef = 8.f);

	/** AC-RD-12 — only in the dungeon, alive, with a position, and not on the way out. */
	bool IsRadarVisible(bool bInDungeon, bool bAlive, bool bHasLocation, bool bEscaping);
}
