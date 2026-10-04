// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

/** One row of Server/Data/entry_points.csv — where a run can start. design/ux/entry-point.md */
struct FS1EntryPoint
{
	uint32	EntryId = 0;
	uint32	FloorId = 0;
	float	X = 0.f;
	float	Y = 0.f;
};

/**
 * The entry points S4 offers. The client reads the server's own file (the same way the item catalog
 * does — ItemDataDirectory), so the two can never disagree. The server still decides where you land.
 */
class S1_API FS1EntryPoints
{
public:
	bool Parse(const FString& Csv, FString& OutError);

	/** Reads entry_points.csv from Directory. */
	bool LoadFromDirectory(const FString& Directory, FString& OutError);

	/** File order — S4 letters them A, B, C … in this order. */
	TArray<FS1EntryPoint> OnFloor(uint32 FloorId) const;

	int32 Num() const { return Points.Num(); }

private:
	TArray<FS1EntryPoint> Points;
};
