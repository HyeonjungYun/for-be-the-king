// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Game/Party/S1PartySubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * PVE modes (ADR-0014 · design/quick-specs/pve-modes-2026-10-11.md) — S3's [입장] for each mode × party. The server refuses
 * the same cases (ServerPacketHandler Handle_C_ENTER_GAME · PartyManager::BeginCoopEnter), so a disabled button never
 * hides an entry the server would take.
 */
namespace
{
	constexpr EAutomationTestFlags GS1PartyTestFlags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1PartyEnterBlockTest, "S1.Party.Logic.EnterBlock_ModeAndParty_MatchesTheServer", GS1PartyTestFlags)
bool FS1PartyEnterBlockTest::RunTest(const FString& Parameters)
{
	using namespace S1PartyLogic;

	// No party or a party of one - every mode is open
	for (const ES1RunMode Mode : { ES1RunMode::Pvp, ES1RunMode::PveSolo, ES1RunMode::PveCoop })
	{
		TestTrue(TEXT("alone - any mode"), EnterBlock(Mode, 0, false, false) == EEnterBlock::None);
		TestTrue(TEXT("party of one - any mode"), EnterBlock(Mode, 1, true, false) == EEnterBlock::None);
	}

	// A party of 2+ - PVE co-op only (D7)
	TestTrue(TEXT("party of 2 - PVP refused"), EnterBlock(ES1RunMode::Pvp, 2, true, false) == EEnterBlock::CoopOnly);
	TestTrue(TEXT("party of 3 - solo refused"), EnterBlock(ES1RunMode::PveSolo, 3, false, true) == EEnterBlock::CoopOnly);

	// Co-op: the leader opens, a member waits - then joins while the run is alive
	TestTrue(TEXT("leader opens"), EnterBlock(ES1RunMode::PveCoop, 3, true, false) == EEnterBlock::None);
	TestTrue(TEXT("member waits"), EnterBlock(ES1RunMode::PveCoop, 3, false, false) == EEnterBlock::WaitForLeader);
	TestTrue(TEXT("member joins a live run"), EnterBlock(ES1RunMode::PveCoop, 2, false, true) == EEnterBlock::None);
	TestTrue(TEXT("leader joins a live run"), EnterBlock(ES1RunMode::PveCoop, 2, true, true) == EEnterBlock::None);

	// One entry per run (D3) - back in base camp after escaping or dying, wait for the party to come back
	TestTrue(TEXT("entered this run - wait"), EnterBlock(ES1RunMode::PveCoop, 3, false, true, true) == EEnterBlock::WaitForParty);
	TestTrue(TEXT("the run ended - open again"), EnterBlock(ES1RunMode::PveCoop, 3, true, false, true) == EEnterBlock::None);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1PartyEnterLabelTest, "S1.Party.Logic.EnterLabel_SaysWhatTheButtonDoes", GS1PartyTestFlags)
bool FS1PartyEnterLabelTest::RunTest(const FString& Parameters)
{
	using namespace S1PartyLogic;

	TestEqual(TEXT("PVP"), FString(EnterLabelKey(ES1RunMode::Pvp, 0, false, false, false)), FString(TEXT("label.enter")));
	TestEqual(TEXT("solo"), FString(EnterLabelKey(ES1RunMode::PveSolo, 1, true, false, false)), FString(TEXT("label.enter")));
	TestEqual(TEXT("co-op alone"), FString(EnterLabelKey(ES1RunMode::PveCoop, 1, true, false, false)), FString(TEXT("label.enter")));
	TestEqual(TEXT("co-op leader"), FString(EnterLabelKey(ES1RunMode::PveCoop, 3, true, false, false)), FString(TEXT("label.enter_coop_leader")));
	TestEqual(TEXT("co-op member"), FString(EnterLabelKey(ES1RunMode::PveCoop, 3, false, false, false)), FString(TEXT("label.enter_coop_wait")));
	TestEqual(TEXT("co-op run alive"), FString(EnterLabelKey(ES1RunMode::PveCoop, 2, false, true, false)), FString(TEXT("label.enter_coop_join")));
	TestEqual(TEXT("entering wins"), FString(EnterLabelKey(ES1RunMode::PveCoop, 3, true, false, true)), FString(TEXT("label.entering")));
	TestEqual(TEXT("already in this run"), FString(EnterLabelKey(ES1RunMode::PveCoop, 2, true, true, false, true)), FString(TEXT("label.enter_coop_back")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
