// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "Game/Escape/S1EscapeLogic.h"
#include "S1EscapeSubsystem.generated.h"

namespace Protocol
{
	class S_ESCAPE_GAUGE;
	class S_LEAVE_GAME;
}

class US1RunHudWidget;
class US1RadarWidget;
class US1WorldLabelWidget;
class US1BlindOverlayWidget;
class US1FogOfWarWidget;

/**
 * The client half of design/gdd/escape-timer.md — time left, tower gauges, the alarm, the run result.
 * The server decides everything; this only mirrors S_ENTER_GAME.run_remaining_ms, S_ESCAPE_GAUGE and
 * S_LEAVE_GAME, draws the tower zones on the ground and feeds the run HUD strip.
 *
 * Packets come in through ClientPacketHandler.cpp (escape-timer.md § Packets).
 */
UCLASS()
class S1_API US1EscapeSubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	static US1EscapeSubsystem* Get();

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// ── Netcode hooks ─────────────────────────────────────────────────────
	void HandleEnterGame(uint32 RunRemainingMs);
	void HandleGauge(const Protocol::S_ESCAPE_GAUGE& Pkt);
	void HandleLeftGame(const Protocol::S_LEAVE_GAME& Pkt);

	/** The floor's towers from towers.csv — the radar reads the same list. */
	const TArray<FS1TowerView>& GetTowers() const { return Towers; }

	/** Seconds left in this run, or -1 outside a run. */
	double GetRemainingSeconds() const;

	/** The radar disc (radar.md) — created with the run HUD so both live for the whole session. */
	const US1RadarWidget* GetRadar() const { return Radar; }

	// ── FTickableGameObject ───────────────────────────────────────────────
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(US1EscapeSubsystem, STATGROUP_Tickables); }
	virtual bool IsTickable() const override { return bInitialized; }
	virtual bool IsTickableInEditor() const override { return false; }

	/** In the dungeon and alive - the run HUD, radar and fog of war show only then. */
	bool InDungeon() const;

private:
	void EnsureHud();
	void UpdateHud(double Now);
	void DrawTowerZones(double Now);

	TArray<FS1TowerView>		Towers;
	TArray<int32>				Warnings;
	TMap<uint32, FS1GaugeView>	Gauges;
	bool						bAlertFilling = false;

	bool						bInRun = false;
	double						DeadlineAt = 0.0;
	double						LastRemaining = -1.0;

	FText						WarningText;
	double						WarningUntil = 0.0;
	FText						ResultText;
	double						ResultUntil = 0.0;

	float						ZoneRadiusCm = 1000.f;
	bool						bInitialized = false;

	UPROPERTY() TObjectPtr<US1RunHudWidget> Hud;
	UPROPERTY() TObjectPtr<US1RadarWidget> Radar;
	UPROPERTY() TObjectPtr<US1WorldLabelWidget> WorldLabels;
	UPROPERTY() TObjectPtr<US1BlindOverlayWidget> BlindOverlay;
	UPROPERTY() TObjectPtr<US1FogOfWarWidget> FogOfWar;
};
