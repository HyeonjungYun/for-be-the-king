// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Game/Item/S1ItemTypes.h"
#include "Game/Item/S1ItemCatalog.h"

/** A skill and the key it lands on. */
struct FS1KeySkill
{
	int32	KeySlot = 0;	// Protocol::EquipSlot used as a key — 1 Shift · 2 Q · 3 E · 4 R · 5 Z · 6 X
	uint32	SkillId = 0;
};

/** One key that reads differently after a swap. SkillId 0 = the key goes empty. */
struct FS1KeyChange
{
	int32	KeySlot = 0;
	uint32	Before = 0;
	uint32	After = 0;
};

/** One stat line of the detail pane. */
struct FS1StatDelta
{
	FName	Stat;
	float	Before = 0.f;
	float	After = 0.f;

	float Delta() const { return After - Before; }
};

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

	/**
	 * Same measure as FindNearestBag — floor plane, inclusive. The loot window closes itself the
	 * moment this turns false, because the server drops out-of-range viewers without telling us.
	 */
	bool IsWithinLootRange(const FVector& BagLocation, const FVector& From, float RangeCm);

	/** Remaining time for the loot window. Display only — the server decides expiry. */
	uint64 RemainingMs(const FS1BagView& Bag, double NowSeconds);

	/**
	 * Which key each of the item's skills lands on when worn.
	 *
	 * The server leaves SkillInfo.slot at 0 for item skills — it does not know an item's part
	 * before the S-1 loader exists. So the rule is part + order: the first skill goes to the
	 * part's key (weapon → Shift), the second to Q (weapons only). The roll rules guarantee a
	 * missing first skill means no second one, so the order never shifts. A non-zero slot
	 * from the server wins.
	 */
	TArray<FS1KeySkill> KeysFor(const FS1ItemTypeDef& Type, const FS1ItemView& Item);

	/**
	 * AC-UI-01. Keys that change when Candidate replaces Equipped (the item worn in the same
	 * part, or null). Weapons always report Shift and Q. Keys that stay the same are left out.
	 */
	TArray<FS1KeyChange> KeyChanges(const FS1ItemTypeDef& Type, const FS1ItemView& Candidate, const FS1ItemView* Equipped);

	/**
	 * flat_floor + base_stat × grade multiplier (GDD D2-b). GradeMultipliers is indexed by
	 * grade − 1 (Common first). The level term is 🔒T3 — MVP items are level 0.
	 */
	float StatValue(const FS1ItemStat& Stat, ES1ItemGrade Grade, TArrayView<const float> GradeMultipliers);

	/** Per-item cap on percent_ms_bonus — the server's EquipStats::MS_BONUS_PER_ITEM_MAX (item-equipment-system.md § 합산 표). */
	inline constexpr float MoveSpeedBonusPerItemMax = 0.15f;

	/**
	 * One worn item's share of percent_ms_bonus, capped per item. The wearer's bonus is the sum over the parts,
	 * applied once: speed = 340 × (1 + Σ) × (1 − slow) — the same order as the server's Player::ComputeMoveSpeed,
	 * so the client never walks faster than the server's ceiling (a faster client is a rejected move).
	 */
	float MoveSpeedBonus(const FS1ItemTypeDef& Type, ES1ItemGrade Grade, TArrayView<const float> GradeMultipliers);

	/**
	 * Before/after per stat for the detail pane — the union of both items' stats, the
	 * candidate's order first. EquippedType null = nothing worn in that part.
	 */
	TArray<FS1StatDelta> StatDeltas(const FS1ItemTypeDef& CandidateType, ES1ItemGrade CandidateGrade,
		const FS1ItemTypeDef* EquippedType, ES1ItemGrade EquippedGrade, TArrayView<const float> GradeMultipliers);
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
