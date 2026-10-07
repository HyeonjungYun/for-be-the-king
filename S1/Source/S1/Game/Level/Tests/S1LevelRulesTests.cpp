// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Game/Level/S1LevelLayout.h"
#include "Game/Level/S1LevelRules.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * design/gdd/level-map.md § Acceptance Criteria — § B (exporter rules) and § C (formulas), BLOCKING.
 *
 * The golden fixture is the approved floor-1 layout (design/levels/floor-1.md): tools/level/floor1.json and
 * tools/level/houses/H{1,2,3}.txt. The Python tools under tools/level/ check the same file — both must pass it.
 *
 * Run headless:
 *   UnrealEditor-Cmd.exe S1.uproject -ExecCmds="Automation RunTests S1.Level; Quit" -nullrhi -unattended
 */
namespace
{
	constexpr EAutomationTestFlags GS1LevelTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	FString LevelToolsDir()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../tools/level")));
	}

	bool LoadGolden(FAutomationTestBase& Test, FS1LevelLayout& Out)
	{
		FString Error;
		const FString Path = FPaths::Combine(LevelToolsDir(), TEXT("floor1.json"));
		if (!Out.LoadFromFile(Path, Error))
		{
			Test.AddError(FString::Printf(TEXT("golden fixture %s: %s"), *Path, *Error));
			return false;
		}
		return true;
	}

	bool LoadGrid(FAutomationTestBase& Test, const FS1LevelHouse& House, FS1HouseGrid& Out)
	{
		FString Text, Error;
		const FString Path = FPaths::Combine(LevelToolsDir(), House.GridFile);
		if (!FFileHelper::LoadFileToString(Text, *Path) || !Out.Parse(Text, Error))
		{
			Test.AddError(FString::Printf(TEXT("grid %s: %s"), *Path, *Error));
			return false;
		}
		return true;
	}

	/** Runs every layout rule and reports failures as test errors. */
	int32 Failures(FAutomationTestBase& Test, const FS1LevelLayout& Layout, FS1LevelReport* OutReport = nullptr)
	{
		FS1LevelReport Report;
		S1LevelRules::CheckLayout(Layout, Report);
		S1LevelRules::CheckRoutes(Layout, Report);
		if (OutReport != nullptr)
		{
			*OutReport = Report;
		}
		return Report.NumFailed();
	}

	/** True if the first check whose name starts with Prefix exists and failed. */
	bool FailsCheck(const FS1LevelLayout& Layout, const FString& Prefix)
	{
		FS1LevelReport Report;
		S1LevelRules::CheckLayout(Layout, Report);
		const FS1LevelCheck* Check = Report.Find(Prefix);
		return Check != nullptr && !Check->Passed();
	}
}

// ── Golden fixture — every rule passes (AC-LM-11 … 21 on the real floor) ─────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1LevelGoldenLayoutTest, "S1.Level.Rules.GoldenFloor1_EveryLayoutAndRouteRule_Passes", GS1LevelTestFlags)
bool FS1LevelGoldenLayoutTest::RunTest(const FString& Parameters)
{
	FS1LevelLayout Layout;
	if (!LoadGolden(*this, Layout))
	{
		return false;
	}

	FS1LevelReport Report;
	const int32 Failed = Failures(*this, Layout, &Report);
	if (Failed > 0)
	{
		AddError(Report.ToString(/*bFailuresOnly*/ true));
	}
	TestEqual(TEXT("failed checks"), Failed, 0);
	TestTrue(TEXT("checks ran"), Report.Checks.Num() > 400);

	// The same numbers the Python checker prints (tools/level/check_layout.py).
	if (const FS1LevelCheck* Cover = Report.Find(TEXT("d_cover (F5)")))
	{
		TestEqual(TEXT("worst distance to a tower (F5)"), Cover->Value, 153.11, 0.01);
	}
	for (const TCHAR* Tag : { TEXT("detour H1 middle third"), TEXT("detour H2 middle third"), TEXT("detour H3 middle third") })
	{
		const FS1LevelCheck* Detour = Report.Find(Tag);
		TestTrue(FString::Printf(TEXT("%s checked"), Tag), Detour != nullptr);
		if (Detour != nullptr)
		{
			TestEqual(FString::Printf(TEXT("%s visible pairs"), Tag), Detour->Value, 0.0);
		}
	}
	if (const FS1LevelCheck* Lantern = Report.Find(TEXT("lantern H2->Tc path length")))
	{
		TestEqual(TEXT("H2 lantern route length"), Lantern->Value, 42.17, 0.01);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1LevelGoldenHousesTest, "S1.Level.Rules.GoldenFloor1_HouseGrids_PassAndMatchLayoutDoors", GS1LevelTestFlags)
bool FS1LevelGoldenHousesTest::RunTest(const FString& Parameters)
{
	FS1LevelLayout Layout;
	if (!LoadGolden(*this, Layout))
	{
		return false;
	}

	TestEqual(TEXT("houses with a grid"), Layout.Houses.FilterByPredicate([](const FS1LevelHouse& H) { return !H.GridFile.IsEmpty(); }).Num(), 3);
	for (const FS1LevelHouse& House : Layout.Houses)
	{
		FS1HouseGrid Grid;
		if (!LoadGrid(*this, House, Grid))
		{
			continue;
		}
		FS1LevelReport Report;
		S1LevelRules::CheckHouseGrid(Grid, House, Report);
		if (Report.NumFailed() > 0)
		{
			AddError(FString::Printf(TEXT("house %s\n%s"), *House.Id, *Report.ToString(true)));
		}
		TestEqual(FString::Printf(TEXT("house %s failed checks"), *House.Id), Report.NumFailed(), 0);
	}
	return true;
}

// ── One unit past each limit fails, the limit itself passes (± 1 cm) ──────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1LevelBoundaryTest, "S1.Level.Rules.Boundaries_OneCentimetrePastALimit_Fails", GS1LevelTestFlags)
bool FS1LevelBoundaryTest::RunTest(const FString& Parameters)
{
	FS1LevelLayout Golden;
	if (!LoadGolden(*this, Golden))
	{
		return false;
	}

	{	// AC-LM-12 / F1 — short side 52 m
		FS1LevelLayout L = Golden;
		L.Houses[0].Y1 = L.Houses[0].Y0 + 51.99;
		TestTrue(TEXT("house 51.99 m wide fails"), FailsCheck(L, TEXT("house H1 short side")));
		L.Houses[0].Y1 = L.Houses[0].Y0 + 52.0;
		TestFalse(TEXT("house 52 m wide passes"), FailsCheck(L, TEXT("house H1 short side")));
	}
	{	// AC-LM-13 — towers 100 m apart
		FS1LevelLayout L = Golden;
		FS1LevelTower* Ta = L.Towers.FindByPredicate([](const FS1LevelTower& T) { return T.Id == TEXT("Ta"); });
		FS1LevelTower* Tb = L.Towers.FindByPredicate([](const FS1LevelTower& T) { return T.Id == TEXT("Tb"); });
		if (TestTrue(TEXT("Ta and Tb exist"), Ta != nullptr && Tb != nullptr))
		{
			Tb->P = Ta->P + FVector2D(0.0, 99.99);
			TestTrue(TEXT("towers 99.99 m apart fail"), FailsCheck(L, TEXT("tower Ta-Tb separation")));
			Tb->P = Ta->P + FVector2D(0.0, 100.0);
			TestFalse(TEXT("towers 100 m apart pass"), FailsCheck(L, TEXT("tower Ta-Tb separation")));
		}
	}
	{	// AC-LM-14 — outer entry band 15–30 m inside the boundary
		FS1LevelLayout L = Golden;
		FS1LevelEntry* E1 = L.Entries.FindByPredicate([](const FS1LevelEntry& E) { return E.Id == 1; });
		if (TestTrue(TEXT("entry 1 exists"), E1 != nullptr))
		{
			E1->P = FVector2D(125.01, 0.0);
			TestTrue(TEXT("14.99 m from the boundary fails"), FailsCheck(L, TEXT("entry 1 band min")));
			E1->P = FVector2D(125.0, 0.0);
			TestFalse(TEXT("15 m from the boundary passes"), FailsCheck(L, TEXT("entry 1 band min")));
		}
	}
	{	// AC-LM-18 — open-field chests 45 m apart
		FS1LevelLayout L = Golden;
		L.OpenChests[1].P = L.OpenChests[0].P + FVector2D(-44.99, 0.0);
		const FString Pair = FString::Printf(TEXT("chest %d-%d"), L.OpenChests[0].Id, L.OpenChests[1].Id);
		TestTrue(TEXT("chests 44.99 m apart fail"), FailsCheck(L, Pair));
		L.OpenChests[1].P = L.OpenChests[0].P + FVector2D(-45.0, 0.0);
		TestFalse(TEXT("chests 45 m apart pass"), FailsCheck(L, Pair));
	}
	return true;
}

// ── House grid — a changed wall is caught ─────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1LevelGridMutationTest, "S1.Level.Rules.HouseGrid_DoorOrChestRemoved_IsCaught", GS1LevelTestFlags)
bool FS1LevelGridMutationTest::RunTest(const FString& Parameters)
{
	FS1LevelLayout Layout;
	if (!LoadGolden(*this, Layout))
	{
		return false;
	}
	const FS1LevelHouse* H1 = Layout.FindHouse(TEXT("H1"));
	FS1HouseGrid Grid;
	if (!TestNotNull(TEXT("H1"), H1) || !LoadGrid(*this, *H1, Grid))
	{
		return false;
	}

	{	// Bricking up the south door: the level and the layout data disagree (AC-LM-09 / 21 spirit — data matches the level)
		FS1HouseGrid Bricked = Grid;
		const int32 Col = Bricked.Rows[28].Find(TEXT("D"));
		if (TestTrue(TEXT("south door found"), Col != INDEX_NONE))
		{
			Bricked.Rows[28][Col] = TEXT('-');
			FS1LevelReport Report;
			S1LevelRules::CheckHouseGrid(Bricked, *H1, Report);
			const FS1LevelCheck* Match = Report.Find(TEXT("house H1 grid layout doors match outer openings"));
			TestTrue(TEXT("missing door is reported"), Match != nullptr && !Match->Passed());
		}
	}
	{	// Taking the chest out of the inner room (AC-LM-12)
		FS1HouseGrid Emptied = Grid;
		Emptied.Rows[27 - 2 * 6][2 * 7 + 1] = TEXT('M');	// cell (6, 7) — the inner chest
		FS1LevelReport Report;
		S1LevelRules::CheckHouseGrid(Emptied, *H1, Report);
		const FS1LevelCheck* Inner = Report.Find(TEXT("house H1 grid has an inner-room chest"));
		TestTrue(TEXT("no inner chest is reported"), Inner != nullptr && !Inner->Passed());
	}
	return true;
}

// ── Round trip comparison (AC-LM-20 — export, then compare with the layout) ────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1LevelCompareTest, "S1.Level.Rules.CompareLayouts_SameOrMoved_ReportsOnlyTheMove", GS1LevelTestFlags)
bool FS1LevelCompareTest::RunTest(const FString& Parameters)
{
	FS1LevelLayout Golden;
	if (!LoadGolden(*this, Golden))
	{
		return false;
	}

	TestEqual(TEXT("a layout equals itself"), S1LevelRules::CompareLayouts(Golden, Golden).Num(), 0);

	FS1LevelLayout Moved = Golden;
	Moved.Entries[3].P += FVector2D(0.02, 0.0);		// 2 cm — past the 1 cm tolerance
	const TArray<FString> Diffs = S1LevelRules::CompareLayouts(Golden, Moved);
	TestEqual(TEXT("one moved entry, one difference"), Diffs.Num(), 1);
	if (Diffs.Num() == 1)
	{
		TestTrue(TEXT("names the entry"), Diffs[0].Contains(FString::Printf(TEXT("entry %d"), Golden.Entries[3].Id)));
	}

	FS1LevelLayout Nudged = Golden;
	Nudged.Cover[0] += FVector2D(0.005, 0.0);		// 5 mm — inside the tolerance
	TestEqual(TEXT("sub-centimetre nudge is not a difference"), S1LevelRules::CompareLayouts(Golden, Nudged).Num(), 0);
	return true;
}

// ── Formulas (AC-LM-31 … 35) ──────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1LevelFormulaTest, "S1.Level.Rules.Formulas_F1F2F4F5_MatchTheGdd", GS1LevelTestFlags)
bool FS1LevelFormulaTest::RunTest(const FString& Parameters)
{
	using namespace S1LevelRules;

	// F1 — house short side from the radar radius
	TestEqual(TEXT("F1 default"), HouseMinShortSide(), 52.0, 1e-9);
	TestEqual(TEXT("F1 widest band"), HouseMinShortSide(22.5, 1.5, 12.0), 60.0, 1e-9);

	// F2 — next lantern on screen, by heading (HUD 0 %)
	TestEqual(TEXT("F2 north"), LanternSpacingMax(0.0), 9.9, 0.05);
	TestEqual(TEXT("F2 north-east"), LanternSpacingMax(45.0), 14.4, 0.05);
	TestEqual(TEXT("F2 east"), LanternSpacingMax(90.0), 14.5, 0.05);
	TestEqual(TEXT("F2 south-east"), LanternSpacingMax(135.0), 10.3, 0.05);
	TestEqual(TEXT("F2 south"), LanternSpacingMax(180.0), 7.0, 0.05);
	TestTrue(TEXT("F2 5 m spacing holds with the HUD over 13 % of the bottom"), LanternSpacingMax(180.0, 0.13) >= 5.0);
	TestFalse(TEXT("F2 6 m spacing breaks at 8 %"), LanternSpacingMax(180.0, 0.08) >= 6.0);

	// F4 — detour clearance by lantern-route direction, and the adopted 22.5 / 20 / 24.5 m cover every angle in range
	TestEqual(TEXT("F4 0°"), DetourClearance(0.0), 20.3, 0.05);
	TestEqual(TEXT("F4 10°"), DetourClearance(10.0), 22.3, 0.05);
	TestEqual(TEXT("F4 34°"), DetourClearance(34.0), 24.3, 0.05);
	TestEqual(TEXT("F4 70°"), DetourClearance(70.0), 19.9, 0.05);
	TestEqual(TEXT("F4 90°"), DetourClearance(90.0), 14.1, 0.05);
	for (double Beta = 0.0; Beta <= 90.0; Beta += 0.5)
	{
		const double Adopted = (Beta <= 10.0) ? 22.5 : (Beta >= 70.0) ? 20.0 : 24.5;
		if (DetourClearance(Beta) > Adopted)
		{
			AddError(FString::Printf(TEXT("F4 adopted %.1f m is short at %.1f°: needs %.2f"), Adopted, Beta, DetourClearance(Beta)));
		}
	}

	// F5 — blind search for a tower
	TestEqual(TEXT("F5 mean"), BlindSearchSeconds(), 170.8, 0.1);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
