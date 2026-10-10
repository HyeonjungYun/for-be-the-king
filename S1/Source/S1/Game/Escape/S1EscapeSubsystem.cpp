// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/Escape/S1EscapeSubsystem.h"

#include "DrawDebugHelpers.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Game/BaseCamp/S1BaseCampSubsystem.h"
#include "Game/UI/S1BlindOverlayWidget.h"
#include "Game/UI/S1RunHudWidget.h"
#include "Game/UI/S1RadarWidget.h"
#include "Game/UI/S1WorldLabelWidget.h"
#include "HAL/PlatformTime.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Protocol.pb.h"

namespace
{
	TWeakObjectPtr<US1EscapeSubsystem> GEscape;

	constexpr int32 RunHudZOrder = 25;			// above S3 (20) so the result banner shows over base camp

	// Blind darkness (2026-10-11) covers the world and the monster names but not the player's own panels (item UI 10),
	// radar or run HUD — a blinded player loses the surroundings, not their UI
	constexpr int32 BlindOverlayZOrder = 9;
	constexpr int32 WorldLabelsZOrder = 8;
	constexpr double WarningSeconds = 3.0;
	constexpr double ResultSeconds = 6.0;

	FText Str(const TCHAR* Key)
	{
		return FText::FromStringTable(TEXT("S1Items"), Key);
	}

	FString DataDirectory()
	{
		FString Relative;
		GConfig->GetString(TEXT("/Script/S1.S1ItemSubsystem"), TEXT("ItemDataDirectory"), Relative, GGameIni);
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), Relative));
	}
}

US1EscapeSubsystem* US1EscapeSubsystem::Get()
{
	return GEscape.Get();
}

void US1EscapeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	GEscape = this;

	const FString Directory = DataDirectory();
	FString Text, Error;

	if (FFileHelper::LoadFileToString(Text, *FPaths::Combine(Directory, TEXT("towers.csv"))))
		Towers = S1EscapeLogic::ParseTowers(Text, /*FloorId*/ 0, Error);

	FString Config;
	FFileHelper::LoadFileToString(Config, *FPaths::Combine(Directory, TEXT("escape_config.csv")));
	Warnings = S1EscapeLogic::ParseWarnings(Config);

	UE_LOG(LogTemp, Log, TEXT("[ESCAPE] towers=%d warnings=%d %s"), Towers.Num(), Warnings.Num(), *Error);
	bInitialized = true;
}

void US1EscapeSubsystem::Deinitialize()
{
	bInitialized = false;
	if (Hud != nullptr)
		Hud->RemoveFromParent();
	if (Radar != nullptr)
		Radar->RemoveFromParent();
	if (WorldLabels != nullptr)
		WorldLabels->RemoveFromParent();
	Hud = nullptr;
	Radar = nullptr;
	WorldLabels = nullptr;
	GEscape = nullptr;
	Super::Deinitialize();
}

void US1EscapeSubsystem::HandleEnterGame(uint32 RunRemainingMs)
{
	Gauges.Reset();
	bAlertFilling = false;
	ResultUntil = 0.0;

	bInRun = RunRemainingMs > 0;
	DeadlineAt = FPlatformTime::Seconds() + RunRemainingMs / 1000.0;
	LastRemaining = RunRemainingMs / 1000.0;

	UE_LOG(LogTemp, Log, TEXT("[ESCAPE] run started - %u ms left"), RunRemainingMs);
}

void US1EscapeSubsystem::HandleGauge(const Protocol::S_ESCAPE_GAUGE& Pkt)
{
	// tower 0 = alert only: someone started (or stopped) filling a tower nearby — no position, no progress
	if (Pkt.tower_id() == 0)
	{
		bAlertFilling = (Pkt.state() == Protocol::ESCAPE_GAUGE_FILLING);
		UE_LOG(LogTemp, Log, TEXT("[ESCAPE] alert %s"), bAlertFilling ? TEXT("filling") : TEXT("idle"));
		return;
	}

	if (Pkt.state() == Protocol::ESCAPE_GAUGE_IDLE && Pkt.progress_permille() == 0 && Pkt.self_in_zone() == false)
	{
		Gauges.Remove(Pkt.tower_id());
		return;
	}

	FS1GaugeView& View = Gauges.FindOrAdd(Pkt.tower_id());
	View.TowerId = Pkt.tower_id();
	View.State = static_cast<uint8>(Pkt.state());
	View.Progress = Pkt.progress_permille() / 1000.f;
	View.RatePerS = Pkt.rate_per_s();
	View.ReceivedAt = FPlatformTime::Seconds();
	View.bSelfInZone = Pkt.self_in_zone();

	UE_LOG(LogTemp, Log, TEXT("[ESCAPE] gauge tower=%u state=%d permille=%u self=%d"), Pkt.tower_id(), static_cast<int32>(Pkt.state()), Pkt.progress_permille(), Pkt.self_in_zone() ? 1 : 0);
}

void US1EscapeSubsystem::HandleLeftGame(const Protocol::S_LEAVE_GAME& Pkt)
{
	bInRun = false;
	Gauges.Reset();
	bAlertFilling = false;

	switch (Pkt.reason())
	{
	case Protocol::RUN_END_ESCAPED:
		ResultText = Pkt.gold_kept() > 0
			? FText::Format(Str(TEXT("run.result.escaped_gold")), FText::AsNumber(Pkt.gold_kept()))
			: Str(TEXT("run.result.escaped"));
		break;
	case Protocol::RUN_END_EXPIRED:
		ResultText = FText::Format(Str(TEXT("run.result.expired")), FText::AsNumber(Pkt.items_destroyed()), FText::AsNumber(Pkt.gold_kept()));
		break;
	default:
		ResultText = FText::GetEmpty();		// death has its own screen
		break;
	}

	ResultUntil = ResultText.IsEmpty() ? 0.0 : FPlatformTime::Seconds() + ResultSeconds;
	UE_LOG(LogTemp, Log, TEXT("[ESCAPE] run ended reason=%d gold=%llu destroyed=%u"), static_cast<int32>(Pkt.reason()), Pkt.gold_kept(), Pkt.items_destroyed());
}

double US1EscapeSubsystem::GetRemainingSeconds() const
{
	return bInRun ? FMath::Max(0.0, DeadlineAt - FPlatformTime::Seconds()) : -1.0;
}

bool US1EscapeSubsystem::InDungeon() const
{
	const US1BaseCampSubsystem* BaseCamp = US1BaseCampSubsystem::Get();
	return BaseCamp != nullptr && BaseCamp->GetPlace() == ES1Place::Dungeon && BaseCamp->IsLocalDead() == false;
}

void US1EscapeSubsystem::EnsureHud()
{
	if (Hud != nullptr)
		return;

	UGameInstance* GameInstance = GetGameInstance();
	APlayerController* PC = GameInstance != nullptr ? GameInstance->GetFirstLocalPlayerController() : nullptr;
	if (PC == nullptr)
		return;

	Hud = CreateWidget<US1RunHudWidget>(PC, US1RunHudWidget::StaticClass());
	if (Hud != nullptr)
		Hud->AddToViewport(RunHudZOrder);

	// Below the item panels' modal layers, above the world. It hides itself outside the dungeon
	Radar = CreateWidget<US1RadarWidget>(PC, US1RadarWidget::StaticClass());
	if (Radar != nullptr)
		Radar->AddToViewport(RunHudZOrder - 1);

	// Monster names over heads — painted in screen space because the world text font has no Hangul
	WorldLabels = CreateWidget<US1WorldLabelWidget>(PC, US1WorldLabelWidget::StaticClass());
	if (WorldLabels != nullptr)
		WorldLabels->AddToViewport(WorldLabelsZOrder);

	// Paints nothing until the server blinds the local player
	BlindOverlay = CreateWidget<US1BlindOverlayWidget>(PC, US1BlindOverlayWidget::StaticClass());
	if (BlindOverlay != nullptr)
		BlindOverlay->AddToViewport(BlindOverlayZOrder);
}

void US1EscapeSubsystem::UpdateHud(double Now)
{
	if (Hud == nullptr)
		return;

	const bool bDungeon = InDungeon();

	// ── Time left ──
	if (bDungeon && bInRun)
	{
		const double Remaining = FMath::Max(0.0, DeadlineAt - Now);
		const int32 Crossed = S1EscapeLogic::WarningCrossed(LastRemaining, Remaining, Warnings);
		if (Crossed > 0)
		{
			WarningText = FText::Format(Str(TEXT("run.warning")), FText::FromString(S1EscapeLogic::FormatRemaining(Crossed)));
			WarningUntil = Now + WarningSeconds;
			UE_LOG(LogTemp, Log, TEXT("[ESCAPE] warning %d s left"), Crossed);
		}
		LastRemaining = Remaining;

		// 120 s is the "leave now or not at all" line (escape-timer.md F4)
		Hud->SetTimer(FText::Format(Str(TEXT("run.timer")), FText::FromString(S1EscapeLogic::FormatRemaining(Remaining))), Remaining <= 120.0);
		Hud->SetWarning(Now < WarningUntil ? WarningText : FText::GetEmpty());
	}
	else
	{
		Hud->SetTimer(FText::GetEmpty(), false);
		Hud->SetWarning(FText::GetEmpty());
	}

	// ── Gauge: the one tower we can see (towers are 144 m apart — at most one at a time) ──
	const FS1GaugeView* Shown = nullptr;
	for (const TPair<uint32, FS1GaugeView>& Pair : Gauges)
	{
		if (Shown == nullptr || Pair.Value.bSelfInZone)
			Shown = &Pair.Value;
	}

	if (bDungeon && Shown != nullptr)
	{
		const float Progress = S1EscapeLogic::ProgressAt(*Shown, Now);
		const bool bDraining = Shown->State == Protocol::ESCAPE_GAUGE_DRAINING;
		Hud->SetGauge(true, Progress, FText::Format(Str(TEXT("run.gauge")), FText::AsNumber(FMath::FloorToInt32(Progress * 100.f))), bDraining);

		// We are in the zone and it is filling — everyone within 40 m has been told (escape-timer.md Core Rule 12)
		const bool bSelfAlarm = Shown->bSelfInZone && Shown->State == Protocol::ESCAPE_GAUGE_FILLING;
		Hud->SetAlarm(bSelfAlarm ? Str(TEXT("run.alarm.self")) : FText::GetEmpty(), true);
	}
	else
	{
		Hud->SetGauge(false, 0.f, FText::GetEmpty(), false);
		Hud->SetAlarm(bDungeon && bAlertFilling ? Str(TEXT("run.alarm.nearby")) : FText::GetEmpty(), false);
	}

	Hud->SetResult(Now < ResultUntil ? ResultText : FText::GetEmpty());
}

void US1EscapeSubsystem::DrawTowerZones(double Now)
{
	if (InDungeon() == false)
		return;

	UGameInstance* GameInstance = GetGameInstance();
	UWorld* World = GameInstance != nullptr ? GameInstance->GetWorld() : nullptr;
	if (World == nullptr)
		return;

	for (const FS1TowerView& Tower : Towers)
	{
		const FVector Centre(Tower.Position, 3.0);

		// The zone edge — where "in" starts (10 m, inside the 12 m plaza). Debug draw until the art exists
		DrawDebugCircle(World, Centre, ZoneRadiusCm, 72, FColor(200, 180, 110), false, -1.f, 0, 6.f, FVector(1, 0, 0), FVector(0, 1, 0), false);

		const FS1GaugeView* Gauge = Gauges.Find(Tower.WireId);
		if (Gauge == nullptr)
			continue;

		// Progress as an arc just outside the edge, clockwise from north
		const float Progress = S1EscapeLogic::ProgressAt(*Gauge, Now);
		const int32 Segments = FMath::Max(1, FMath::RoundToInt32(72 * Progress));
		const float Radius = ZoneRadiusCm + 60.f;
		const FColor ArcColor = (Gauge->State == Protocol::ESCAPE_GAUGE_DRAINING) ? FColor(150, 140, 110) : FColor(240, 200, 80);
		for (int32 i = 0; i < Segments && Progress > 0.f; i++)
		{
			const float A0 = 2.f * PI * i / 72.f;
			const float A1 = 2.f * PI * FMath::Min(i + 1.f, 72.f * Progress) / 72.f;
			const FVector P0 = Centre + FVector(FMath::Cos(A0), FMath::Sin(A0), 0.f) * Radius;
			const FVector P1 = Centre + FVector(FMath::Cos(A1), FMath::Sin(A1), 0.f) * Radius;
			DrawDebugLine(World, P0, P1, ArcColor, false, -1.f, 0, 14.f);
		}
	}
}

void US1EscapeSubsystem::Tick(float DeltaTime)
{
	EnsureHud();

	const double Now = FPlatformTime::Seconds();
	UpdateHud(Now);
	DrawTowerZones(Now);
}
