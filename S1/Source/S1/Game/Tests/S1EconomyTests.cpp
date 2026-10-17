// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Game/BaseCamp/S1CampLayout.h"
#include "Game/Item/S1Economy.h"
#include "Game/Item/S1ItemLogic.h"

#if WITH_DEV_AUTOMATION_TESTS

/** Economy, first cut (2026-10-17, design/quick-specs/economy-loot-2026-10-17.md) — the client's labels match the server's formulas. */
namespace
{
	constexpr EAutomationTestFlags GS1EconomyTestFlags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1EconomyFormulaTest, "S1.Economy.Formulas_MatchTheServer", GS1EconomyTestFlags)
bool FS1EconomyFormulaTest::RunTest(const FString& Parameters)
{
	// Arrange
	FS1EconomyRules Rules;
	FS1LootCatalog Loot;
	FString Error;
	TestTrue(TEXT("loot parses"), Loot.Parse(TEXT("id,name_key,sell_price,stack_max\r\n905,loot.905,120,5\r\n"), Error));

	// Act / Assert - the same numbers as GameServerTests Economy.비용_매입가_사망차감_식
	TestEqual(TEXT("enhance L0"), S1Economy::EnhanceCost(0, Rules), static_cast<uint64>(20));
	TestEqual(TEXT("enhance L4"), S1Economy::EnhanceCost(4, Rules), static_cast<uint64>(49));
	TestEqual(TEXT("enhance L15"), S1Economy::EnhanceCost(15, Rules), static_cast<uint64>(568));

	FS1ItemView Shards;
	Shards.ItemTypeId = 905;
	Shards.StackCount = 3;
	TestEqual(TEXT("loot price = 120 x 3"), S1Economy::SellPrice(Shards, Loot, Rules), static_cast<uint64>(360));

	FS1ItemView Gear;
	Gear.ItemTypeId = 101;
	Gear.Grade = ES1ItemGrade::Rare;
	Gear.Level = 5;
	TestEqual(TEXT("rare +5 = 90"), S1Economy::SellPrice(Gear, Loot, Rules), static_cast<uint64>(90));

	// Rejections
	FS1LootCatalog Bad;
	TestFalse(TEXT("id outside 900..999"), Bad.Parse(TEXT("id,name_key,sell_price,stack_max\n101,x,5,10\n"), Error));
	TestFalse(TEXT("missing column"), Bad.Parse(TEXT("id,name_key\n901,x\n"), Error));

	// Rules override
	FS1EconomyRules Custom;
	TestTrue(TEXT("rules parse"), Custom.Parse(TEXT("key,value\nenhance_cost_base,30\nsell_grade_2,70\n"), Error));
	TestEqual(TEXT("base 30"), S1Economy::EnhanceCost(0, Custom), static_cast<uint64>(30));
	TestEqual(TEXT("grade 2 = 70"), Custom.SellGrade[2], static_cast<uint32>(70));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1EconomyLiveTest, "S1.Economy.LiveServerData_AndTradeNpc", GS1EconomyTestFlags)
bool FS1EconomyLiveTest::RunTest(const FString& Parameters)
{
	// The server's own files load, and the trade NPC opens the sell window; an unaffordable enhance says so
	const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../Server/Data")));
	FS1LootCatalog Loot;
	FS1EconomyRules Rules;
	FString Error;
	TestTrue(TEXT("loot_types.csv ") + Error, Loot.LoadFromDirectory(Dir, Error));
	TestTrue(TEXT("economy.csv ") + Error, Rules.LoadFromDirectory(Dir, Error));
	TestEqual(TEXT("six loot types"), Loot.Num(), 6);
	TestEqual(TEXT("death loss 10 %"), Rules.DeathGoldLossPct, static_cast<uint32>(10));

	const FS1CampDialogue Trade = FS1CampLayout::DialogueFor(TEXT("trade"));
	TestTrue(TEXT("trade opens the sell window"), Trade.Options.Num() == 2 && Trade.Options[0].Action == ES1CampAction::OpenSell);
	TestEqual(TEXT("not enough gold notice"), S1ItemLogic::NoticeFor(ES1ItemResult::NotEnoughGold, ES1ItemAction::Enhance), ES1ItemNotice::NotEnoughGold);
	return true;
}

#endif
