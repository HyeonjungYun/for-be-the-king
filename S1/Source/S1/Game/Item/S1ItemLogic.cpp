// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/Item/S1ItemLogic.h"

namespace S1ItemLogic
{
	ES1ItemNotice NoticeFor(ES1ItemResult Result, ES1ItemAction Action)
	{
		switch (Result)
		{
		case ES1ItemResult::Ok:
		case ES1ItemResult::Already:
			return ES1ItemNotice::None;

		case ES1ItemResult::NotFound:
			// Losing a loot race: the slot is already emptying, and that is the feedback.
			// A text notice would make a race look like an error (2026-10-02 decision).
			return Action == ES1ItemAction::Loot ? ES1ItemNotice::None : ES1ItemNotice::NotFound;

		case ES1ItemResult::InvalidState:
			if (Action == ES1ItemAction::Drop)
				return ES1ItemNotice::MustUnequipFirst;
			if (Action == ES1ItemAction::Loot)
				return ES1ItemNotice::TooFar;
			return ES1ItemNotice::CannotNow;

		case ES1ItemResult::InventoryFull:
			if (Action == ES1ItemAction::Stash)
				return ES1ItemNotice::VaultFull;
			if (Action == ES1ItemAction::Unstash)
				return ES1ItemNotice::CarriedFull;
			return ES1ItemNotice::InventoryFull;

		case ES1ItemResult::BadRequest:
			return ES1ItemNotice::BadRequest;

		case ES1ItemResult::DbError:
			return ES1ItemNotice::RetryLater;
		}

		// A code this client does not know — a newer server. Say something rather than nothing.
		return ES1ItemNotice::CannotNow;
	}

	ES1DiscardRule DiscardRuleFor(const FS1ItemView& Item, bool bEquippedThisSession)
	{
		if (Item.State == ES1ItemState::Equipped)
			return ES1DiscardRule::Reject;

		if (Item.Grade >= ES1ItemGrade::Rare)
			return ES1DiscardRule::Confirm;

		if (bEquippedThisSession)
			return ES1DiscardRule::Confirm;

		return ES1DiscardRule::Immediate;
	}

	uint64 FindNearestBag(const TMap<uint64, FS1BagView>& Bags, const FVector& From, float RangeCm)
	{
		const double RangeSq = static_cast<double>(RangeCm) * RangeCm;

		uint64 Best = 0;
		double BestSq = TNumericLimits<double>::Max();

		for (const TPair<uint64, FS1BagView>& Pair : Bags)
		{
			if (Pair.Value.bHasLocation == false)
				continue;

			const double DistSq = FVector::DistSquared2D(Pair.Value.Location, From);
			if (DistSq > RangeSq)
				continue;

			if (DistSq < BestSq || (DistSq == BestSq && Pair.Key < Best))
			{
				Best = Pair.Key;
				BestSq = DistSq;
			}
		}

		return Best;
	}

	uint64 RemainingMs(const FS1BagView& Bag, double NowSeconds)
	{
		if (Bag.RemainingMsAtReceive == 0)
			return 0;

		const double ElapsedMs = FMath::Max(0.0, (NowSeconds - Bag.ReceivedAtSeconds) * 1000.0);
		if (ElapsedMs >= static_cast<double>(Bag.RemainingMsAtReceive))
			return 0;

		return Bag.RemainingMsAtReceive - static_cast<uint64>(ElapsedMs);
	}
}

void FS1LootBatch::Record(ES1ItemResult Result)
{
	++Received;

	switch (Result)
	{
	case ES1ItemResult::Ok:
	case ES1ItemResult::Already:
	case ES1ItemResult::NotFound:	// Someone else took it — it left the bag either way
		break;

	case ES1ItemResult::InventoryFull:
		++Full;
		break;

	default:
		++Kept;
		break;
	}
}
