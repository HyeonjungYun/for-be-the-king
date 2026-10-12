// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/UI/S1ItemUIStyle.h"
#include "Game/Item/S1ItemCatalog.h"
#include "Game/Item/S1ItemSubsystem.h"
#include "Styling/CoreStyle.h"

namespace S1ItemUIStyle
{
	FLinearColor PanelBackground()	{ return FLinearColor(FColor::FromHex(TEXT("1A1A19EB"))); }
	FLinearColor CellBackground()	{ return FLinearColor(FColor::FromHex(TEXT("262624FF"))); }
	FLinearColor CellHover()		{ return FLinearColor(FColor::FromHex(TEXT("383835FF"))); }
	FLinearColor Divider()			{ return FLinearColor(FColor::FromHex(TEXT("383835FF"))); }
	FLinearColor TextPrimary()		{ return FLinearColor(FColor::FromHex(TEXT("FFFFFFFF"))); }
	FLinearColor TextSecondary()	{ return FLinearColor(FColor::FromHex(TEXT("C3C2B7FF"))); }
	FLinearColor TextTertiary()		{ return FLinearColor(FColor::FromHex(TEXT("898781FF"))); }
	FLinearColor TextShadowed()		{ return FLinearColor(FColor::FromHex(TEXT("55544FFF"))); }
	FLinearColor BetrayerRed()		{ return FLinearColor(FColor::FromHex(TEXT("E54B3CFF"))); }		// party-betrayal R6 · R10

	FLinearColor GradeColor(ES1ItemGrade Grade)
	{
		switch (Grade)
		{
		case ES1ItemGrade::Common:		return FLinearColor(FColor::FromHex(TEXT("9AA0A6FF")));	// art-bible
		case ES1ItemGrade::Rare:		return FLinearColor(FColor::FromHex(TEXT("C9A227FF")));	// TEMP
		case ES1ItemGrade::Epic:		return FLinearColor(FColor::FromHex(TEXT("A06CD5FF")));	// TEMP
		case ES1ItemGrade::Legendary:	return FLinearColor(FColor::FromHex(TEXT("E66767FF")));	// art-bible
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
			return FText::Format(Text(TEXT("label.unknown_item")), FText::AsNumber(TypeId));

		return Keyed(Type->NameKey);
	}

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
