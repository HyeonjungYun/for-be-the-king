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

		case ES1ItemResult::LonePathInParty:
			return ES1ItemNotice::LonePathInParty;
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

	bool IsWithinLootRange(const FVector& BagLocation, const FVector& From, float RangeCm)
	{
		return FVector::DistSquared2D(BagLocation, From) <= static_cast<double>(RangeCm) * RangeCm;
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

	TArray<FS1KeySkill> KeysFor(const FS1ItemTypeDef& Type, const FS1ItemView& Item)
	{
		constexpr int32 WeaponPrimaryKey = 1;	// Shift
		constexpr int32 WeaponSecondaryKey = 2;	// Q

		TArray<FS1KeySkill> Out;

		for (int32 i = 0; i < Item.Skills.Num(); i++)
		{
			const FS1ItemSkill& Skill = Item.Skills[i];
			if (Skill.SkillId == 0)
				continue;

			int32 Key = Skill.Slot;
			if (Key == 0)
			{
				if (i == 0)
					Key = Type.IsWeapon() ? WeaponPrimaryKey : Type.EquipSlot;
				else if (i == 1 && Type.IsWeapon())
					Key = WeaponSecondaryKey;
			}

			// A second skill on a non-weapon has nowhere to go.
			if (Key != 0)
				Out.Add(FS1KeySkill{ Key, Skill.SkillId });
		}

		return Out;
	}

	TArray<FS1KeyChange> KeyChanges(const FS1ItemTypeDef& Type, const FS1ItemView& Candidate, const FS1ItemView* Equipped)
	{
		TArray<int32> Keys;
		if (Type.IsWeapon())
			Keys = { 1, 2 };
		else
			Keys = { Type.EquipSlot };

		const TArray<FS1KeySkill> After = KeysFor(Type, Candidate);
		const TArray<FS1KeySkill> Before = Equipped != nullptr ? KeysFor(Type, *Equipped) : TArray<FS1KeySkill>();

		auto SkillOn = [](const TArray<FS1KeySkill>& List, int32 Key) -> uint32
		{
			const FS1KeySkill* Found = List.FindByPredicate([Key](const FS1KeySkill& K) { return K.KeySlot == Key; });
			return Found != nullptr ? Found->SkillId : 0;
		};

		TArray<FS1KeyChange> Out;
		for (int32 Key : Keys)
		{
			const uint32 BeforeId = SkillOn(Before, Key);
			const uint32 AfterId = SkillOn(After, Key);
			if (BeforeId != AfterId)
				Out.Add(FS1KeyChange{ Key, BeforeId, AfterId });
		}

		return Out;
	}

	float StatValue(const FS1ItemStat& Stat, ES1ItemGrade Grade, TArrayView<const float> GradeMultipliers, uint32 Level)
	{
		const int32 Index = static_cast<int32>(Grade) - 1;
		const float Multiplier = GradeMultipliers.IsValidIndex(Index) ? GradeMultipliers[Index] : 1.f;
		const float LevelBonus = Stat.BaseStat * ItemLevelRatePerBase * static_cast<float>(FMath::Min(Level, ItemLevelMax));

		return Stat.FlatFloor + Stat.BaseStat * Multiplier + LevelBonus;
	}

	FText LabelWithLevel(const FText& Name, uint32 Level)
	{
		return Level > 0 ? FText::Format(INVTEXT("{0} +{1}"), Name, FText::AsNumber(static_cast<int32>(Level))) : Name;
	}

	float MoveSpeedBonus(const FS1ItemTypeDef& Type, ES1ItemGrade Grade, TArrayView<const float> GradeMultipliers, uint32 Level)
	{
		static const FName PercentMs(TEXT("percent_ms_bonus"));

		float Bonus = 0.f;
		for (const FS1ItemStat& Stat : Type.Stats)
		{
			if (Stat.Stat == PercentMs)
				Bonus += StatValue(Stat, Grade, GradeMultipliers, Level);
		}
		return FMath::Min(Bonus, MoveSpeedBonusPerItemMax);
	}

	TArray<FS1StatDelta> StatDeltas(const FS1ItemTypeDef& CandidateType, ES1ItemGrade CandidateGrade,
		const FS1ItemTypeDef* EquippedType, ES1ItemGrade EquippedGrade, TArrayView<const float> GradeMultipliers,
		uint32 CandidateLevel, uint32 EquippedLevel)
	{
		TArray<FS1StatDelta> Out;

		auto FindStat = [](const FS1ItemTypeDef* Type, FName Name) -> const FS1ItemStat*
		{
			return Type != nullptr ? Type->Stats.FindByPredicate([Name](const FS1ItemStat& S) { return S.Stat == Name; }) : nullptr;
		};

		for (const FS1ItemStat& Stat : CandidateType.Stats)
		{
			FS1StatDelta Line;
			Line.Stat = Stat.Stat;
			Line.After = StatValue(Stat, CandidateGrade, GradeMultipliers, CandidateLevel);

			if (const FS1ItemStat* Worn = FindStat(EquippedType, Stat.Stat))
				Line.Before = StatValue(*Worn, EquippedGrade, GradeMultipliers, EquippedLevel);

			Out.Add(Line);
		}

		// Stats only the worn item has — swapping loses them.
		if (EquippedType != nullptr)
		{
			for (const FS1ItemStat& Worn : EquippedType->Stats)
			{
				if (FindStat(&CandidateType, Worn.Stat) != nullptr)
					continue;

				FS1StatDelta Line;
				Line.Stat = Worn.Stat;
				Line.Before = StatValue(Worn, EquippedGrade, GradeMultipliers, EquippedLevel);
				Out.Add(Line);
			}
		}

		return Out;
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
