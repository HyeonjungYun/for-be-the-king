// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Game/Escape/S1EscapeLogic.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * design/gdd/escape-timer.md — the client's run HUD logic (the server decides; this only shows).
 *
 * Run headless:
 *   UnrealEditor-Cmd.exe S1.uproject -ExecCmds="Automation RunTests S1.Escape; Quit" -nullrhi -unattended
 */
namespace
{
	constexpr EAutomationTestFlags GS1EscapeTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1EscapeProgressTest, "S1.Escape.Logic.ProgressAt_ExtrapolatesAndClamps", GS1EscapeTestFlags)
bool FS1EscapeProgressTest::RunTest(const FString& Parameters)
{
	FS1GaugeView Filling;
	Filling.Progress = 0.5f;
	Filling.RatePerS = 1.f / 60.f;
	Filling.ReceivedAt = 100.0;

	TestTrue(TEXT("6 s after 50 % → 60 %"), FMath::IsNearlyEqual(S1EscapeLogic::ProgressAt(Filling, 106.0), 0.6f, 0.0001f));
	TestEqual(TEXT("clamped at full"), S1EscapeLogic::ProgressAt(Filling, 1000.0), 1.f);
	TestEqual(TEXT("before receipt reads the received value"), S1EscapeLogic::ProgressAt(Filling, 50.0), 0.5f);

	FS1GaugeView Draining = Filling;
	Draining.RatePerS = -2.f / 60.f;
	TestTrue(TEXT("draining twice as fast"), FMath::IsNearlyEqual(S1EscapeLogic::ProgressAt(Draining, 106.0), 0.3f, 0.0001f));
	TestEqual(TEXT("clamped at empty"), S1EscapeLogic::ProgressAt(Draining, 1000.0), 0.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1EscapeFormatTest, "S1.Escape.Logic.FormatRemaining_RoundsUp", GS1EscapeTestFlags)
bool FS1EscapeFormatTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("30 min"), S1EscapeLogic::FormatRemaining(1800.0), FString(TEXT("30:00")));
	TestEqual(TEXT("rounds up - 0:00 only when gone"), S1EscapeLogic::FormatRemaining(0.2), FString(TEXT("0:01")));
	TestEqual(TEXT("gone"), S1EscapeLogic::FormatRemaining(0.0), FString(TEXT("0:00")));
	TestEqual(TEXT("negative clamps"), S1EscapeLogic::FormatRemaining(-5.0), FString(TEXT("0:00")));
	TestEqual(TEXT("1:01"), S1EscapeLogic::FormatRemaining(60.4), FString(TEXT("1:01")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1EscapeWarningTest, "S1.Escape.Logic.WarningCrossed_OnlyTheLatestStep", GS1EscapeTestFlags)
bool FS1EscapeWarningTest::RunTest(const FString& Parameters)
{
	const TArray<int32> Steps = { 600, 300, 120, 60, 10 };
	TestEqual(TEXT("601 → 599 crosses 600"), S1EscapeLogic::WarningCrossed(601.0, 599.0, Steps), 600);
	TestEqual(TEXT("exactly at 120 counts"), S1EscapeLogic::WarningCrossed(120.5, 120.0, Steps), 120);
	TestEqual(TEXT("no crossing"), S1EscapeLogic::WarningCrossed(500.0, 499.0, Steps), -1);
	TestEqual(TEXT("a hitch over three steps fires only the last"), S1EscapeLogic::WarningCrossed(700.0, 50.0, Steps), 60);
	TestEqual(TEXT("going up never fires"), S1EscapeLogic::WarningCrossed(50.0, 700.0, Steps), -1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1EscapeDataTest, "S1.Escape.Logic.ServerFiles_TowersAndWarnings", GS1EscapeTestFlags)
bool FS1EscapeDataTest::RunTest(const FString& Parameters)
{
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../Server/Data")));

	FString Towers, Config, Error;
	TestTrue(TEXT("towers.csv"), FFileHelper::LoadFileToString(Towers, *FPaths::Combine(Directory, TEXT("towers.csv"))));
	TestTrue(TEXT("escape_config.csv"), FFileHelper::LoadFileToString(Config, *FPaths::Combine(Directory, TEXT("escape_config.csv"))));

	const TArray<FS1TowerView> Floor1 = S1EscapeLogic::ParseTowers(Towers, 0, Error);
	if (TestEqual(TEXT("three towers on floor 1"), Floor1.Num(), 3))
	{
		// Wire ids follow the string ids — the server makes the same mapping (escape-timer.md Core Rule 1)
		TestEqual(TEXT("Ta = 1"), Floor1[0].TowerId + FString::FromInt(Floor1[0].WireId), FString(TEXT("Ta1")));
		TestEqual(TEXT("Tc = 3"), Floor1[2].TowerId + FString::FromInt(Floor1[2].WireId), FString(TEXT("Tc3")));
		TestEqual(TEXT("Tc position"), Floor1[2].Position, FVector2D(-7800.0, 0.0));
	}
	TestEqual(TEXT("no towers on a TEMP floor"), S1EscapeLogic::ParseTowers(Towers, 2, Error).Num(), 0);

	const TArray<int32> Warnings = S1EscapeLogic::ParseWarnings(Config);
	TestEqual(TEXT("five warning steps"), Warnings.Num(), 5);
	if (Warnings.Num() == 5)
		TestEqual(TEXT("descending, 600 first"), Warnings[0], 600);

	TestEqual(TEXT("missing key falls back to the GDD default"), S1EscapeLogic::ParseWarnings(TEXT("key,value\n")).Num(), 5);
	return true;
}

#endif
