// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/UI/S1ItemDetailWidget.h"
#include "Game/UI/S1ItemUIStyle.h"
#include "Game/Item/S1ItemLogic.h"
#include "Game/Item/S1ItemSubsystem.h"
#include "Blueprint/WidgetTree.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"

using namespace S1ItemUIStyle;

void US1ItemDetailWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	Lines = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	WidgetTree->RootWidget = Lines;
}

void US1ItemDetailWidget::Clear()
{
	if (Lines != nullptr)
		Lines->ClearChildren();
}

void US1ItemDetailWidget::AddLine(const FText& Text, int32 FontSize, const FLinearColor& Color)
{
	UTextBlock* Line = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Line->SetText(Text);
	Line->SetFont(Font(FontSize));
	Line->SetColorAndOpacity(FSlateColor(Color));
	Line->SetAutoWrapText(true);

	if (UVerticalBoxSlot* LineSlot = Lines->AddChildToVerticalBox(Line))
		LineSlot->SetPadding(FMargin(0.f, 1.f));
}

FText US1ItemDetailWidget::KeyLabel(int32 KeySlot) const
{
	const FText Name = KeyName ? KeyName(KeySlot) : FText::AsNumber(KeySlot);
	return FText::Format(INVTEXT("[{0}]"), Name);
}

void US1ItemDetailWidget::AddItemHeader(const US1ItemSubsystem& Items, const FS1ItemView& Item)
{
	const FS1ItemTypeDef* Type = Items.GetCatalog().Find(Item.ItemTypeId);
	if (Type == nullptr)
		return;

	// ── Skill first (EB:529) ──
	const TArray<FS1KeySkill> Keys = S1ItemLogic::KeysFor(*Type, Item);
	if (Keys.Num() == 0)
	{
		AddLine(Text(TEXT("label.no_skill")), FontEmphasis, TextSecondary());
	}

	for (const FS1KeySkill& Key : Keys)
	{
		FText Head = FText::Format(INVTEXT("{0} {1}"), KeyLabel(Key.KeySlot), SkillName(Key.SkillId));

		// A trinket takes skills from the other parts. Say which one, or it reads like a mistake (EB:531).
		if (Type->EquipSlot == 6)
			Head = FText::Format(INVTEXT("{0} · {1}"), Head, SkillFamily(Key.SkillId));

		AddLine(Head, FontEmphasis, TextPrimary());
		AddLine(SkillDesc(Key.SkillId), FontSmall, TextSecondary());
	}

	// ── Name · part · archetype · grade ──
	AddLine(FText::Format(INVTEXT("{0} · {1} · {2} · {3}"),
		ItemName(Type, Item.ItemTypeId), PartLabel(Type->EquipSlot), ArchetypeLabel(Type->Archetype), GradeLabel(Item.Grade)),
		FontBody, TextSecondary());
}

void US1ItemDetailWidget::ShowItem(const US1ItemSubsystem& Items, const FS1ItemView& Item, const FS1ItemView* Worn)
{
	Clear();
	if (Lines == nullptr)
		return;

	const FS1ItemTypeDef* Type = Items.GetCatalog().Find(Item.ItemTypeId);
	if (Type == nullptr)
	{
		AddLine(ItemName(nullptr, Item.ItemTypeId), FontEmphasis, Type == nullptr && Items.IsLoot(Item.ItemTypeId) ? TextPrimary() : TextSecondary());

		// Sell-only loot (2026-10-17 economy): what it is for and what the guild pays - "매입가 360 골드 (개당 120 × 3)"
		if (const FS1LootTypeDef* Loot = Items.GetLoot().Find(Item.ItemTypeId))
		{
			AddLine(Text(TEXT("label.loot")), FontBody, TextSecondary());
			AddLine(FText::Format(Text(TEXT("label.loot_price")), FText::AsNumber(S1Economy::SellPrice(Item, Items.GetLoot(), Items.GetEconomy())),
				FText::AsNumber(Loot->SellPrice), FText::AsNumber(static_cast<int32>(Item.StackCount))), FontBody, TextPrimary());
		}
		return;
	}

	AddItemHeader(Items, Item);

	// 강화 레벨 (item-enhance-2026-10-12 R8) - 0 이어도 보여 준다: "Lv 0 / 16" 이 강화가 있다는 안내다
	AddLine(Item.Level >= S1ItemLogic::ItemLevelMax
		? FText::Format(Text(TEXT("label.enhance_max")), FText::AsNumber(static_cast<int32>(Item.Level)))
		: FText::Format(Text(TEXT("label.enhance_level")), FText::AsNumber(static_cast<int32>(Item.Level)), FText::AsNumber(static_cast<int32>(S1ItemLogic::ItemLevelMax))),
		FontBody, TextSecondary());

	// What the guild buyer would pay for it (2026-10-17 economy) - grade base × (1 + bonus × level)
	AddLine(FText::Format(Text(TEXT("label.gear_price")), FText::AsNumber(S1Economy::SellPrice(Item, Items.GetLoot(), Items.GetEconomy()))),
		FontBody, TextSecondary());

	// 고독한 길 (2026-10-17) - the option and its price, in one place: no party while worn, stronger when outnumbered
	if (Item.bLonePath)
	{
		AddLine(Text(TEXT("label.lone_path")), FontBody, LonePathPurple());
		AddLine(Text(TEXT("label.lone_path_desc")), FontSmall, TextSecondary());
	}

	const bool bIsWorn = Item.State == ES1ItemState::Equipped;
	const FS1ItemView* Compare = bIsWorn ? nullptr : Worn;
	const FS1ItemTypeDef* CompareType = Compare != nullptr ? Items.GetCatalog().Find(Compare->ItemTypeId) : nullptr;

	// ── Stats, with the delta against what is worn ──
	const TArray<FS1StatDelta> Stats = S1ItemLogic::StatDeltas(*Type, Item.Grade,
		CompareType, Compare != nullptr ? Compare->Grade : ES1ItemGrade::None, Items.GetGradeMultipliers(),
		Item.Level, Compare != nullptr ? Compare->Level : 0);

	for (const FS1StatDelta& Stat : Stats)
	{
		if (bIsWorn)
		{
			AddLine(FText::Format(INVTEXT("{0} {1}"), StatLabel(Stat.Stat), StatValueText(Stat.Stat, Stat.After)),
				FontBody, TextPrimary());
			continue;
		}

		// The sign carries the direction, so the line reads without color (§ Accessibility).
		AddLine(FText::Format(INVTEXT("{0} {1} ({2})"), StatLabel(Stat.Stat), StatValueText(Stat.Stat, Stat.After),
			StatDeltaText(Stat.Stat, Stat.Delta())), FontBody, TextPrimary());
	}

	// ── Keys that change if this goes on ──
	if (bIsWorn == false)
	{
		const TArray<FS1KeyChange> Changes = S1ItemLogic::KeyChanges(*Type, Item, Compare);
		for (const FS1KeyChange& Change : Changes)
		{
			const FText Before = Change.Before != 0 ? SkillName(Change.Before) : FText::AsCultureInvariant(TEXT("—"));
			const FText After = Change.After != 0 ? SkillName(Change.After) : FText::AsCultureInvariant(TEXT("—"));

			AddLine(FText::Format(INVTEXT("{0} {1} {2} → {3}"),
				Text(TEXT("label.keys_changed")), KeyLabel(Change.KeySlot), Before, After), FontBody, TextSecondary());
		}
	}
}

void US1ItemDetailWidget::ShowDiscardConfirm(const US1ItemSubsystem& Items, const FS1ItemView& Item)
{
	Clear();
	if (Lines == nullptr)
		return;

	const FS1ItemTypeDef* Type = Items.GetCatalog().Find(Item.ItemTypeId);
	AddLine(ItemName(Type, Item.ItemTypeId), FontEmphasis, TextPrimary());
	AddLine(Text(TEXT("label.discard_confirm")), FontBody, TextPrimary());
}
