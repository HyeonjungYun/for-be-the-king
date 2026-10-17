// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Game/BaseCamp/S1CampLayout.h"
#include "Game/Item/S1ItemLogic.h"
#include "Game/Item/S1ItemTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

/** Base-camp map (2026-10-17, design/quick-specs/basecamp-map-2026-10-17.md AC-BC-10 ~ 12) and the lone path notice. */
namespace
{
	constexpr EAutomationTestFlags GS1CampTestFlags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	const TCHAR* CampCsv =
		TEXT("kind,id,x,y,yaw,half_x,half_y\r\n")
		TEXT("area,camp,-24000,0,0,1500,1500\r\n")
		TEXT("spawn,player,-25000,0,0,0,0\r\n")
		TEXT("dummy,90,-24300,900,180,0,0\r\n")
		TEXT("npc,stash,-23300,-1000,180,70,70\r\n")
		TEXT("npc,enhance,-23300,-200,180,70,70\r\n")
		TEXT("npc,gate,-22750,700,180,70,70\r\n")
		TEXT("prop,anvil,-23300,150,0,60,40\r\n");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1CampLayoutParseTest, "S1.Camp.Layout_ParseAndReach", GS1CampTestFlags)
bool FS1CampLayoutParseTest::RunTest(const FString& Parameters)
{
	// Arrange
	FS1CampLayout Layout;
	FString Error;

	// Act
	const bool bOk = Layout.Parse(CampCsv, Error);

	// Assert - AC-BC-10 rows, the walkable square, the talk reach (from the body edge)
	TestTrue(TEXT("parsed"), bOk);
	TestEqual(TEXT("npcs"), Layout.GetNpcs().Num(), 3);
	TestEqual(TEXT("props"), Layout.GetProps().Num(), 1);
	TestEqual(TEXT("spawn x"), Layout.GetSpawn().X, -25000.f);
	TestTrue(TEXT("spawn inside"), Layout.Contains(-25000.f, 0.f));
	TestFalse(TEXT("sea outside"), Layout.Contains(-26000.f, 0.f));

	const FS1CampPoint* Near = Layout.NearestNpc(-23300.f - 70.f - 150.f, -1000.f, 220.f);	// 150 cm from the stash keeper's edge
	TestTrue(TEXT("stash keeper in reach"), Near != nullptr && Near->Id == TEXT("stash"));
	TestNull(TEXT("nobody in reach at the spawn"), Layout.NearestNpc(-25000.f, 0.f, 220.f));
	const FS1CampPoint* Between = Layout.NearestNpc(-23300.f, -650.f, 400.f);					// stash edge 280 cm, enhance edge 380 cm - both in reach
	TestTrue(TEXT("nearest wins"), Between != nullptr && Between->Id == TEXT("stash"));

	// Rejections - no area / no spawn / bad header
	FS1CampLayout Bad;
	TestFalse(TEXT("no area"), Bad.Parse(TEXT("kind,id,x,y,yaw,half_x,half_y\nspawn,player,0,0,0,0,0\n"), Error));
	TestFalse(TEXT("bad header"), Bad.Parse(TEXT("id,x\nfoo,1\n"), Error));
	TestFalse(TEXT("short row"), Bad.Parse(TEXT("kind,id,x,y,yaw,half_x,half_y\narea,camp,0,0\n"), Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1CampDialogueTest, "S1.Camp.Dialogue_EachNpcOpensItsOwnWindow", GS1CampTestFlags)
bool FS1CampDialogueTest::RunTest(const FString& Parameters)
{
	// AC-BC-11 - three NPCs, three separate jobs; the last option always leaves (Esc)
	const FS1CampDialogue Stash = FS1CampLayout::DialogueFor(TEXT("stash"));
	const FS1CampDialogue Enhance = FS1CampLayout::DialogueFor(TEXT("enhance"));
	const FS1CampDialogue Gate = FS1CampLayout::DialogueFor(TEXT("gate"));

	TestTrue(TEXT("stash opens the stash"), Stash.Options.Num() == 2 && Stash.Options[0].Action == ES1CampAction::OpenStash);
	TestTrue(TEXT("enhance opens the enhance window"), Enhance.Options.Num() == 2 && Enhance.Options[0].Action == ES1CampAction::OpenEnhance);
	TestTrue(TEXT("gate opens the gate and the party"), Gate.Options.Num() == 3 && Gate.Options[0].Action == ES1CampAction::OpenGate
		&& Gate.Options[1].Action == ES1CampAction::OpenParty);
	for (const FS1CampDialogue* D : { &Stash, &Enhance, &Gate })
	{
		TestTrue(TEXT("has a name and a line"), D->NameKey != nullptr && D->LineKey != nullptr);
		TestEqual(TEXT("last option leaves"), D->Options.Last().Action, ES1CampAction::Close);
	}
	TestEqual(TEXT("unknown npc - nothing to say"), FS1CampLayout::DialogueFor(TEXT("merchant")).Options.Num(), 0);

	// The lone path refusal reads as its own notice, not "지금은 할 수 없습니다"
	TestEqual(TEXT("lone path notice"), S1ItemLogic::NoticeFor(ES1ItemResult::LonePathInParty, ES1ItemAction::Equip), ES1ItemNotice::LonePathInParty);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1CampLiveDataTest, "S1.Camp.Layout_LiveServerData", GS1CampTestFlags)
bool FS1CampLiveDataTest::RunTest(const FString& Parameters)
{
	// AC-BC-12 - the client reads the server's own basecamp.csv (the walls the server checks = the props drawn here)
	FS1CampLayout Layout;
	FString Error;
	const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../Server/Data")));
	TestTrue(TEXT("loads ") + Error, Layout.LoadFromDirectory(Dir, Error));
	TestEqual(TEXT("three npcs"), Layout.GetNpcs().Num(), 3);
	for (const TCHAR* Id : { TEXT("stash"), TEXT("enhance"), TEXT("gate") })
	{
		const FS1CampPoint* Npc = Layout.FindNpc(Id);
		TestTrue(FString::Printf(TEXT("%s present and inside"), Id), Npc != nullptr && Layout.Contains(Npc->X, Npc->Y));
	}
	TestTrue(TEXT("spawn inside"), Layout.Contains(Layout.GetSpawn().X, Layout.GetSpawn().Y));
	return true;
}

#endif
