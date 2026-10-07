// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

/** One tower's gauge as the server last described it (S_ESCAPE_GAUGE). design/gdd/escape-timer.md § C */
struct FS1GaugeView
{
	uint32	TowerId = 0;			// 1.. (Ta=1 · Tb=2 · Tc=3)
	uint8	State = 0;				// Protocol::EscapeGaugeState
	float	Progress = 0.f;			// 0..1 when received
	float	RatePerS = 0.f;			// the server's slope — the client extrapolates between corrections
	double	ReceivedAt = 0.0;		// seconds, world clock
	bool	bSelfInZone = false;
};

/** One tower on the floor, from the server's towers.csv (the same file — the client cannot disagree). */
struct FS1TowerView
{
	FString		TowerId;
	uint32		WireId = 0;
	FVector2D	Position = FVector2D::ZeroVector;		// cm
};

/** Pure functions for the run HUD — Unreal automation tests call these (S1.Escape.*). */
namespace S1EscapeLogic
{
	/** Progress now, extrapolated along the server's rate and clamped to 0..1. */
	float ProgressAt(const FS1GaugeView& Gauge, double Now);

	/** "29:59" — whole seconds rounded up, so 0:00 only shows once the time is really gone. */
	FString FormatRemaining(double Seconds);

	/**
	 * The warning step (seconds left) crossed between two readings, or -1. Thresholds are
	 * descending (600, 300, 120, 60, 10 — escape_config run_warning_s). Only the latest one fires
	 * if several were crossed at once (a hitch must not play three sounds).
	 */
	int32 WarningCrossed(double PrevRemaining, double NowRemaining, const TArray<int32>& Thresholds);

	/**
	 * towers.csv (tower_id,floor_id,x,y) for one floor, sorted by tower_id — wire ids follow that
	 * order (Ta=1 · Tb=2 · Tc=3, escape-timer.md Core Rule 1). Empty on a bad file.
	 */
	TArray<FS1TowerView> ParseTowers(const FString& Csv, uint32 FloorId, FString& OutError);

	/** run_warning_s from escape_config.csv ("600;300;120;60;10"), descending. Falls back to the GDD default. */
	TArray<int32> ParseWarnings(const FString& ConfigCsv);
}
