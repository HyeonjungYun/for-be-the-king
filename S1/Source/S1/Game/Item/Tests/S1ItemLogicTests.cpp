// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Game/Item/S1ItemLogic.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * design/ux/item-ui.md § Acceptance Criteria — the [unit] rows (BLOCKING).
 *
 * Run headless:
 *   UnrealEditor-Cmd.exe S1.uproject -ExecCmds="Automation RunTests S1.Item; Quit" -nullrhi -unattended
 *
 * Enums are compared as int32 so a failure prints the number instead of failing to format.
 */
namespace
{
	constexpr EAutomationTestFlags GS1ItemTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	FS1ItemView MakeItem(ES1ItemGrade Grade, ES1ItemState State)
	{
		FS1ItemView Item;
		Item.InstanceId = 1;
		Item.Grade = Grade;
		Item.State = State;
		return Item;
	}

	FS1BagView MakeBag(uint64 BagId, const FVector& Location)
	{
		FS1BagView Bag;
		Bag.BagId = BagId;
		Bag.bHasLocation = true;
		Bag.Location = Location;
		return Bag;
	}

	template <typename EnumType>
	int32 AsInt(EnumType Value)
	{
		return static_cast<int32>(Value);
	}
}

// ── AC-UI-02 — result code → notice ──────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1ItemNoticeForTest, "S1.Item.Logic.NoticeFor_EachResultAndAction_MatchesSpecTable", GS1ItemTestFlags)
bool FS1ItemNoticeForTest::RunTest(const FString& Parameters)
{
	using S1ItemLogic::NoticeFor;

	// Success and idempotent duplicates say nothing.
	for (ES1ItemAction Action : { ES1ItemAction::Loot, ES1ItemAction::Equip, ES1ItemAction::Unequip,
		ES1ItemAction::Drop, ES1ItemAction::Stash, ES1ItemAction::Unstash })
	{
		TestEqual(TEXT("Ok is silent"), AsInt(NoticeFor(ES1ItemResult::Ok, Action)), AsInt(ES1ItemNotice::None));
		TestEqual(TEXT("Already is silent"), AsInt(NoticeFor(ES1ItemResult::Already, Action)), AsInt(ES1ItemNotice::None));
	}

	// Losing a loot race is shown by the slot emptying, not by text.
	TestEqual(TEXT("loot NotFound is silent"),
		AsInt(NoticeFor(ES1ItemResult::NotFound, ES1ItemAction::Loot)), AsInt(ES1ItemNotice::None));
	TestEqual(TEXT("equip NotFound says not found"),
		AsInt(NoticeFor(ES1ItemResult::NotFound, ES1ItemAction::Equip)), AsInt(ES1ItemNotice::NotFound));
	TestEqual(TEXT("stash NotFound says not found"),
		AsInt(NoticeFor(ES1ItemResult::NotFound, ES1ItemAction::Stash)), AsInt(ES1ItemNotice::NotFound));

	TestEqual(TEXT("drop InvalidState = unequip first"),
		AsInt(NoticeFor(ES1ItemResult::InvalidState, ES1ItemAction::Drop)), AsInt(ES1ItemNotice::MustUnequipFirst));
	TestEqual(TEXT("loot InvalidState = too far"),
		AsInt(NoticeFor(ES1ItemResult::InvalidState, ES1ItemAction::Loot)), AsInt(ES1ItemNotice::TooFar));
	TestEqual(TEXT("equip InvalidState = cannot now"),
		AsInt(NoticeFor(ES1ItemResult::InvalidState, ES1ItemAction::Equip)), AsInt(ES1ItemNotice::CannotNow));

	TestEqual(TEXT("loot InventoryFull"),
		AsInt(NoticeFor(ES1ItemResult::InventoryFull, ES1ItemAction::Loot)), AsInt(ES1ItemNotice::InventoryFull));
	TestEqual(TEXT("unequip InventoryFull"),
		AsInt(NoticeFor(ES1ItemResult::InventoryFull, ES1ItemAction::Unequip)), AsInt(ES1ItemNotice::InventoryFull));
	TestEqual(TEXT("stash InventoryFull = vault full"),
		AsInt(NoticeFor(ES1ItemResult::InventoryFull, ES1ItemAction::Stash)), AsInt(ES1ItemNotice::VaultFull));
	TestEqual(TEXT("unstash InventoryFull = carried full"),
		AsInt(NoticeFor(ES1ItemResult::InventoryFull, ES1ItemAction::Unstash)), AsInt(ES1ItemNotice::CarriedFull));

	TestEqual(TEXT("BadRequest"),
		AsInt(NoticeFor(ES1ItemResult::BadRequest, ES1ItemAction::Equip)), AsInt(ES1ItemNotice::BadRequest));
	TestEqual(TEXT("DbError = retry later"),
		AsInt(NoticeFor(ES1ItemResult::DbError, ES1ItemAction::Stash)), AsInt(ES1ItemNotice::RetryLater));

	// A code from a newer server still produces a notice.
	TestEqual(TEXT("unknown code is not silent"),
		AsInt(NoticeFor(static_cast<ES1ItemResult>(99), ES1ItemAction::Loot)), AsInt(ES1ItemNotice::CannotNow));

	return true;
}

// ── AC-UI-03 — discard rule ──────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1ItemDiscardRuleTest, "S1.Item.Logic.DiscardRule_GradeStateAndHistory_PicksImmediateConfirmOrReject", GS1ItemTestFlags)
bool FS1ItemDiscardRuleTest::RunTest(const FString& Parameters)
{
	using S1ItemLogic::DiscardRuleFor;

	TestEqual(TEXT("rare asks first"),
		AsInt(DiscardRuleFor(MakeItem(ES1ItemGrade::Rare, ES1ItemState::Carried), false)), AsInt(ES1DiscardRule::Confirm));
	TestEqual(TEXT("legendary asks first"),
		AsInt(DiscardRuleFor(MakeItem(ES1ItemGrade::Legendary, ES1ItemState::Vaulted), false)), AsInt(ES1DiscardRule::Confirm));
	TestEqual(TEXT("common, never equipped this session, goes at once"),
		AsInt(DiscardRuleFor(MakeItem(ES1ItemGrade::Common, ES1ItemState::Carried), false)), AsInt(ES1DiscardRule::Immediate));
	TestEqual(TEXT("common, equipped this session, asks first"),
		AsInt(DiscardRuleFor(MakeItem(ES1ItemGrade::Common, ES1ItemState::Carried), true)), AsInt(ES1DiscardRule::Confirm));
	TestEqual(TEXT("equipped is refused whatever the grade"),
		AsInt(DiscardRuleFor(MakeItem(ES1ItemGrade::Common, ES1ItemState::Equipped), true)), AsInt(ES1DiscardRule::Reject));
	TestEqual(TEXT("equipped legendary is refused too"),
		AsInt(DiscardRuleFor(MakeItem(ES1ItemGrade::Legendary, ES1ItemState::Equipped), false)), AsInt(ES1DiscardRule::Reject));

	return true;
}

// ── AC-UI-04 — which bag F opens ─────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1ItemNearestBagTest, "S1.Item.Logic.FindNearestBag_BagsAroundPawn_PicksNearestInRange", GS1ItemTestFlags)
bool FS1ItemNearestBagTest::RunTest(const FString& Parameters)
{
	using S1ItemLogic::FindNearestBag;

	const FVector Origin = FVector::ZeroVector;
	const float Range = 200.f;

	{
		TMap<uint64, FS1BagView> Bags;
		TestEqual(TEXT("no bags -> 0"), static_cast<int64>(FindNearestBag(Bags, Origin, Range)), static_cast<int64>(0));
	}
	{
		TMap<uint64, FS1BagView> Bags;
		Bags.Add(10, MakeBag(10, FVector(150, 0, 0)));
		Bags.Add(20, MakeBag(20, FVector(100, 0, 0)));
		TestEqual(TEXT("nearer of two wins"), static_cast<int64>(FindNearestBag(Bags, Origin, Range)), static_cast<int64>(20));
	}
	{
		TMap<uint64, FS1BagView> Bags;
		Bags.Add(10, MakeBag(10, FVector(201, 0, 0)));
		TestEqual(TEXT("just outside range -> 0"), static_cast<int64>(FindNearestBag(Bags, Origin, Range)), static_cast<int64>(0));
	}
	{
		TMap<uint64, FS1BagView> Bags;
		Bags.Add(10, MakeBag(10, FVector(200, 0, 0)));
		TestEqual(TEXT("exactly at range is inside"), static_cast<int64>(FindNearestBag(Bags, Origin, Range)), static_cast<int64>(10));
	}
	{
		// The server measures on the floor plane (Room.cpp IsInLootRange). Height must not matter.
		TMap<uint64, FS1BagView> Bags;
		Bags.Add(10, MakeBag(10, FVector(100, 0, 900)));
		TestEqual(TEXT("height is ignored"), static_cast<int64>(FindNearestBag(Bags, Origin, Range)), static_cast<int64>(10));
	}
	{
		TMap<uint64, FS1BagView> Bags;
		FS1BagView NoSpawnYet;
		NoSpawnYet.BagId = 10;
		Bags.Add(10, NoSpawnYet);
		TestEqual(TEXT("bag without a spawn position is skipped"), static_cast<int64>(FindNearestBag(Bags, Origin, Range)), static_cast<int64>(0));
	}
	{
		TMap<uint64, FS1BagView> Bags;
		Bags.Add(30, MakeBag(30, FVector(0, 100, 0)));
		Bags.Add(20, MakeBag(20, FVector(100, 0, 0)));
		TestEqual(TEXT("tie goes to the lower id"), static_cast<int64>(FindNearestBag(Bags, Origin, Range)), static_cast<int64>(20));
	}

	return true;
}

// ── Loot window auto-close — the server drops viewers who walk away without a packet ──

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1ItemLootRangeTest, "S1.Item.Logic.IsWithinLootRange_WalkingAway_TurnsFalsePastRange", GS1ItemTestFlags)
bool FS1ItemLootRangeTest::RunTest(const FString& Parameters)
{
	using S1ItemLogic::IsWithinLootRange;

	const FVector Bag(1000, 1000, 100);
	const float Range = 200.f;

	TestTrue(TEXT("standing on it"), IsWithinLootRange(Bag, FVector(1000, 1000, 100), Range));
	TestTrue(TEXT("exactly at range is still inside"), IsWithinLootRange(Bag, FVector(1200, 1000, 100), Range));
	TestFalse(TEXT("one step past range closes"), IsWithinLootRange(Bag, FVector(1201, 1000, 100), Range));
	TestTrue(TEXT("height is ignored, like the server"), IsWithinLootRange(Bag, FVector(1000, 1100, 900), Range));
	TestFalse(TEXT("diagonal past range"), IsWithinLootRange(Bag, FVector(1150, 1150, 100), Range));

	return true;
}

// ── AC-UI-05 — "전체 획득" tally ──────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1ItemLootBatchTest, "S1.Item.Logic.LootBatch_MixedResults_ClosesOnlyWhenEverythingLeft", GS1ItemTestFlags)
bool FS1ItemLootBatchTest::RunTest(const FString& Parameters)
{
	{
		FS1LootBatch Batch;
		Batch.Expected = 3;
		Batch.Record(ES1ItemResult::Ok);
		Batch.Record(ES1ItemResult::Ok);
		TestFalse(TEXT("2 of 3 is not complete"), Batch.IsComplete());
		TestFalse(TEXT("incomplete never closes"), Batch.ShouldClose());
		Batch.Record(ES1ItemResult::Ok);
		TestTrue(TEXT("3 of 3 is complete"), Batch.IsComplete());
		TestTrue(TEXT("all taken closes"), Batch.ShouldClose());
	}
	{
		FS1LootBatch Batch;
		Batch.Expected = 3;
		Batch.Record(ES1ItemResult::Ok);
		Batch.Record(ES1ItemResult::InventoryFull);
		Batch.Record(ES1ItemResult::InventoryFull);
		TestEqual(TEXT("two did not fit"), Batch.Full, 2);
		TestFalse(TEXT("items left behind keep it open"), Batch.ShouldClose());
	}
	{
		// Someone else took one while ours were in flight. It left the bag all the same.
		FS1LootBatch Batch;
		Batch.Expected = 2;
		Batch.Record(ES1ItemResult::Ok);
		Batch.Record(ES1ItemResult::NotFound);
		TestEqual(TEXT("a lost race is not counted as full"), Batch.Full, 0);
		TestTrue(TEXT("empty after a lost race closes"), Batch.ShouldClose());
	}
	{
		FS1LootBatch Batch;
		Batch.Expected = 2;
		Batch.Record(ES1ItemResult::Ok);
		Batch.Record(ES1ItemResult::InvalidState);
		TestEqual(TEXT("too far is kept, not full"), Batch.Kept, 1);
		TestEqual(TEXT("too far is not full"), Batch.Full, 0);
		TestFalse(TEXT("an item still in the bag keeps it open"), Batch.ShouldClose());
	}

	return true;
}

// ── Loot window countdown ────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1ItemRemainingMsTest, "S1.Item.Logic.RemainingMs_ClockAfterReceipt_CountsDownToZero", GS1ItemTestFlags)
bool FS1ItemRemainingMsTest::RunTest(const FString& Parameters)
{
	using S1ItemLogic::RemainingMs;

	FS1BagView Bag;
	Bag.ReceivedAtSeconds = 100.0;

	Bag.RemainingMsAtReceive = 0;
	TestEqual(TEXT("no timer stays 0 (chest)"), static_cast<int64>(RemainingMs(Bag, 105.0)), static_cast<int64>(0));

	Bag.RemainingMsAtReceive = 60000;
	TestEqual(TEXT("counts down from receipt"), static_cast<int64>(RemainingMs(Bag, 110.0)), static_cast<int64>(50000));
	TestEqual(TEXT("never below 0"), static_cast<int64>(RemainingMs(Bag, 200.0)), static_cast<int64>(0));
	TestEqual(TEXT("a clock before receipt does not add time"), static_cast<int64>(RemainingMs(Bag, 90.0)), static_cast<int64>(60000));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
