// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/UI/S1InventoryPanelWidget.h"
#include "Game/UI/S1ItemSlotWidget.h"
#include "Game/UI/S1ItemUIStyle.h"
#include "Game/Item/S1ItemLogic.h"
#include "Game/Item/S1ItemSubsystem.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
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

namespace
{
	/** Worn parts, left to right. The weapon cell covers two keys (Shift, Q). */
	constexpr int32 StripParts[] = { 1, 3, 4, 5, 6 };


	TArray<int32> KeysOfPart(int32 Part)
	{
		return Part == 1 ? TArray<int32>{ 1, 2 } : TArray<int32>{ Part };
	}
}

US1ItemSubsystem* US1InventoryPanelWidget::GetItems() const
{
	UGameInstance* GameInstance = GetGameInstance();
	return GameInstance != nullptr ? GameInstance->GetSubsystem<US1ItemSubsystem>() : nullptr;
}

void US1InventoryPanelWidget::SetKeyNameResolver(FS1KeyNameResolver InResolver)
{
	KeyName = InResolver;
	if (Detail != nullptr)
		Detail->SetKeyNameResolver(MoveTemp(InResolver));
}

// ── Build ────────────────────────────────────────────────────────────────

void US1InventoryPanelWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	UBorder* Background = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Background->SetBrushColor(PanelBackground());
	Background->SetPadding(FMargin(PanelPadding));

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Background->SetContent(Column);
	WidgetTree->RootWidget = Background;

	auto MakeText = [this](int32 Size, const FLinearColor& Color) -> UTextBlock*
	{
		UTextBlock* Text = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Text->SetFont(Font(Size));
		Text->SetColorAndOpacity(FSlateColor(Color));
		return Text;
	};

	// ── Header row: count on the left, sort toggle on the right ──
	UHorizontalBox* HeaderRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	HeaderText = MakeText(FontEmphasis, TextPrimary());
	SortText = MakeText(FontSmall, TextTertiary());
	if (UHorizontalBoxSlot* HeaderSlot = HeaderRow->AddChildToHorizontalBox(HeaderText))
		HeaderSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	if (UHorizontalBoxSlot* SortSlot = HeaderRow->AddChildToHorizontalBox(SortText))
		SortSlot->SetVerticalAlignment(VAlign_Center);
	if (UVerticalBoxSlot* RowSlot = Column->AddChildToVerticalBox(HeaderRow))
		RowSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));

	// ── Notice line — its height is always reserved. Collapsing it while empty made the strip
	// and grid jump down when a notice appeared, moving the cell under the cursor mid-click. ──
	USizeBox* NoticeBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	NoticeBox->SetHeightOverride(NoticeLineHeight);
	NoticeText = MakeText(FontBody, TextPrimary());
	NoticeText->SetVisibility(ESlateVisibility::HitTestInvisible);
	NoticeBox->SetContent(NoticeText);
	if (UVerticalBoxSlot* NoticeSlot = Column->AddChildToVerticalBox(NoticeBox))
		NoticeSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 2.f));

	// ── Key strip ──
	UHorizontalBox* Strip = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	for (int32 Part : StripParts)
	{
		UVerticalBox* Cell = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());

		US1ItemSlotWidget* PartSlot = CreateWidget<US1ItemSlotWidget>(this, US1ItemSlotWidget::StaticClass());
		PartSlot->SetCellSize(StripCellSize);
		PartSlot->OnClicked.BindUObject(this, &US1InventoryPanelWidget::HandleSlotClicked);
		PartSlot->OnHovered.BindUObject(this, &US1InventoryPanelWidget::HandleSlotHovered);
		Cell->AddChildToVerticalBox(PartSlot);

		// One line per key, so a Shadowed key can be dimmed on its own (the weapon has two).
		for (int32 Key : KeysOfPart(Part))
		{
			UTextBlock* KeyLine = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
			KeyLine->SetFont(Font(FontSmall));
			KeyLine->SetAutoWrapText(true);
			Cell->AddChildToVerticalBox(KeyLine);

			StripKeys.Add(KeyLine);
			StripKeySlots.Add(Key);
		}

		if (UHorizontalBoxSlot* CellSlot = Strip->AddChildToHorizontalBox(Cell))
		{
			CellSlot->SetPadding(FMargin(0.f, 0.f, CellGap, 0.f));
			CellSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		}

		StripSlots.Add(PartSlot);
	}
	if (UVerticalBoxSlot* StripSlot = Column->AddChildToVerticalBox(Strip))
		StripSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));

	// ── Carried grid ──
	UUniformGridPanel* Grid = WidgetTree->ConstructWidget<UUniformGridPanel>(UUniformGridPanel::StaticClass());
	Grid->SetSlotPadding(FMargin(CellGap * 0.5f));

	const US1ItemSubsystem* Items = GetItems();
	const int32 Capacity = Items != nullptr ? Items->GetCarriedCapacity() : 0;
	for (int32 i = 0; i < Capacity; i++)
	{
		US1ItemSlotWidget* Cell = CreateWidget<US1ItemSlotWidget>(this, US1ItemSlotWidget::StaticClass());
		Cell->SetCellSize(GridCellSize);
		Cell->OnClicked.BindUObject(this, &US1InventoryPanelWidget::HandleSlotClicked);
		Cell->OnHovered.BindUObject(this, &US1InventoryPanelWidget::HandleSlotHovered);
		Grid->AddChildToUniformGrid(Cell, i / GridColumns, i % GridColumns);
		GridSlots.Add(Cell);
	}
	if (UVerticalBoxSlot* GridSlot = Column->AddChildToVerticalBox(Grid))
		GridSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));

	// ── Detail pane ──
	Detail = CreateWidget<US1ItemDetailWidget>(this, US1ItemDetailWidget::StaticClass());
	if (KeyName)
		Detail->SetKeyNameResolver(KeyName);
	if (UVerticalBoxSlot* DetailSlot = Column->AddChildToVerticalBox(Detail))
		DetailSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));

	// Safety net for the height budget: overflow is cut from the bottom — keys changed, then
	// stats — so the skill lines at the top always survive (§ Layout Zones, UX review A11).
	Detail->SetClipping(EWidgetClipping::ClipToBounds);

	Refresh();
}

void US1InventoryPanelWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (US1ItemSubsystem* Items = GetItems())
	{
		ItemsChangedHandle = Items->OnItemsChanged.AddUObject(this, &US1InventoryPanelWidget::Refresh);
		KeysChangedHandle = Items->OnKeysChanged.AddUObject(this, &US1InventoryPanelWidget::Refresh);
		NoticeHandle = Items->OnItemNotice.AddUObject(this, &US1InventoryPanelWidget::HandleNotice);
	}

	Refresh();
}

void US1InventoryPanelWidget::NativeDestruct()
{
	// The subsystem outlives every widget. Leaving these bound would call into a dead object.
	if (US1ItemSubsystem* Items = GetItems())
	{
		Items->OnItemsChanged.Remove(ItemsChangedHandle);
		Items->OnKeysChanged.Remove(KeysChangedHandle);
		Items->OnItemNotice.Remove(NoticeHandle);
	}

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ConfirmTimer);
		World->GetTimerManager().ClearTimer(NoticeTimer);
	}

	Super::NativeDestruct();
}

// ── Refresh ──────────────────────────────────────────────────────────────

void US1InventoryPanelWidget::Refresh()
{
	US1ItemSubsystem* Items = GetItems();
	if (Items == nullptr || HeaderText == nullptr)
		return;

	const FS1ItemCatalog& Catalog = Items->GetCatalog();
	TArray<const FS1ItemView*> Carried = Items->GetByState(ES1ItemState::Carried);
	const int32 Capacity = Items->GetCarriedCapacity();

	// ── Header: count, and "가득 참" spelled out — never color alone ──
	FText Header = FText::Format(INVTEXT("{0} {1}/{2}"), Text(TEXT("label.carried")), FText::AsNumber(Carried.Num()), FText::AsNumber(Capacity));
	if (Carried.Num() >= Capacity)
		Header = FText::Format(INVTEXT("{0} · ■ {1}"), Header, Text(TEXT("label.full")));
	HeaderText->SetText(Header);

	// ── Key strip ──
	const TArray<FS1KeySlotView> KeySlots = Items->GetKeySlots();
	for (int32 i = 0; i < StripSlots.Num(); i++)
	{
		const int32 Part = StripParts[i];

		if (const FS1ItemView* Worn = Items->GetEquipped(Part))
			StripSlots[i]->SetItem(Worn->InstanceId, Worn->Grade, SlotLabel(Catalog.Find(Worn->ItemTypeId), *Worn), Items->IsPending(Worn->InstanceId));
		else
			StripSlots[i]->SetEmpty(true);
	}

	for (int32 i = 0; i < StripKeys.Num(); i++)
	{
		const int32 Key = StripKeySlots[i];
		const FS1KeySlotView* View = KeySlots.FindByPredicate([Key](const FS1KeySlotView& K) { return K.KeySlot == Key; });
		const bool bHasSkill = View != nullptr && View->SkillId != 0 && View->Bind != ES1BindState::Unbound;

		// Shadowed keeps the skill name but goes dark — a dead key, not an error, so no red and
		// no reason here; S3 explains it (EB:537-542). An empty key reads "—" instead.
		const bool bShadowed = bHasSkill && View->Bind == ES1BindState::Shadowed;

		const FText Name = bHasSkill ? SkillName(View->SkillId) : FText::AsCultureInvariant(TEXT("—"));
		const FText KeyText = KeyName ? KeyName(Key) : FText::AsNumber(Key);

		StripKeys[i]->SetText(FText::Format(INVTEXT("[{0}] {1}"), KeyText, Name));
		StripKeys[i]->SetColorAndOpacity(FSlateColor(bShadowed ? TextShadowed() : TextSecondary()));
	}

	// ── Grid, sorted so the same item does not jump between redraws ──
	auto SortKey = [&Catalog, this](const FS1ItemView& Item) -> FString
	{
		const FS1ItemTypeDef* Type = Catalog.Find(Item.ItemTypeId);
		if (bSortBySkill)
		{
			const TArray<FS1KeySkill> Keys = Type != nullptr ? S1ItemLogic::KeysFor(*Type, Item) : TArray<FS1KeySkill>();
			// Items without a skill go last.
			return Keys.Num() > 0 ? SkillName(Keys[0].SkillId).ToString() : FString::ChrN(1, static_cast<TCHAR>(0xFFFF));
		}
		return ItemName(Type, Item.ItemTypeId).ToString();
	};

	Carried.Sort([&SortKey](const FS1ItemView& A, const FS1ItemView& B)
	{
		const FString KeyA = SortKey(A);
		const FString KeyB = SortKey(B);
		return KeyA != KeyB ? KeyA < KeyB : A.InstanceId < B.InstanceId;
	});

	for (int32 i = 0; i < GridSlots.Num(); i++)
	{
		if (Carried.IsValidIndex(i))
		{
			const FS1ItemView& Item = *Carried[i];
			GridSlots[i]->SetItem(Item.InstanceId, Item.Grade, SlotLabel(Catalog.Find(Item.ItemTypeId), Item), Items->IsPending(Item.InstanceId));
		}
		else
		{
			GridSlots[i]->SetEmpty(false);
		}
	}

	SortText->SetText(FText::Format(INVTEXT("⇅ {0}"), Text(bSortBySkill ? TEXT("label.sort_skill") : TEXT("label.sort_name"))));

	// The item under the cursor or in the confirm may have just changed or gone.
	if (HoveredId != 0 && Items->FindItem(HoveredId) == nullptr)
		HoveredId = 0;
	if (ConfirmId != 0 && Items->FindItem(ConfirmId) == nullptr)
		ConfirmId = 0;

	RefreshDetail();
}

void US1InventoryPanelWidget::RefreshDetail()
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

FReply US1InventoryPanelWidget::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	// The sort label is plain text; hit-test it here rather than dressing it as a button.
	if (SortText != nullptr && SortText->GetCachedGeometry().IsUnderLocation(InMouseEvent.GetScreenSpacePosition()))
	{
		bSortBySkill = !bSortBySkill;
		Refresh();
	}
	else
	{
		// Clicking elsewhere in the panel drops a pending discard confirm (§ Interaction Map).
		CancelConfirm();
	}

	// The panel background swallows the click — it must never confirm an aimed skill behind it.
	return FReply::Handled();
}

void US1InventoryPanelWidget::HandleSlotHovered(uint64 InstanceId)
{
	HoveredId = InstanceId;
	RefreshDetail();
}

void US1InventoryPanelWidget::HandleSlotClicked(uint64 InstanceId, FKey Button, bool bCtrl)
{
	US1ItemSubsystem* Items = GetItems();
	if (Items == nullptr)
		return;

	// An empty cell counts as "elsewhere in the panel".
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

	if (Button != EKeys::LeftMouseButton && Button != EKeys::RightMouseButton)
		return;

	if (Item->State == ES1ItemState::Equipped)
	{
		Items->RequestUnequip(InstanceId);
	}
	else if (Item->State == ES1ItemState::Carried)
	{
		const FS1ItemTypeDef* Type = Items->GetCatalog().Find(Item->ItemTypeId);
		if (Type == nullptr)
		{
			// Sell-only loot (2026-10-17 economy) says where it goes; a truly unknown type cannot tell which part it goes on
			ShowNotice(Notice(Items->IsLoot(Item->ItemTypeId) ? ES1ItemNotice::LootNotEquippable : ES1ItemNotice::BadRequest, 0), InstanceId);
			ShakeSlot(InstanceId);
			return;
		}

		Items->RequestEquip(InstanceId, Type->EquipSlot);
	}

	Refresh();	// Show the pending dim right away
}

void US1InventoryPanelWidget::Discard(const FS1ItemView& Item)
{
	US1ItemSubsystem* Items = GetItems();
	if (Items == nullptr)
		return;

	switch (Items->DiscardRuleFor(Item.InstanceId))
	{
	case ES1DiscardRule::Reject:
		CancelConfirm();
		ShowNotice(Notice(ES1ItemNotice::MustUnequipFirst, 0), Item.InstanceId);
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
			// Second Ctrl+click on the same cell inside the window — it goes.
			CancelConfirm();
			Items->RequestDrop(Item.InstanceId);
			Refresh();
			return;
		}

		ConfirmId = Item.InstanceId;
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().SetTimer(ConfirmTimer, FTimerDelegate::CreateUObject(this, &US1InventoryPanelWidget::CancelConfirm),
				Items->GetDiscardConfirmSeconds(), false);
		}
		RefreshDetail();
		return;
	}
}

void US1InventoryPanelWidget::CancelConfirm()
{
	if (ConfirmId == 0)
		return;

	ConfirmId = 0;
	if (UWorld* World = GetWorld())
		World->GetTimerManager().ClearTimer(ConfirmTimer);

	RefreshDetail();
}

void US1InventoryPanelWidget::OnClosed()
{
	CancelConfirm();
	HoveredId = 0;
	RefreshDetail();
}

// ── Notices ──────────────────────────────────────────────────────────────

void US1InventoryPanelWidget::HandleNotice(ES1ItemNotice InNotice, uint64 InstanceId, int32 Count)
{
	ShowNotice(Notice(InNotice, Count), InstanceId);
	ShakeSlot(InstanceId);
}

void US1InventoryPanelWidget::ShowNotice(const FText& InText, uint64 InstanceId)
{
	if (NoticeText == nullptr)
		return;

	NoticeText->SetText(InText);

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(NoticeTimer, FTimerDelegate::CreateUObject(this, &US1InventoryPanelWidget::ClearNotice),
			NoticeSeconds, false);
	}
}

void US1InventoryPanelWidget::ClearNotice()
{
	if (NoticeText != nullptr)
		NoticeText->SetText(FText::GetEmpty());
}

void US1InventoryPanelWidget::ShakeSlot(uint64 InstanceId)
{
	if (InstanceId == 0)
		return;

	for (US1ItemSlotWidget* Cell : StripSlots)
	{
		if (Cell->GetInstanceId() == InstanceId)
			Cell->Shake();
	}
	for (US1ItemSlotWidget* Cell : GridSlots)
	{
		if (Cell->GetInstanceId() == InstanceId)
			Cell->Shake();
	}
}
