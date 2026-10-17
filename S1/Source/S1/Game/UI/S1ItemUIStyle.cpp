// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/UI/S1ItemUIStyle.h"
#include "Game/Item/S1ItemCatalog.h"
#include "Game/Item/S1ItemLogic.h"
#include "Game/Item/S1ItemSubsystem.h"
#include "Styling/CoreStyle.h"

namespace S1ItemUIStyle
{
	// 16진 문자열은 처음 한 번만 해석한다 - 칸마다 매 프레임(NativePaint) 불린다 (2026-10-17). 값은 예전과 같다
	namespace
	{
		FLinearColor Hex(const TCHAR* Code) { return FLinearColor(FColor::FromHex(Code)); }
	}

	FLinearColor PanelBackground()	{ static const FLinearColor C = Hex(TEXT("1A1A19EB")); return C; }
	FLinearColor CellBackground()	{ static const FLinearColor C = Hex(TEXT("262624FF")); return C; }
	FLinearColor CellHover()		{ static const FLinearColor C = Hex(TEXT("383835FF")); return C; }
	FLinearColor Divider()			{ static const FLinearColor C = Hex(TEXT("383835FF")); return C; }
	FLinearColor TextPrimary()		{ static const FLinearColor C = Hex(TEXT("FFFFFFFF")); return C; }
	FLinearColor TextSecondary()	{ static const FLinearColor C = Hex(TEXT("C3C2B7FF")); return C; }
	FLinearColor TextTertiary()		{ static const FLinearColor C = Hex(TEXT("898781FF")); return C; }
	FLinearColor TextShadowed()		{ static const FLinearColor C = Hex(TEXT("55544FFF")); return C; }
	FLinearColor BetrayerRed()		{ static const FLinearColor C = Hex(TEXT("E54B3CFF")); return C; }		// party-betrayal R6 · R10

	FLinearColor GradeColor(ES1ItemGrade Grade)
	{
		static const FLinearColor Common = Hex(TEXT("9AA0A6FF"));		// art-bible
		static const FLinearColor Rare = Hex(TEXT("C9A227FF"));			// TEMP
		static const FLinearColor Epic = Hex(TEXT("A06CD5FF"));			// TEMP
		static const FLinearColor Legendary = Hex(TEXT("E66767FF"));	// art-bible
		switch (Grade)
		{
		case ES1ItemGrade::Common:		return Common;
		case ES1ItemGrade::Rare:		return Rare;
		case ES1ItemGrade::Epic:		return Epic;
		case ES1ItemGrade::Legendary:	return Legendary;
		default:						return Divider();
		}
	}

	FSlateFontInfo Font(int32 Size)
	{
		return FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), static_cast<float>(Size));
	}

	FText Text(const TCHAR* Key)
	{
		return FText::FromStringTable(US1ItemSubsystem::StringTableId, Key);
	}

	namespace
	{
		FText Keyed(const FString& Key)
		{
			return FText::FromStringTable(US1ItemSubsystem::StringTableId, Key);
		}
	}

	FText ItemName(const FS1ItemTypeDef* Type, uint32 TypeId)
	{
		if (Type == nullptr)
		{
			// Sell-only loot (2026-10-17 economy) - its own table, not the gear catalog
			const US1ItemSubsystem* Items = US1ItemSubsystem::Get();
			if (const FS1LootTypeDef* Loot = Items != nullptr ? Items->GetLoot().Find(TypeId) : nullptr)
				return Keyed(Loot->NameKey);
			return FText::Format(Text(TEXT("label.unknown_item")), FText::AsNumber(TypeId));
		}

		return Keyed(Type->NameKey);
	}

	FText SlotLabel(const FS1ItemTypeDef* Type, const FS1ItemView& Item)
	{
		// Loot shows how many are stacked in the cell - "들개 이빨 ×7"
		if (Type == nullptr && Item.StackCount > 1)
			return FText::Format(INVTEXT("{0}\n×{1}"), ItemName(Type, Item.ItemTypeId), FText::AsNumber(static_cast<int32>(Item.StackCount)));

		const FText Label = S1ItemLogic::LabelWithLevel(ItemName(Type, Item.ItemTypeId), Item.Level);
		return Item.bLonePath ? FText::Format(INVTEXT("{0}\n{1}"), Label, Text(TEXT("label.lone_tag"))) : Label;
	}

	FLinearColor LonePathPurple()			{ return FLinearColor(FColor::FromHex(TEXT("B58CFFFF"))); }

	FText GradeLabel(ES1ItemGrade Grade)		{ return Keyed(FString::Printf(TEXT("grade.%d"), static_cast<int32>(Grade))); }
	FText PartLabel(int32 EquipSlot)			{ return Keyed(FString::Printf(TEXT("part.%d"), EquipSlot)); }
	FText ArchetypeLabel(FName Archetype)		{ return Keyed(TEXT("archetype.") + Archetype.ToString()); }
	FText StatLabel(FName Stat)					{ return Keyed(TEXT("stat.") + Stat.ToString()); }
	FText SkillName(uint32 SkillId)				{ return Keyed(FString::Printf(TEXT("skill.%u"), SkillId)); }
	FText SkillDesc(uint32 SkillId)				{ return Keyed(FString::Printf(TEXT("skill.%u.desc"), SkillId)); }
	FText SkillFamily(uint32 SkillId)			{ return Keyed(FString::Printf(TEXT("skill.%u.family"), SkillId)); }

	FText Notice(ES1ItemNotice Notice, int32 Count)
	{
		switch (Notice)
		{
		case ES1ItemNotice::InventoryFull:		return Text(TEXT("notice.inventory_full"));
		case ES1ItemNotice::VaultFull:			return Text(TEXT("notice.vault_full"));
		case ES1ItemNotice::CarriedFull:		return Text(TEXT("notice.carried_full"));
		case ES1ItemNotice::NotFound:			return Text(TEXT("notice.not_found"));
		case ES1ItemNotice::MustUnequipFirst:	return Text(TEXT("notice.must_unequip"));
		case ES1ItemNotice::TooFar:				return Text(TEXT("notice.too_far"));
		case ES1ItemNotice::CannotNow:			return Text(TEXT("notice.cannot_now"));
		case ES1ItemNotice::BadRequest:			return Text(TEXT("notice.bad_request"));
		case ES1ItemNotice::RetryLater:			return Text(TEXT("notice.retry_later"));
		case ES1ItemNotice::LootPartial:		return FText::Format(Text(TEXT("notice.loot_partial")), FText::AsNumber(Count));
		case ES1ItemNotice::LonePathInParty:	return Text(TEXT("notice.lone_path_party"));
		case ES1ItemNotice::NotEnoughGold:		return Text(TEXT("notice.not_enough_gold"));
		case ES1ItemNotice::Sold:				return FText::Format(Text(TEXT("notice.sold")), FText::AsNumber(Count));
		case ES1ItemNotice::LootNotEquippable:	return Text(TEXT("notice.loot_not_equippable"));
		case ES1ItemNotice::SellEquipped:		return Text(TEXT("notice.sell_equipped"));
		default:								return FText::GetEmpty();
		}
	}

	namespace
	{
		bool IsRatio(FName Stat)
		{
			static const FName Ratios[] =
			{
				TEXT("crit_chance"), TEXT("percent_as_bonus"), TEXT("percent_ms_bonus"), TEXT("resistance"),
			};
			for (const FName& Ratio : Ratios)
			{
				if (Ratio == Stat)
					return true;
			}
			return false;
		}

		FNumberFormattingOptions Digits(int32 Max)
		{
			FNumberFormattingOptions Options;
			Options.MinimumFractionalDigits = 0;
			Options.MaximumFractionalDigits = Max;
			return Options;
		}
	}

	FText StatValueText(FName Stat, float Value)
	{
		if (IsRatio(Stat))
		{
			const FNumberFormattingOptions Options = Digits(1);
			return FText::AsPercent(Value, &Options);
		}

		const FNumberFormattingOptions Options = Digits(2);
		return FText::AsNumber(Value, &Options);
	}

	FText StatDeltaText(FName Stat, float Delta)
	{
		// The sign is not language — culture-invariant on purpose.
		const FText Magnitude = StatValueText(Stat, FMath::Abs(Delta));
		return FText::Format(Delta >= 0.f ? INVTEXT("+{0}") : INVTEXT("-{0}"), Magnitude);
	}
}
