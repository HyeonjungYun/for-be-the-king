// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Game/Radar/S1RadarLogic.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * design/gdd/radar.md § Acceptance Criteria A — the [UA] rows (BLOCKING).
 *
 * Run headless:
 *   UnrealEditor-Cmd.exe S1.uproject -ExecCmds="Automation RunTests S1.Radar; Quit" -nullrhi -unattended
 */
namespace
{
	constexpr EAutomationTestFlags GS1RadarTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	bool Near(const FVector2D& A, const FVector2D& B, double Tol = 0.01) { return A.Equals(B, Tol); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1RadarRangeTest, "S1.Radar.Logic.InRange_InclusiveXYOnly_AC_RD_01", GS1RadarTestFlags)
bool FS1RadarRangeTest::RunTest(const FString& Parameters)
{
	const FVector2D O(0, 0);
	TestTrue(TEXT("(2250, 0) inside"), S1RadarLogic::IsInRadarRange(O, FVector2D(2250, 0), 2250.f));
	TestFalse(TEXT("(2251, 0) outside"), S1RadarLogic::IsInRadarRange(O, FVector2D(2251, 0), 2250.f));
	TestTrue(TEXT("(1590, 1590) inside"), S1RadarLogic::IsInRadarRange(O, FVector2D(1590, 1590), 2250.f));
	TestFalse(TEXT("(1591, 1591) outside"), S1RadarLogic::IsInRadarRange(O, FVector2D(1591, 1591), 2250.f));
	TestFalse(TEXT("NaN outside"), S1RadarLogic::IsInRadarRange(O, FVector2D(NAN, 0), 2250.f));
	TestFalse(TEXT("Inf outside"), S1RadarLogic::IsInRadarRange(O, FVector2D(INFINITY, 0), 2250.f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1RadarMappingTest, "S1.Radar.Logic.WorldToRadar_NorthUp_AC_RD_02", GS1RadarTestFlags)
bool FS1RadarMappingTest::RunTest(const FString& Parameters)
{
	const FVector2D O(0, 0);
	TestTrue(TEXT("north is up"), Near(S1RadarLogic::WorldToRadarOffset(O, FVector2D(1125, 0), 2250.f, 64.f), FVector2D(0, -32)));
	TestTrue(TEXT("east is right"), Near(S1RadarLogic::WorldToRadarOffset(O, FVector2D(0, 1125), 2250.f, 64.f), FVector2D(32, 0)));
	TestTrue(TEXT("south is down"), Near(S1RadarLogic::WorldToRadarOffset(O, FVector2D(-1125, 0), 2250.f, 64.f), FVector2D(0, 32)));
	TestTrue(TEXT("west is left"), Near(S1RadarLogic::WorldToRadarOffset(O, FVector2D(0, -1125), 2250.f, 64.f), FVector2D(-32, 0)));
	TestTrue(TEXT("self is the centre"), Near(S1RadarLogic::WorldToRadarOffset(O, O, 2250.f, 64.f), FVector2D(0, 0)));
	TestTrue(TEXT("F1 example 1080p"), Near(S1RadarLogic::WorldToRadarOffset(O, FVector2D(1200, -1200), 2250.f, 64.f), FVector2D(-34.13, -34.13)));
	TestTrue(TEXT("F1 example 720p"), Near(S1RadarLogic::WorldToRadarOffset(O, FVector2D(1200, -1200), 2250.f, 42.67f), FVector2D(-22.76, -22.76)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1RadarFadeTest, "S1.Radar.Logic.EdgeAlpha_LinearBand_AC_RD_03", GS1RadarTestFlags)
bool FS1RadarFadeTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("rim 0"), S1RadarLogic::EdgeAlpha(2250.f, 2250.f, 150.f), 0.f);
	TestEqual(TEXT("band start 1"), S1RadarLogic::EdgeAlpha(2100.f, 2250.f, 150.f), 1.f);
	TestTrue(TEXT("half way 0.5"), FMath::IsNearlyEqual(S1RadarLogic::EdgeAlpha(2175.f, 2250.f, 150.f), 0.5f, 0.001f));
	TestEqual(TEXT("well inside 1"), S1RadarLogic::EdgeAlpha(2000.f, 2250.f, 150.f), 1.f);
	TestEqual(TEXT("fade 0 is a step - inside"), S1RadarLogic::EdgeAlpha(2250.f, 2250.f, 0.f), 1.f);
	TestEqual(TEXT("fade 0 is a step - outside"), S1RadarLogic::EdgeAlpha(2251.f, 2250.f, 0.f), 0.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1RadarAllowListTest, "S1.Radar.Logic.CollectBlips_OnlyTowersAndSealedChests_AC_RD_04_05_06", GS1RadarTestFlags)
bool FS1RadarAllowListTest::RunTest(const FString& Parameters)
{
	FS1RadarInput Input;
	Input.Towers.Emplace(1, FVector2D(500, 0));
	Input.Chests.Add({ 3'000'000'101ull, FVector2D(0, 500), false });		// sealed chest
	Input.Chests.Add({ 3'000'000'102ull, FVector2D(0, 600), true });		// looted - hidden
	Input.Chests.Add({ 2'000'000'001ull, FVector2D(0, 700), false });		// a death / monster bag - not a chest
	Input.Chests.Add({ 4'000'000'005ull, FVector2D(0, 800), false });		// a monster id - not a chest

	const TArray<FS1RadarBlip> Blips = S1RadarLogic::CollectBlips(Input);
	if (TestEqual(TEXT("tower + one sealed chest"), Blips.Num(), 2))
	{
		TestTrue(TEXT("tower first"), Blips[0].Kind == ES1RadarBlip::Tower);
		TestTrue(TEXT("then the chest"), Blips[1].Kind == ES1RadarBlip::Chest && Blips[1].Id == 3'000'000'101ull);
	}

	// The id band (AC-RD-05)
	TestFalse(TEXT("2,999,999,999"), S1RadarLogic::IsChestObjectId(2'999'999'999ull));
	TestTrue(TEXT("3,000,000,000"), S1RadarLogic::IsChestObjectId(3'000'000'000ull));
	TestTrue(TEXT("3,999,999,999"), S1RadarLogic::IsChestObjectId(3'999'999'999ull));
	TestFalse(TEXT("4,000,000,000 monster band"), S1RadarLogic::IsChestObjectId(4'000'000'000ull));
	TestFalse(TEXT("UINT64_MAX"), S1RadarLogic::IsChestObjectId(MAX_uint64));

	// Out of range drops it
	FS1RadarInput Far;
	Far.Towers.Emplace(1, FVector2D(2300, 0));
	TestEqual(TEXT("tower 23 m away hidden"), S1RadarLogic::CollectBlips(Far).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1RadarLootedTest, "S1.Radar.Logic.ChestLootedFromContents_AC_RD_07", GS1RadarTestFlags)
bool FS1RadarLootedTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("(-1, 0)"), S1RadarLogic::ChestLootedFromContents(-1, 0));
	TestTrue(TEXT("(2, 1)"), S1RadarLogic::ChestLootedFromContents(2, 1));
	TestTrue(TEXT("(1, 0)"), S1RadarLogic::ChestLootedFromContents(1, 0));
	TestFalse(TEXT("(-1, 2)"), S1RadarLogic::ChestLootedFromContents(-1, 2));
	TestFalse(TEXT("(2, 2) - only opened"), S1RadarLogic::ChestLootedFromContents(2, 2));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1RadarLayoutTest, "S1.Radar.Logic.ComputeLayout_FitsAboveThePanels_AC_RD_10", GS1RadarTestFlags)
bool FS1RadarLayoutTest::RunTest(const FString& Parameters)
{
	struct FCase { float H; float Ring; float Widget; float Bottom; };
	for (const FCase& C : { FCase{ 720.f, 85.33f, 97.33f, 102.67f }, FCase{ 1080.f, 128.f, 146.f, 154.f }, FCase{ 1440.f, 170.67f, 194.67f, 205.33f } })
	{
		const FS1RadarLayout L = S1RadarLogic::ComputeLayout(C.H);
		TestTrue(*FString::Printf(TEXT("%.0f ring"), C.H), FMath::IsNearlyEqual(L.RingDiameterPx, C.Ring, 0.1f));
		TestTrue(*FString::Printf(TEXT("%.0f widget"), C.H), FMath::IsNearlyEqual(L.WidgetSizePx, C.Widget, 0.1f));
		TestTrue(*FString::Printf(TEXT("%.0f bottom"), C.H), FMath::IsNearlyEqual(L.BottomPx, C.Bottom, 0.1f));
		TestTrue(*FString::Printf(TEXT("%.0f fits"), C.H), L.bFitsFreeBand);
	}
	TestFalse(TEXT("140 u ring overlaps the panels"), S1RadarLogic::ComputeLayout(1080.f, 140.f).bFitsFreeBand);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1RadarVisibleTest, "S1.Radar.Logic.IsRadarVisible_AllFourOnly_AC_RD_12", GS1RadarTestFlags)
bool FS1RadarVisibleTest::RunTest(const FString& Parameters)
{
	int32 Visible = 0;
	for (int32 Bits = 0; Bits < 16; Bits++)
	{
		const bool bShown = S1RadarLogic::IsRadarVisible((Bits & 1) != 0, (Bits & 2) != 0, (Bits & 4) != 0, (Bits & 8) != 0);
		Visible += bShown ? 1 : 0;
		if (bShown)
			TestEqual(TEXT("only dungeon · alive · located · not escaping"), Bits, 7);
	}
	TestEqual(TEXT("one combination of sixteen"), Visible, 1);
	return true;
}

#endif
