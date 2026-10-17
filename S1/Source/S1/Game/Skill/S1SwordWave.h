// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

/**
 * How a skill's projectile looks (S1/Content/Data/skill_vfx.csv, 2026-10-15). A skill not in the table keeps the stock
 * bolt / crescent drawing of US1ProjectileSubsystem.
 *   crescent_v - a sword wave standing upright, edge first: the arc lies in the plane of flight and up (검의 찬양 1009 -
 *                a downward cut throws a blade that stands on the ground). Height / depth / thickness in cm
 */
struct FS1ProjectileLook
{
	FName Kind;
	float HeightCm = 220.f;			// tip to tip
	float DepthCm = 55.f;			// how far the middle of the arc bulges ahead of the tips
	float ThicknessCm = 28.f;		// at the middle - the tips come to a point
	float SinkPct = 0.f;			// how much of the height is under the floor (optional sink_pct, 0..90) - 50 = a blade rising out of the ground
	FLinearColor Colour = FLinearColor(0.75f, 0.85f, 1.f);
};

/** A built crescent - local space: X = forward (the convex side), Z = up, Y = 0 (the blade has no width). */
struct FS1SwordWaveMesh
{
	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FLinearColor> Colours;		// leading edge white, body the skill's colour, trailing edge faint - the material shows these
	static constexpr int32 RowsPerStep = 3;		// lead · body · trail, per step along the arc
};

namespace S1SwordWave
{
	/** skill_vfx.csv: skill_id,projectile,height_cm,depth_cm,thickness_cm,colour (linear r g b 0..1)[,sink_pct]. False + OutError on a bad row. */
	S1_API bool ParseLooks(const FString& Csv, TMap<uint32, FS1ProjectileLook>& OutLooks, FString& OutError);

	/**
	 * The crescent plus Echoes fainter copies trailing behind it (the motion blur of a fast blade). Segments per arc.
	 * The arc is a circle through both tips (0, ±Height/2) and the front (Depth, 0); its thickness is Thickness at the
	 * middle and 0 at the tips.
	 */
	S1_API FS1SwordWaveMesh BuildCrescent(const FS1ProjectileLook& Look, int32 Segments = 24, int32 Echoes = 2, float EchoGapCm = 32.f);
}
