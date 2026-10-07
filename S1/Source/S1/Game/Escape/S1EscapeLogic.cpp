// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/Escape/S1EscapeLogic.h"

float S1EscapeLogic::ProgressAt(const FS1GaugeView& Gauge, double Now)
{
	const double Elapsed = FMath::Max(0.0, Now - Gauge.ReceivedAt);
	return FMath::Clamp(static_cast<float>(Gauge.Progress + Gauge.RatePerS * Elapsed), 0.f, 1.f);
}

FString S1EscapeLogic::FormatRemaining(double Seconds)
{
	const int32 Whole = FMath::Max(0, FMath::CeilToInt32(Seconds));
	return FString::Printf(TEXT("%d:%02d"), Whole / 60, Whole % 60);
}

int32 S1EscapeLogic::WarningCrossed(double PrevRemaining, double NowRemaining, const TArray<int32>& Thresholds)
{
	int32 Crossed = -1;
	for (int32 Threshold : Thresholds)
	{
		// Passed it on the way down: was above, is now at or below
		if (PrevRemaining > Threshold && NowRemaining <= Threshold)
		{
			if (Crossed == -1 || Threshold < Crossed)
				Crossed = Threshold;
		}
	}
	return Crossed;
}

TArray<FS1TowerView> S1EscapeLogic::ParseTowers(const FString& Csv, uint32 FloorId, FString& OutError)
{
	TArray<FS1TowerView> Towers;

	TArray<FString> Lines;
	Csv.ParseIntoArrayLines(Lines, /*InCullEmpty*/ true);
	if (Lines.Num() == 0)
		return Towers;

	TArray<FString> Header;
	Lines[0].TrimStartAndEnd().ParseIntoArray(Header, TEXT(","), false);
	const int32 IdCol = Header.IndexOfByKey(TEXT("tower_id"));
	const int32 FloorCol = Header.IndexOfByKey(TEXT("floor_id"));
	const int32 XCol = Header.IndexOfByKey(TEXT("x"));
	const int32 YCol = Header.IndexOfByKey(TEXT("y"));
	if (IdCol == INDEX_NONE || FloorCol == INDEX_NONE || XCol == INDEX_NONE || YCol == INDEX_NONE)
	{
		OutError = TEXT("towers.csv: header is missing tower_id, floor_id, x or y");
		return Towers;
	}

	for (int32 Line = 1; Line < Lines.Num(); Line++)
	{
		TArray<FString> Cells;
		Lines[Line].TrimStartAndEnd().ParseIntoArray(Cells, TEXT(","), false);
		if (Cells.Num() != Header.Num())
		{
			OutError = FString::Printf(TEXT("towers.csv line %d: %d cells, header has %d"), Line + 1, Cells.Num(), Header.Num());
			return TArray<FS1TowerView>();
		}

		if (static_cast<uint32>(FCString::Atoi(*Cells[FloorCol])) != FloorId)
			continue;

		FS1TowerView& Tower = Towers.AddDefaulted_GetRef();
		Tower.TowerId = Cells[IdCol];
		Tower.Position = FVector2D(FCString::Atof(*Cells[XCol]), FCString::Atof(*Cells[YCol]));
	}

	Towers.Sort([](const FS1TowerView& A, const FS1TowerView& B) { return A.TowerId < B.TowerId; });
	for (int32 i = 0; i < Towers.Num(); i++)
		Towers[i].WireId = static_cast<uint32>(i + 1);

	return Towers;
}

TArray<int32> S1EscapeLogic::ParseWarnings(const FString& ConfigCsv)
{
	TArray<int32> Warnings;

	TArray<FString> Lines;
	ConfigCsv.ParseIntoArrayLines(Lines, true);
	for (const FString& Line : Lines)
	{
		FString Key, Value;
		if (Line.TrimStartAndEnd().Split(TEXT(","), &Key, &Value) && Key == TEXT("run_warning_s"))
		{
			TArray<FString> Parts;
			Value.ParseIntoArray(Parts, TEXT(";"), true);
			for (const FString& Part : Parts)
			{
				const int32 Seconds = FCString::Atoi(*Part);
				if (Seconds > 0)
					Warnings.Add(Seconds);
			}
		}
	}

	if (Warnings.Num() == 0)
		Warnings = { 600, 300, 120, 60, 10 };

	Warnings.Sort([](int32 A, int32 B) { return A > B; });
	return Warnings;
}
