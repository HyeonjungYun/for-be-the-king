// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/BaseCamp/S1EntryPoints.h"
#include "Misc/DefaultValueHelper.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

// Named, not anonymous — a unity build puts this next to S1ItemCatalog.cpp's own CSV helpers.
namespace S1EntryPointsCsv
{
	void SplitCells(const FString& Line, TArray<FString>& OutCells)
	{
		Line.ParseIntoArray(OutCells, TEXT(","), /*InCullEmpty*/ false);
		for (FString& Cell : OutCells)
			Cell.TrimStartAndEndInline();
	}
}

bool FS1EntryPoints::Parse(const FString& Csv, FString& OutError)
{
	using namespace S1EntryPointsCsv;

	TArray<FString> Lines;
	Csv.ParseIntoArrayLines(Lines, /*InCullEmpty*/ false);

	TArray<FString> Header;
	TArray<FS1EntryPoint> Parsed;
	int32 ColId = INDEX_NONE, ColFloor = INDEX_NONE, ColX = INDEX_NONE, ColY = INDEX_NONE;

	for (int32 i = 0; i < Lines.Num(); i++)
	{
		if (Lines[i].TrimStartAndEnd().IsEmpty())
			continue;

		TArray<FString> Cells;
		SplitCells(Lines[i], Cells);

		if (Header.Num() == 0)
		{
			Header = MoveTemp(Cells);
			ColId = Header.IndexOfByKey(FString(TEXT("entry_id")));
			ColFloor = Header.IndexOfByKey(FString(TEXT("floor_id")));
			ColX = Header.IndexOfByKey(FString(TEXT("x")));
			ColY = Header.IndexOfByKey(FString(TEXT("y")));

			if (ColId == INDEX_NONE || ColFloor == INDEX_NONE || ColX == INDEX_NONE || ColY == INDEX_NONE)
			{
				OutError = TEXT("entry_points.csv: header is missing entry_id, floor_id, x or y");
				return false;
			}
			continue;
		}

		const int32 Line = i + 1;
		if (Cells.Num() != Header.Num())
		{
			OutError = FString::Printf(TEXT("entry_points.csv line %d: %d cells, header has %d"), Line, Cells.Num(), Header.Num());
			return false;
		}

		int32 Id = 0, Floor = 0;
		FS1EntryPoint Point;
		if (FDefaultValueHelper::ParseInt(Cells[ColId], Id) == false || Id <= 0
			|| FDefaultValueHelper::ParseInt(Cells[ColFloor], Floor) == false || Floor < 0
			|| FDefaultValueHelper::ParseFloat(Cells[ColX], Point.X) == false
			|| FDefaultValueHelper::ParseFloat(Cells[ColY], Point.Y) == false)
		{
			OutError = FString::Printf(TEXT("entry_points.csv line %d: entry_id, floor_id, x and y must be numbers (entry_id > 0)"), Line);
			return false;
		}

		Point.EntryId = static_cast<uint32>(Id);
		Point.FloorId = static_cast<uint32>(Floor);
		Parsed.Add(Point);
	}

	Points = MoveTemp(Parsed);
	return true;
}

bool FS1EntryPoints::LoadFromDirectory(const FString& Directory, FString& OutError)
{
	FString Csv;
	const FString Path = FPaths::Combine(Directory, TEXT("entry_points.csv"));
	if (FFileHelper::LoadFileToString(Csv, *Path) == false)
	{
		OutError = FString::Printf(TEXT("cannot read %s"), *Path);
		return false;
	}

	return Parse(Csv, OutError);
}

TArray<FS1EntryPoint> FS1EntryPoints::OnFloor(uint32 FloorId) const
{
	return Points.FilterByPredicate([FloorId](const FS1EntryPoint& Point) { return Point.FloorId == FloorId; });
}
