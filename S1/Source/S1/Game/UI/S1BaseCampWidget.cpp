// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/UI/S1BaseCampWidget.h"
#include "Game/UI/S1EntryMapWidget.h"
#include "Game/UI/S1ItemDetailWidget.h"
#include "Game/UI/S1ItemSlotWidget.h"
#include "Game/UI/S1ItemUIComponent.h"
#include "Game/UI/S1ItemUIStyle.h"
#include "Game/BaseCamp/S1BaseCampSubsystem.h"
#include "Game/Item/S1ItemLogic.h"
#include "Game/Item/S1ItemSubsystem.h"
#include "Game/S1SkillTable.h"
#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateColorBrush.h"
#include "Components/Border.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/UniformGridPanel.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/GameInstance.h"
#include "TimerManager.h"

using namespace S1ItemUIStyle;

namespace S1BaseCampLayout
{
	/** Worn parts, left to right. The weapon cell covers two keys (Shift, Q). */
	constexpr int32 StripParts[] = { 1, 3, 4, 5, 6 };

	/** § Layout Zones — 보관 | 나 | 반입 */
	constexpr float ColumnWeights[] = { 0.28f, 0.44f, 0.28f };

	constexpr float ScreenPadding = 24.f;

	/** Vault capacity — Server ServerPacketHandler.cpp VAULT_SLOT_COUNT (entities.yaml vault_slots). */
	constexpr int32 VaultCapacity = 25;

	TArray<int32> KeysOfPart(int32 Part)
	{
		return Part == 1 ? TArray<int32>{ 1, 2 } : TArray<int32>{ Part };
	}

	/** Name order, or skill-name order with skill-less items last. Ties go to the instance id so nothing jumps. */
	void SortForGrid(TArray<const FS1ItemView*>& Items, const FS1ItemCatalog& Catalog, bool bBySkill)
	{
		auto SortKey = [&Catalog, bBySkill](const FS1ItemView& Item) -> FString
		{
			const FS1ItemTypeDef* Type = Catalog.Find(Item.ItemTypeId);
			if (bBySkill)
			{
				const TArray<FS1KeySkill> Keys = Type != nullptr ? S1ItemLogic::KeysFor(*Type, Item) : TArray<FS1KeySkill>();
				return Keys.Num() > 0 ? SkillName(Keys[0].SkillId).ToString() : FString::ChrN(1, static_cast<TCHAR>(0xFFFF));
			}
			return ItemName(Type, Item.ItemTypeId).ToString();
		};

		Items.Sort([&SortKey](const FS1ItemView& A, const FS1ItemView& B)
		{
			const FString KeyA = SortKey(A);
			const FString KeyB = SortKey(B);
			return KeyA != KeyB ? KeyA < KeyB : A.InstanceId < B.InstanceId;
		});
	}

	// ── 스킬 제원 — atomic attributes as the player reads them (skill-system.md B1a) ──

	/** "4m", "2.5m" — centimetres from the skill table, one decimal at most. */
	FText Meters(float Cm)
	{
		FNumberFormattingOptions Options;
		Options.MaximumFractionalDigits = 1;
		return FText::AsNumber(Cm / 100.f, &Options);
	}

	/** "8", "0.3", "0.06" — milliseconds as seconds. */
	FText Seconds(uint32 Ms)
	{
		FNumberFormattingOptions Options;
		Options.MaximumFractionalDigits = 2;
		return FText::AsNumber(Ms / 1000.f, &Options);
	}

	/** How the key is used — the one thing a player must know before the fight (skill-system.md aim_type). */
	FText AimLabel(ES1SkillAimType Aim)
	{
		switch (Aim)
		{
		case ES1SkillAimType::SelfArea:		return Text(TEXT("aim.self_area"));
		case ES1SkillAimType::Target:		return Text(TEXT("aim.target"));
		case ES1SkillAimType::Direction:	return Text(TEXT("aim.direction"));
		case ES1SkillAimType::Point:		return Text(TEXT("aim.point"));
		default:							return Text(TEXT("aim.none"));
		}
	}

	/**
	 * The hit shape — left out when it would mislead: a dash is "CircleSelf, radius 0" only so it
	 * has a shape at all; it hits nothing (skill-system.md R12b).
	 */
	FText ShapeLabel(const FS1SkillDef& Def)
	{
		switch (Def.Shape)
		{
		case ES1SkillShape::Single:			return Text(TEXT("shape.single"));
		case ES1SkillShape::Line:			return Text(TEXT("shape.line"));
		case ES1SkillShape::CirclePoint:	return Text(TEXT("shape.circle_point"));
		default:							return Def.RadiusCm > 0.f ? Text(TEXT("shape.circle_self")) : FText::GetEmpty();
		}
	}

	/** One line: aim · shape · reach · cast · cooldown. No derived numbers — no damage, no DPS (B1a). */
	FText AtomicLine(const FS1SkillDef& Def)
	{
		TArray<FText> Parts;
		Parts.Add(AimLabel(Def.AimType));

		const FText Shape = ShapeLabel(Def);
		if (Shape.IsEmpty() == false)
			Parts.Add(Shape);

		// A dash's range is its travel — say "이동", not "사거리".
		if (Def.HasMovement())
			Parts.Add(FText::Format(Text(TEXT("atomic.move")), Meters(Def.MoveDistCm)));
		else if (Def.RangeCm > 0.f)
			Parts.Add(FText::Format(Text(TEXT("atomic.range")), Meters(Def.RangeCm)));

		if (Def.WidthCm > 0.f)
			Parts.Add(FText::Format(Text(TEXT("atomic.width")), Meters(Def.WidthCm)));
		if (Def.RadiusCm > 0.f)
			Parts.Add(FText::Format(Text(TEXT("atomic.radius")), Meters(Def.RadiusCm)));

		Parts.Add(Def.CastMs == 0 ? Text(TEXT("atomic.cast_instant")) : FText::Format(Text(TEXT("atomic.cast")), Seconds(Def.CastMs)));
		Parts.Add(FText::Format(Text(TEXT("atomic.cooldown")), Seconds(Def.CooldownMs)));

		return FText::Join(FText::AsCultureInvariant(TEXT(" · ")), Parts);
	}
}

US1ItemSubsystem* US1BaseCampWidget::GetItems() const
{
	UGameInstance* GameInstance = GetGameInstance();
	return GameInstance != nullptr ? GameInstance->GetSubsystem<US1ItemSubsystem>() : nullptr;
}

US1BaseCampSubsystem* US1BaseCampWidget::GetBaseCamp() const
{
	UGameInstance* GameInstance = GetGameInstance();
	return GameInstance != nullptr ? GameInstance->GetSubsystem<US1BaseCampSubsystem>() : nullptr;
}

FText US1BaseCampWidget::KeyName(int32 KeySlot) const
{
	// Our pawn may be gone in base camp — the resolver falls back to the fixed names then.
	return US1ItemUIComponent::KeyNameForController(GetOwningPlayer(), KeySlot);
}

// ── Build ────────────────────────────────────────────────────────────────

void US1BaseCampWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	auto MakeText = [this](int32 Size, const FLinearColor& Color) -> UTextBlock*
	{
		UTextBlock* Text = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Text->SetFont(Font(Size));
		Text->SetColorAndOpacity(FSlateColor(Color));
		return Text;
	};

	// A notice line whose height is always reserved, so nothing below it jumps when it fills.
	auto MakeNotice = [this, &MakeText](UVerticalBox* Column)
	{
		USizeBox* Box = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		Box->SetHeightOverride(NoticeLineHeight);
		UTextBlock* Text = MakeText(FontBody, TextPrimary());
		Text->SetVisibility(ESlateVisibility::HitTestInvisible);
		Box->SetContent(Text);
		if (UVerticalBoxSlot* BoxSlot = Column->AddChildToVerticalBox(Box))
			BoxSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));
		Notices.Add(Text);
	};

	auto MakeGrid = [this](UVerticalBox* Column, int32 Cells, TArray<TObjectPtr<US1ItemSlotWidget>>& OutSlots)
	{
		UUniformGridPanel* Grid = WidgetTree->ConstructWidget<UUniformGridPanel>(UUniformGridPanel::StaticClass());
		Grid->SetSlotPadding(FMargin(CellGap * 0.5f));
		for (int32 i = 0; i < Cells; i++)
		{
			US1ItemSlotWidget* Cell = CreateWidget<US1ItemSlotWidget>(this, US1ItemSlotWidget::StaticClass());
			Cell->SetCellSize(GridCellSize);
			Cell->OnClicked.BindUObject(this, &US1BaseCampWidget::HandleSlotClicked);
			Cell->OnHovered.BindUObject(this, &US1BaseCampWidget::HandleSlotHovered);
			Grid->AddChildToUniformGrid(Cell, i / GridColumns, i % GridColumns);
			OutSlots.Add(Cell);
		}
		if (UVerticalBoxSlot* GridSlot = Column->AddChildToVerticalBox(Grid))
			GridSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 12.f));
	};

	auto MakeHeader = [this, &MakeText](UVerticalBox* Column, TObjectPtr<UTextBlock>& OutHeader, TObjectPtr<UTextBlock>& OutSort,
		TObjectPtr<UEditableTextBox>& OutFilter)
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		OutHeader = MakeText(FontEmphasis, TextPrimary());
		OutSort = MakeText(FontSmall, TextTertiary());
		if (UHorizontalBoxSlot* HeaderSlot = Row->AddChildToHorizontalBox(OutHeader))
			HeaderSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		if (UHorizontalBoxSlot* SortSlot = Row->AddChildToHorizontalBox(OutSort))
			SortSlot->SetVerticalAlignment(VAlign_Center);
		if (UVerticalBoxSlot* RowSlot = Column->AddChildToVerticalBox(Row))
			RowSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));

		// Skill-name filter — which of the near-identical 환도 carries 짓쳐들기 is otherwise a hover-by-hover
		// search (equipment-skill-binding.md § 인벤토리 — 알려진 부담). Display only: nothing is sent.
		OutFilter = WidgetTree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass());
		const FSlateColorBrush Plain(FLinearColor::White);
		FEditableTextBoxStyle Style = OutFilter->GetWidgetStyle();
		Style
			.SetFont(Font(FontBody))
			.SetForegroundColor(FSlateColor(TextPrimary()))
			.SetBackgroundColor(FSlateColor(CellBackground()))
			.SetBackgroundImageNormal(Plain)
			.SetBackgroundImageHovered(Plain)
			.SetBackgroundImageFocused(Plain)
			.SetPadding(FMargin(6.f, 3.f));
		OutFilter->SetWidgetStyle(Style);
		OutFilter->SetHintText(Text(TEXT("label.filter_hint")));
		if (UVerticalBoxSlot* FilterSlot = Column->AddChildToVerticalBox(OutFilter))
			FilterSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));
	};

	// ── The whole screen — base camp has nothing behind it worth seeing ──
	UBorder* Background = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	// Opaque: the dungeon HUD panels may let the world show through, but in base camp there is no
	// world to show — after a death the camera points at the sky.
	FLinearColor Opaque = PanelBackground();
	Opaque.A = 1.f;
	Background->SetBrushColor(Opaque);
	Background->SetPadding(FMargin(S1BaseCampLayout::ScreenPadding));
	WidgetTree->RootWidget = Background;

	UHorizontalBox* Columns = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Background->SetContent(Columns);

	UVerticalBox* Column[3];
	for (int32 i = 0; i < 3; i++)
	{
		Column[i] = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
		if (UHorizontalBoxSlot* ColumnSlot = Columns->AddChildToHorizontalBox(Column[i]))
		{
			FSlateChildSize Size(ESlateSizeRule::Fill);
			Size.Value = S1BaseCampLayout::ColumnWeights[i];
			ColumnSlot->SetSize(Size);
			ColumnSlot->SetPadding(FMargin(i == 0 ? 0.f : S1BaseCampLayout::ScreenPadding * 0.5f, 0.f, i == 2 ? 0.f : S1BaseCampLayout::ScreenPadding * 0.5f, 0.f));
		}
	}

	// ── B-L 보관 ──
	MakeHeader(Column[0], VaultHeader, VaultSort, VaultFilter);
	VaultFilter->OnTextChanged.AddDynamic(this, &US1BaseCampWidget::HandleVaultFilterChanged);
	MakeNotice(Column[0]);
	MakeGrid(Column[0], S1BaseCampLayout::VaultCapacity, VaultSlots);

	// ── B-C 나 — keys first: this screen is choosing keys, not gear (SK:1463) ──
	MakeNotice(Column[1]);

	UHorizontalBox* Strip = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	for (int32 Part : S1BaseCampLayout::StripParts)
	{
		UVerticalBox* Cell = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());

		US1ItemSlotWidget* PartSlot = CreateWidget<US1ItemSlotWidget>(this, US1ItemSlotWidget::StaticClass());
		PartSlot->SetCellSize(GridCellSize);
		PartSlot->OnClicked.BindUObject(this, &US1BaseCampWidget::HandleSlotClicked);
		PartSlot->OnHovered.BindUObject(this, &US1BaseCampWidget::HandleSlotHovered);
		Cell->AddChildToVerticalBox(PartSlot);

		// Under the cell: the worn item's archetype, or the part's name when nothing is worn (§ Information Hierarchy ②).
		UTextBlock* PartLine = MakeText(FontSmall, TextTertiary());
		Cell->AddChildToVerticalBox(PartLine);
		StripPartLabels.Add(PartLine);

		for (int32 Key : S1BaseCampLayout::KeysOfPart(Part))
		{
			UTextBlock* KeyLine = MakeText(FontSmall, TextSecondary());
			KeyLine->SetAutoWrapText(true);
			Cell->AddChildToVerticalBox(KeyLine);

			StripKeys.Add(KeyLine);
			StripKeySlots.Add(Key);
		}

		if (UHorizontalBoxSlot* CellSlot = Strip->AddChildToHorizontalBox(Cell))
		{
			CellSlot->SetPadding(FMargin(0.f, 0.f, CellGap * 2.f, 0.f));
			CellSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		}

		StripSlots.Add(PartSlot);
	}
	if (UVerticalBoxSlot* StripSlot = Column[1]->AddChildToVerticalBox(Strip))
		StripSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 16.f));

	// 장비 합산 — named so it does not read as the final combat value (2026-10-02 decision)
	TotalsTitle = MakeText(FontEmphasis, TextPrimary());
	TotalsTitle->SetText(Text(TEXT("label.equip_total")));
	if (UVerticalBoxSlot* TitleSlot = Column[1]->AddChildToVerticalBox(TotalsTitle))
		TitleSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));

	TotalLines = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	if (UVerticalBoxSlot* LinesSlot = Column[1]->AddChildToVerticalBox(TotalLines))
		LinesSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 16.f));

	// 스킬 제원 — how each key is operated: aim, reach, cast, cooldown (skill-system.md B1a "원자 속성은 보인다").
	// The minimum ahead of design/ux/loadout.md, which owns the full panel.
	UTextBlock* AtomicTitle = MakeText(FontEmphasis, TextPrimary());
	AtomicTitle->SetText(Text(TEXT("label.atomic")));
	if (UVerticalBoxSlot* AtomicTitleSlot = Column[1]->AddChildToVerticalBox(AtomicTitle))
		AtomicTitleSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));

	AtomicLines = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	if (UVerticalBoxSlot* AtomicSlot = Column[1]->AddChildToVerticalBox(AtomicLines))
		AtomicSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 16.f));

	Detail = CreateWidget<US1ItemDetailWidget>(this, US1ItemDetailWidget::StaticClass());
	Detail->SetKeyNameResolver([this](int32 KeySlot) { return KeyName(KeySlot); });
	if (UVerticalBoxSlot* DetailSlot = Column[1]->AddChildToVerticalBox(Detail))
		DetailSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	Detail->SetClipping(EWidgetClipping::ClipToBounds);

	// ── B-R 반입 — and [입장] under it, so "what I take" is the last thing read (§ Layout Zones) ──
	MakeHeader(Column[2], CarriedHeader, CarriedSort, CarriedFilter);
	CarriedFilter->OnTextChanged.AddDynamic(this, &US1BaseCampWidget::HandleCarriedFilterChanged);
	MakeNotice(Column[2]);

	const US1ItemSubsystem* Items = GetItems();
	MakeGrid(Column[2], Items != nullptr ? Items->GetCarriedCapacity() : 25, CarriedSlots);

	// The server dresses a character with nothing at all - worn, carried and vault empty - in the starter kit
	// on entry (account-login-mvp Core Rule 8). Said here so entering empty-handed is not a surprise either way
	StarterNotice = MakeText(FontSmall, TextSecondary());
	StarterNotice->SetText(Text(TEXT("label.starter_kit")));
	StarterNotice->SetAutoWrapText(true);
	StarterNotice->SetVisibility(ESlateVisibility::Collapsed);
	if (UVerticalBoxSlot* StarterSlot = Column[2]->AddChildToVerticalBox(StarterNotice))
		StarterSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));

	EnterButton = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	EnterButton->SetPadding(FMargin(16.f, 10.f));
	EnterButton->SetHorizontalAlignment(HAlign_Center);
	EnterText = MakeText(FontEmphasis, TextPrimary());
	EnterButton->SetContent(EnterText);
	Column[2]->AddChildToVerticalBox(EnterButton);

	Refresh();
}

void US1BaseCampWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (US1ItemSubsystem* Items = GetItems())
	{
		ItemsChangedHandle = Items->OnItemsChanged.AddUObject(this, &US1BaseCampWidget::Refresh);
		KeysChangedHandle = Items->OnKeysChanged.AddUObject(this, &US1BaseCampWidget::Refresh);
		NoticeHandle = Items->OnItemNotice.AddUObject(this, &US1BaseCampWidget::HandleNotice);
	}

	if (US1BaseCampSubsystem* BaseCamp = GetBaseCamp())
	{
		PlaceChangedHandle = BaseCamp->OnPlaceChanged.AddUObject(this, &US1BaseCampWidget::HandlePlaceChanged);
		ReadyHandle = BaseCamp->OnInventoryReady.AddUObject(this, &US1BaseCampWidget::Refresh);
		RejectedHandle = BaseCamp->OnEnterRejected.AddUObject(this, &US1BaseCampWidget::HandleEnterRejected);
	}

	// S3 opens fresh — at login and after every run. A filter is a search, not a setting: left in
	// place it would hide what came back from the run. The sort order is a preference and stays.
	VaultFilterText.Reset();
	CarriedFilterText.Reset();
	for (UEditableTextBox* Box : { VaultFilter.Get(), CarriedFilter.Get() })
	{
		if (Box != nullptr)
			Box->SetText(FText::GetEmpty());
	}

	Refresh();
}

void US1BaseCampWidget::NativeDestruct()
{
	if (US1ItemSubsystem* Items = GetItems())
	{
		Items->OnItemsChanged.Remove(ItemsChangedHandle);
		Items->OnKeysChanged.Remove(KeysChangedHandle);
		Items->OnItemNotice.Remove(NoticeHandle);
	}

	if (US1BaseCampSubsystem* BaseCamp = GetBaseCamp())
	{
		BaseCamp->OnPlaceChanged.Remove(PlaceChangedHandle);
		BaseCamp->OnInventoryReady.Remove(ReadyHandle);
		BaseCamp->OnEnterRejected.Remove(RejectedHandle);
	}

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ConfirmTimer);
		World->GetTimerManager().ClearTimer(NoticeTimer);
	}

	// S3 leaves when the run starts (or the world goes) — S4 sits on top of it and goes too
	CloseEntryMap();

	Super::NativeDestruct();
}

// ── Refresh ──────────────────────────────────────────────────────────────

bool US1BaseCampWidget::IsLocked() const
{
	const US1BaseCampSubsystem* BaseCamp = GetBaseCamp();
	return BaseCamp == nullptr || BaseCamp->IsInventoryReady() == false || BaseCamp->GetPlace() != ES1Place::BaseCamp;
}

void US1BaseCampWidget::Refresh()
{
	if (GetItems() == nullptr || VaultHeader == nullptr)
		return;

	RefreshGrid(true);
	RefreshGrid(false);
	RefreshStrip();
	RefreshTotals();
	RefreshAtomic();
	RefreshEnter();

	const US1ItemSubsystem* Items = GetItems();
	if (HoveredId != 0 && Items->FindItem(HoveredId) == nullptr)
		HoveredId = 0;
	if (ConfirmId != 0 && Items->FindItem(ConfirmId) == nullptr)
		ConfirmId = 0;

	RefreshDetail();
}

void US1BaseCampWidget::RefreshGrid(bool bVault)
{
	US1ItemSubsystem* Items = GetItems();
	const US1BaseCampSubsystem* BaseCamp = GetBaseCamp();
	const bool bReady = BaseCamp != nullptr && BaseCamp->IsInventoryReady();

	const FS1ItemCatalog& Catalog = Items->GetCatalog();
	TArray<const FS1ItemView*> List = Items->GetByState(bVault ? ES1ItemState::Vaulted : ES1ItemState::Carried);
	const int32 Capacity = bVault ? S1BaseCampLayout::VaultCapacity : Items->GetCarriedCapacity();
	const int32 Total = List.Num();

	// The filter hides cells, not items — the count and "가득 참" stay about the whole column.
	const FString& Filter = bVault ? VaultFilterText : CarriedFilterText;
	if (Filter.IsEmpty() == false)
		List = List.FilterByPredicate([&Filter](const FS1ItemView* Item) { return MatchesFilter(*Item, Filter); });

	S1BaseCampLayout::SortForGrid(List, Catalog, bVault ? bVaultBySkill : bCarriedBySkill);

	// Header: count, and "가득 참" spelled out — never color alone. Before the sync, "불러오는 중".
	FText Header;
	if (bReady == false)
	{
		Header = FText::Format(INVTEXT("{0} · {1}"), Text(bVault ? TEXT("label.vault") : TEXT("label.carry_in")), Text(TEXT("label.loading")));
	}
	else
	{
		Header = FText::Format(INVTEXT("{0} {1}/{2}"), Text(bVault ? TEXT("label.vault") : TEXT("label.carry_in")),
			FText::AsNumber(Total), FText::AsNumber(Capacity));
		if (Total >= Capacity)
			Header = FText::Format(INVTEXT("{0} · ■ {1}"), Header, Text(TEXT("label.full")));
		if (Filter.IsEmpty() == false)
			Header = FText::Format(INVTEXT("{0} · {1}"), Header, FText::Format(Text(TEXT("label.filter_count")), FText::AsNumber(List.Num())));
	}

	(bVault ? VaultHeader : CarriedHeader)->SetText(Header);
	(bVault ? VaultSort : CarriedSort)->SetText(FText::Format(INVTEXT("⇅ {0}"),
		Text((bVault ? bVaultBySkill : bCarriedBySkill) ? TEXT("label.sort_skill") : TEXT("label.sort_name"))));

	TArray<TObjectPtr<US1ItemSlotWidget>>& Cells = bVault ? VaultSlots : CarriedSlots;
	for (int32 i = 0; i < Cells.Num(); i++)
	{
		if (bReady && List.IsValidIndex(i))
		{
			const FS1ItemView& Item = *List[i];
			Cells[i]->SetItem(Item.InstanceId, Item.Grade, ItemName(Catalog.Find(Item.ItemTypeId), Item.ItemTypeId), Items->IsPending(Item.InstanceId));
		}
		else
		{
			Cells[i]->SetEmpty(false);
		}
	}
}

void US1BaseCampWidget::RefreshStrip()
{
	US1ItemSubsystem* Items = GetItems();
	const FS1ItemCatalog& Catalog = Items->GetCatalog();

	for (int32 i = 0; i < StripSlots.Num(); i++)
	{
		const FS1ItemView* Worn = Items->GetEquipped(S1BaseCampLayout::StripParts[i]);
		const FS1ItemTypeDef* Type = Worn != nullptr ? Catalog.Find(Worn->ItemTypeId) : nullptr;

		if (Worn != nullptr)
			StripSlots[i]->SetItem(Worn->InstanceId, Worn->Grade, ItemName(Type, Worn->ItemTypeId), Items->IsPending(Worn->InstanceId));
		else
			StripSlots[i]->SetEmpty(true);

		if (StripPartLabels.IsValidIndex(i))
			StripPartLabels[i]->SetText(Type != nullptr ? ArchetypeLabel(Type->Archetype) : PartLabel(S1BaseCampLayout::StripParts[i]));
	}

	const TMap<int32, uint32> KeySkills = ComputeKeySkills();

	for (int32 i = 0; i < StripKeys.Num(); i++)
	{
		const int32 Key = StripKeySlots[i];
		const uint32 SkillId = KeySkills.FindRef(Key);

		// Shadowed: the same skill already sits on an earlier key. S3 is where the reason shows (EB:537-542).
		const int32 Shadower = ShadowedBy(KeySkills, Key);

		const FText Name = SkillId != 0 ? SkillName(SkillId) : FText::AsCultureInvariant(TEXT("—"));
		FText Line = FText::Format(INVTEXT("[{0}] {1}"), KeyName(Key), Name);
		if (Shadower != 0)
			Line = FText::Format(INVTEXT("{0}\n{1}"), Line, FText::Format(Text(TEXT("label.shadowed_by")), KeyName(Shadower)));

		StripKeys[i]->SetText(Line);
		StripKeys[i]->SetColorAndOpacity(FSlateColor(Shadower != 0 ? TextShadowed() : TextSecondary()));
	}
}

TMap<int32, uint32> US1BaseCampWidget::ComputeKeySkills() const
{
	// The keys come from S_EQUIP_SYNC, which only the dungeon sends. In base camp they are worked
	// out from what is worn — the same part + order rule the server binds by (S1ItemLogic::KeysFor).
	TMap<int32, uint32> KeySkills;
	const US1ItemSubsystem* Items = GetItems();
	if (Items == nullptr)
		return KeySkills;

	for (int32 Part : S1BaseCampLayout::StripParts)
	{
		const FS1ItemView* Worn = Items->GetEquipped(Part);
		const FS1ItemTypeDef* Type = Worn != nullptr ? Items->GetCatalog().Find(Worn->ItemTypeId) : nullptr;
		if (Type == nullptr)
			continue;

		for (const FS1KeySkill& KeySkill : S1ItemLogic::KeysFor(*Type, *Worn))
			KeySkills.Add(KeySkill.KeySlot, KeySkill.SkillId);
	}
	return KeySkills;
}

int32 US1BaseCampWidget::ShadowedBy(const TMap<int32, uint32>& KeySkills, int32 Key)
{
	const uint32 SkillId = KeySkills.FindRef(Key);
	if (SkillId == 0)
		return 0;

	for (int32 Earlier = 1; Earlier < Key; Earlier++)
	{
		if (KeySkills.FindRef(Earlier) == SkillId)
			return Earlier;
	}
	return 0;
}

bool US1BaseCampWidget::MatchesFilter(const FS1ItemView& Item, const FString& Filter)
{
	if (Filter.IsEmpty())
		return true;

	return Item.Skills.ContainsByPredicate([&Filter](const FS1ItemSkill& Skill)
	{
		return Skill.SkillId != 0 && SkillName(Skill.SkillId).ToString().Contains(Filter, ESearchCase::IgnoreCase);
	});
}

void US1BaseCampWidget::RefreshAtomic()
{
	AtomicLines->ClearChildren();

	auto AddLine = [this](const FText& Line, int32 Size, const FLinearColor& Color, float Bottom)
	{
		UTextBlock* Text = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Text->SetFont(Font(Size));
		Text->SetColorAndOpacity(FSlateColor(Color));
		Text->SetAutoWrapText(true);
		Text->SetText(Line);
		if (UVerticalBoxSlot* LineSlot = AtomicLines->AddChildToVerticalBox(Text))
			LineSlot->SetPadding(FMargin(0.f, 0.f, 0.f, Bottom));
	};

	// Live keys only — a shadowed key does nothing, so its numbers would be noise (the strip already says why).
	const TMap<int32, uint32> KeySkills = ComputeKeySkills();
	int32 Shown = 0;
	for (int32 Key = 1; Key <= 6; Key++)
	{
		const uint32 SkillId = KeySkills.FindRef(Key);
		const FS1SkillDef* Def = SkillId != 0 ? FS1SkillTable::Find(SkillId) : nullptr;
		if (Def == nullptr || ShadowedBy(KeySkills, Key) != 0)
			continue;

		AddLine(FText::Format(INVTEXT("[{0}] {1}  {2}"), KeyName(Key), SkillName(SkillId), S1BaseCampLayout::AtomicLine(*Def)),
			FontBody, TextSecondary(), 0.f);
		AddLine(SkillDesc(SkillId), FontSmall, TextTertiary(), 6.f);
		Shown++;
	}

	if (Shown == 0)
		AddLine(FText::AsCultureInvariant(TEXT("—")), FontBody, TextTertiary(), 0.f);
}

void US1BaseCampWidget::HandleVaultFilterChanged(const FText& InText)
{
	VaultFilterText = InText.ToString().TrimStartAndEnd();
	if (GetItems() != nullptr && VaultHeader != nullptr)
		RefreshGrid(true);
}

void US1BaseCampWidget::HandleCarriedFilterChanged(const FText& InText)
{
	CarriedFilterText = InText.ToString().TrimStartAndEnd();
	if (GetItems() != nullptr && VaultHeader != nullptr)
		RefreshGrid(false);
}

void US1BaseCampWidget::RefreshTotals()
{
	US1ItemSubsystem* Items = GetItems();
	TotalLines->ClearChildren();

	// Stats in the order they first appear across the parts, summed over everything worn.
	TArray<FName> Order;
	TMap<FName, float> Sums;
	for (int32 Part : S1BaseCampLayout::StripParts)
	{
		const FS1ItemView* Worn = Items->GetEquipped(Part);
		const FS1ItemTypeDef* Type = Worn != nullptr ? Items->GetCatalog().Find(Worn->ItemTypeId) : nullptr;
		if (Type == nullptr)
			continue;

		for (const FS1ItemStat& Stat : Type->Stats)
		{
			if (Sums.Contains(Stat.Stat) == false)
				Order.Add(Stat.Stat);

			Sums.FindOrAdd(Stat.Stat) += S1ItemLogic::StatValue(Stat, Worn->Grade, Items->GetGradeMultipliers());
		}
	}

	auto AddLine = [this](const FText& Line, const FLinearColor& Color)
	{
		UTextBlock* Text = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Text->SetFont(Font(FontBody));
		Text->SetColorAndOpacity(FSlateColor(Color));
		Text->SetText(Line);
		TotalLines->AddChildToVerticalBox(Text);
	};

	if (Order.Num() == 0)
	{
		AddLine(FText::AsCultureInvariant(TEXT("—")), TextTertiary());
		return;
	}

	for (const FName& Stat : Order)
		AddLine(FText::Format(INVTEXT("{0}  {1}"), StatLabel(Stat), StatValueText(Stat, Sums[Stat])), TextSecondary());
}

void US1BaseCampWidget::RefreshEnter()
{
	const US1BaseCampSubsystem* BaseCamp = GetBaseCamp();
	const bool bEntering = BaseCamp != nullptr && BaseCamp->GetPlace() == ES1Place::Entering;
	const bool bCanEnter = IsLocked() == false;

	// Empty-handed entry is normal (GC:644-646) — only "not loaded yet" and "already going" disable it.
	EnterText->SetText(Text(bEntering ? TEXT("label.entering") : TEXT("label.enter")));
	EnterText->SetColorAndOpacity(FSlateColor(bCanEnter ? TextPrimary() : TextShadowed()));
	EnterButton->SetBrushColor(bCanEnter ? CellHover() : CellBackground());

	// Same condition the server checks (ServerPacketHandler LoadPlayerItems ownedRows == 0) - only once the
	// base-camp sync is in, so a half-loaded S3 never promises a kit the server will not give
	const US1ItemSubsystem* Items = GetItems();
	const bool bOwnsNothing = Items != nullptr
		&& Items->GetByState(ES1ItemState::Vaulted).Num() == 0
		&& Items->GetByState(ES1ItemState::Carried).Num() == 0
		&& Items->GetByState(ES1ItemState::Equipped).Num() == 0;
	const bool bReady = BaseCamp != nullptr && BaseCamp->IsInventoryReady();
	StarterNotice->SetVisibility(bReady && bOwnsNothing ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
}

void US1BaseCampWidget::RefreshDetail()
{
	US1ItemSubsystem* Items = GetItems();
	if (Items == nullptr || Detail == nullptr)
		return;

	if (ConfirmId != 0)
	{
		if (const FS1ItemView* Item = Items->FindItem(ConfirmId))
		{
			Detail->ShowDiscardConfirm(*Items, *Item);
			return;
		}
	}

	const FS1ItemView* Item = HoveredId != 0 ? Items->FindItem(HoveredId) : nullptr;
	if (Item == nullptr)
	{
		Detail->Clear();
		return;
	}

	const FS1ItemTypeDef* Type = Items->GetCatalog().Find(Item->ItemTypeId);
	const FS1ItemView* Worn = Type != nullptr ? Items->GetEquipped(Type->EquipSlot) : nullptr;
	Detail->ShowItem(*Items, *Item, Worn);
}

// ── Input ────────────────────────────────────────────────────────────────

FReply US1BaseCampWidget::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	const FVector2D At = InMouseEvent.GetScreenSpacePosition();
	auto IsUnder = [&At](const UWidget* Widget) { return Widget != nullptr && Widget->GetCachedGeometry().IsUnderLocation(At); };

	if (InMouseEvent.GetEffectingButton() == EKeys::LeftMouseButton && IsUnder(EnterButton))
	{
		// [입장] — no shortcut on purpose (§ Interaction Map: 실수 방지). It opens S4, the entry-point map;
		// C_ENTER_GAME goes out from there once a point or [무작위] is picked (design/ux/entry-point.md)
		if (IsLocked() == false)
		{
			CancelConfirm();
			ClearNotices();
			OpenEntryMap();
		}
	}
	else if (IsUnder(VaultSort))
	{
		bVaultBySkill = !bVaultBySkill;
		RefreshGrid(true);
	}
	else if (IsUnder(CarriedSort))
	{
		bCarriedBySkill = !bCarriedBySkill;
		RefreshGrid(false);
	}
	else
	{
		CancelConfirm();
	}

	return FReply::Handled();
}

void US1BaseCampWidget::HandleSlotHovered(uint64 InstanceId)
{
	HoveredId = InstanceId;
	RefreshDetail();
}

void US1BaseCampWidget::HandleSlotClicked(uint64 InstanceId, FKey Button, bool bCtrl)
{
	US1ItemSubsystem* Items = GetItems();
	if (Items == nullptr || IsLocked())
		return;

	const FS1ItemView* Item = InstanceId != 0 ? Items->FindItem(InstanceId) : nullptr;
	if (Item == nullptr)
	{
		CancelConfirm();
		return;
	}

	// Waiting on the server — the cell is dimmed and repeat clicks send nothing (AC-UI-22).
	if (Items->IsPending(InstanceId))
		return;

	if (bCtrl && Button == EKeys::LeftMouseButton)
	{
		Discard(*Item);
		return;
	}

	if (ConfirmId != 0)
		CancelConfirm();

	const bool bLeft = Button == EKeys::LeftMouseButton;
	const bool bRight = Button == EKeys::RightMouseButton;

	switch (Item->State)
	{
	case ES1ItemState::Vaulted:
		if (bLeft)
		{
			Items->RequestStash(InstanceId, /*bToVault*/ false);
		}
		else if (bRight)
		{
			// Not silently ignored — say why (AC-UI-30, 원칙 3)
			ShowNotice(EColumn::Vault, Text(TEXT("notice.vault_equip")), InstanceId);
			ShakeSlot(InstanceId);
		}
		break;

	case ES1ItemState::Carried:
		if (bLeft)
		{
			Items->RequestStash(InstanceId, /*bToVault*/ true);
		}
		else if (bRight)
		{
			const FS1ItemTypeDef* Type = Items->GetCatalog().Find(Item->ItemTypeId);
			if (Type == nullptr)
			{
				ShowNotice(EColumn::Carried, Notice(ES1ItemNotice::BadRequest, 0), InstanceId);
				ShakeSlot(InstanceId);
				return;
			}
			Items->RequestEquip(InstanceId, Type->EquipSlot);
		}
		break;

	case ES1ItemState::Equipped:
		if (bLeft || bRight)
			Items->RequestUnequip(InstanceId);
		break;

	default:
		break;
	}

	Refresh();	// Show the pending dim right away
}

void US1BaseCampWidget::Discard(const FS1ItemView& Item)
{
	US1ItemSubsystem* Items = GetItems();
	if (Items == nullptr)
		return;

	const EColumn Column = Item.State == ES1ItemState::Vaulted ? EColumn::Vault
		: Item.State == ES1ItemState::Carried ? EColumn::Carried : EColumn::Center;

	switch (Items->DiscardRuleFor(Item.InstanceId))
	{
	case ES1DiscardRule::Reject:
		CancelConfirm();
		ShowNotice(Column, Notice(ES1ItemNotice::MustUnequipFirst, 0), Item.InstanceId);
		ShakeSlot(Item.InstanceId);
		return;

	case ES1DiscardRule::Immediate:
		CancelConfirm();
		Items->RequestDrop(Item.InstanceId);
		Refresh();
		return;

	case ES1DiscardRule::Confirm:
		if (ConfirmId == Item.InstanceId)
		{
			// Second Ctrl+click on the same cell inside the window — it goes. In base camp it is gone for good.
			CancelConfirm();
			Items->RequestDrop(Item.InstanceId);
			Refresh();
			return;
		}

		ConfirmId = Item.InstanceId;
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().SetTimer(ConfirmTimer, FTimerDelegate::CreateUObject(this, &US1BaseCampWidget::CancelConfirm),
				Items->GetDiscardConfirmSeconds(), false);
		}
		RefreshDetail();
		return;
	}
}

void US1BaseCampWidget::CancelConfirm()
{
	if (ConfirmId == 0)
		return;

	ConfirmId = 0;
	if (UWorld* World = GetWorld())
		World->GetTimerManager().ClearTimer(ConfirmTimer);

	RefreshDetail();
}

// ── Model and flow events ────────────────────────────────────────────────

void US1BaseCampWidget::HandleNotice(ES1ItemNotice InNotice, uint64 InstanceId, int32 Count)
{
	const US1BaseCampSubsystem* BaseCamp = GetBaseCamp();
	if (BaseCamp == nullptr || BaseCamp->IsInBaseCamp() == false)
		return;

	// The reason goes to the column the item belongs to; a full column names itself (§ Events Fired).
	EColumn Column = EColumn::Center;
	ES1ItemNotice Shown = InNotice;

	if (InNotice == ES1ItemNotice::VaultFull)
	{
		Column = EColumn::Vault;
	}
	else if (InNotice == ES1ItemNotice::CarriedFull || InNotice == ES1ItemNotice::InventoryFull)
	{
		// In base camp the carried grid is "반입" — taking a part off into a full one says so.
		Column = EColumn::Carried;
		Shown = ES1ItemNotice::CarriedFull;
	}
	else if (const US1ItemSubsystem* Items = GetItems())
	{
		if (const FS1ItemView* Item = Items->FindItem(InstanceId))
		{
			Column = Item->State == ES1ItemState::Vaulted ? EColumn::Vault
				: Item->State == ES1ItemState::Carried ? EColumn::Carried : EColumn::Center;
		}
	}

	ShowNotice(Column, Notice(Shown, Count), InstanceId);
	ShakeSlot(InstanceId);
}

void US1BaseCampWidget::HandlePlaceChanged(ES1Place Place)
{
	if (Place == ES1Place::BaseCamp)
	{
		// Back from a run — what was hovered or being discarded belongs to the old view.
		HoveredId = 0;
		CancelConfirm();
	}

	Refresh();
}

void US1BaseCampWidget::HandleEnterRejected()
{
	// The lock is already off (place is BaseCamp again). S4 goes away and S3 says why next to the button.
	CloseEntryMap();
	ShowNotice(EColumn::Carried, Text(TEXT("notice.enter_rejected")), 0);
	Refresh();
}

void US1BaseCampWidget::OpenEntryMap()
{
	if (EntryMap == nullptr)
		EntryMap = CreateWidget<US1EntryMapWidget>(GetOwningPlayer(), US1EntryMapWidget::StaticClass());

	if (EntryMap == nullptr)
		return;

	// Over S3 (base camp sits at 20). S4 takes the keyboard for 1~9, R and Esc
	constexpr int32 EntryMapZOrder = 30;
	if (EntryMap->IsInViewport() == false)
		EntryMap->AddToViewport(EntryMapZOrder);

	EntryMap->SetKeyboardFocus();
}

void US1BaseCampWidget::CloseEntryMap()
{
	if (EntryMap != nullptr && EntryMap->IsInViewport())
		EntryMap->Close();
}

void US1BaseCampWidget::ShowNotice(EColumn Column, const FText& InText, uint64 InstanceId)
{
	ClearNotices();

	const int32 Index = static_cast<int32>(Column);
	if (Notices.IsValidIndex(Index) && Notices[Index] != nullptr)
		Notices[Index]->SetText(InText);

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(NoticeTimer, FTimerDelegate::CreateUObject(this, &US1BaseCampWidget::ClearNotices),
			NoticeSeconds, false);
	}
}

void US1BaseCampWidget::ClearNotices()
{
	for (UTextBlock* Line : Notices)
	{
		if (Line != nullptr)
			Line->SetText(FText::GetEmpty());
	}
}

void US1BaseCampWidget::ShakeSlot(uint64 InstanceId)
{
	if (InstanceId == 0)
		return;

	for (TArray<TObjectPtr<US1ItemSlotWidget>>* Cells : { &VaultSlots, &CarriedSlots, &StripSlots })
	{
		for (US1ItemSlotWidget* Cell : *Cells)
		{
			if (Cell->GetInstanceId() == InstanceId)
				Cell->Shake();
		}
	}
}
