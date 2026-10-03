// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Game/Item/S1ItemTypes.h"

/**
 * Item UI decisions that need no world, no packets and no widgets.
 *
 * Split out of the subsystem so the [unit] rows of design/ux/item-ui.md § Acceptance
 * Criteria run headless (Tests/S1ItemLogicTests.cpp). The subsystem calls these and never
 * reimplements them.
 */
namespace S1ItemLogic
{
	/** AC-UI-02. Which notice a result produces, given the request that caused it. */
	ES1ItemNotice NoticeFor(ES1ItemResult Result, ES1ItemAction Action);

	/**
	 * AC-UI-03. What a discard click does.
	 *
	 * bEquippedThisSession: the item was equipped at some point while this client process
	 * lived. The GDD asks for "equipped last session", which the server does not track —
	 * design/ux/item-ui.md § Data Requirements.
	 */
	ES1DiscardRule DiscardRuleFor(const FS1ItemView& Item, bool bEquippedThisSession);

	/**
	 * AC-UI-04. The bag F opens: nearest within RangeCm (inclusive), or 0.
	 *
	 * Measured on the floor plane, like the server's IsInLootRange. Bags whose spawn has
	 * not arrived yet have no location and are skipped. Ties go to the lower id so the
	 * same press always picks the same bag.
	 */
	uint64 FindNearestBag(const TMap<uint64, FS1BagView>& Bags, const FVector& From, float RangeCm);

	/** Remaining time for the loot window. Display only — the server decides expiry. */
	uint64 RemainingMs(const FS1BagView& Bag, double NowSeconds);
}

/**
 * AC-UI-05. One "전체 획득" press is N separate C_LOOTs. This collects their results.
 */
struct FS1LootBatch
{
	int32 Expected = 0;
	int32 Received = 0;
	int32 Full = 0;		// ITEM_INVENTORY_FULL — the item stayed in the bag
	int32 Kept = 0;		// Any other failure that left the item in the bag (too far, ...)

	void Record(ES1ItemResult Result);

	bool IsComplete() const { return Received >= Expected; }

	/** Every item left the bag — taken by us or already gone to someone else. Close the window. */
	bool ShouldClose() const { return IsComplete() && Full == 0 && Kept == 0; }
};
