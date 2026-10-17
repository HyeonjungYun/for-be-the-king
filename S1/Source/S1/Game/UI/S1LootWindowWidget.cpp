// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/UI/S1LootWindowWidget.h"
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
#include "GameFramework/Pawn.h"
#include "TimerManager.h"

using namespace S1ItemUIStyle;

namespace
{
	constexpr float TickSeconds = 0.25f;			// Range check + countdown
	constexpr double OpeningHintSeconds = 0.3;		// § States — "여는 중" only past 300 ms
	constexpr uint64 ExpiryEmphasisMs = 60000;		// § States — last minute reads bigger

	FText FormatRemaining(uint64 RemainingMs)
	{
		const uint64 Seconds = (RemainingMs + 999) / 1000;	// 0:00 only when it truly ends
		FNumberFormattingOptions TwoDigits;
		TwoDigits.MinimumIntegralDigits = 2;
		return FText::Format(INVTEXT("{0}:{1}"), FText::AsNumber(Seconds / 60), FText::AsNumber(Seconds % 60, &TwoDigits));
	}
}

US1ItemSubsystem* US1LootWindowWidget::GetItems() const
{
	UGameInstance* GameInstance = GetGameInstance();
	return GameInstance != nullptr ? GameInstance->GetSubsystem<US1ItemSubsystem>() : nullptr;
}

void US1LootWindowWidget::SetKeyNameResolver(FS1KeyNameResolver InResolver)
{
	KeyName = InResolver;
	if (Detail != nullptr)
		Detail->SetKeyNameResolver(MoveTemp(InResolver));
}

// ── Build ────────────────────────────────────────────────────────────────

void US1LootWindowWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	auto MakeText = [this](int32 Size, const FLinearColor& Color) -> UTextBlock*
	{
		UTextBlock* Text = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Text->SetFont(Font(Size));
		Text->SetColorAndOpacity(FSlateColor(Color));
		return Text;
	};

	UBorder* Background = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Background->SetBrushColor(PanelBackground());
	Background->SetPadding(FMargin(PanelPadding));
	WidgetTree->RootWidget = Background;

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Background->SetContent(Column);

	// ── Header: what this is, how long it lasts, how full you are ──
	UHorizontalBox* HeaderRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	TitleText = MakeText(FontEmphasis, TextPrimary());
	MyBagText = MakeText(FontSmall, TextSecondary());
	if (UHorizontalBoxSlot* TitleSlot = HeaderRow->AddChildToHorizontalBox(TitleText))
		TitleSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	if (UHorizontalBoxSlot* MyBagSlot = HeaderRow->AddChildToHorizontalBox(MyBagText))
		MyBagSlot->SetVerticalAlignment(VAlign_Center);
	if (UVerticalBoxSlot* RowSlot = Column->AddChildToVerticalBox(HeaderRow))
		RowSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));

	// ── Status line — height reserved so the grid never jumps ──
	USizeBox* StatusBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	StatusBox->SetHeightOverride(NoticeLineHeight);
	StatusText = MakeText(FontBody, TextPrimary());
	StatusBox->SetContent(StatusText);
	if (UVerticalBoxSlot* StatusSlot = Column->AddChildToVerticalBox(StatusBox))
		StatusSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 2.f));

	// ── Contents grid ──
	UUniformGridPanel* Grid = WidgetTree->ConstructWidget<UUniformGridPanel>(UUniformGridPanel::StaticClass());
	Grid->SetSlotPadding(FMargin(CellGap * 0.5f));
	for (int32 i = 0; i < LootGridCells; i++)
	{
		US1ItemSlotWidget* Cell = CreateWidget<US1ItemSlotWidget>(this, US1ItemSlotWidget::StaticClass());
		Cell->SetCellSize(GridCellSize);
		Cell->OnClicked.BindUObject(this, &US1LootWindowWidget::HandleSlotClicked);
		Cell->OnHovered.BindUObject(this, &US1LootWindowWidget::HandleSlotHovered);
		Grid->AddChildToUniformGrid(Cell, i / GridColumns, i % GridColumns);
		GridSlots.Add(Cell);
	}
	if (UVerticalBoxSlot* GridSlot = Column->AddChildToVerticalBox(Grid))
		GridSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));

	// ── 전체 획득 — a plain bordered label, hit-tested in NativeOnMouseButtonDown ──
	TakeAllButton = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	TakeAllButton->SetPadding(FMargin(8.f, 4.f));
	TakeAllButton->SetHorizontalAlignment(HAlign_Center);
	TakeAllText = MakeText(FontBody, TextPrimary());
	TakeAllText->SetText(Text(TEXT("label.take_all")));
	TakeAllButton->SetContent(TakeAllText);
	if (UVerticalBoxSlot* ButtonSlot = Column->AddChildToVerticalBox(TakeAllButton))
		ButtonSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));

	// ── Detail pane ──
	Detail = CreateWidget<US1ItemDetailWidget>(this, US1ItemDetailWidget::StaticClass());
	if (KeyName)
		Detail->SetKeyNameResolver(KeyName);
	if (UVerticalBoxSlot* DetailSlot = Column->AddChildToVerticalBox(Detail))
		DetailSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	Detail->SetClipping(EWidgetClipping::ClipToBounds);
}

void US1LootWindowWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (US1ItemSubsystem* Items = GetItems())
	{
		BagChangedHandle = Items->OnBagChanged.AddUObject(this, &US1LootWindowWidget::HandleBagChanged);
		BagClosedHandle = Items->OnBagClosed.AddUObject(this, &US1LootWindowWidget::HandleBagClosed);
		ItemsChangedHandle = Items->OnItemsChanged.AddUObject(this, &US1LootWindowWidget::Refresh);
		NoticeHandle = Items->OnItemNotice.AddUObject(this, &US1LootWindowWidget::HandleNotice);
	}
}

void US1LootWindowWidget::NativeDestruct()
{
	if (US1ItemSubsystem* Items = GetItems())
	{
		Items->OnBagChanged.Remove(BagChangedHandle);
		Items->OnBagClosed.Remove(BagClosedHandle);
		Items->OnItemsChanged.Remove(ItemsChangedHandle);
		Items->OnItemNotice.Remove(NoticeHandle);
	}

	if (UWorld* World = GetWorld())
		World->GetTimerManager().ClearTimer(NoticeTimer);

	Super::NativeDestruct();
}

// ── Open / close ─────────────────────────────────────────────────────────

void US1LootWindowWidget::Open(uint64 InBagId)
{
	US1ItemSubsystem* Items = GetItems();
	if (Items == nullptr || InBagId == 0)
		return;

	BagId = InBagId;
	HoveredId = 0;
	OpenedAtSeconds = FPlatformTime::Seconds();
	TickAccumulator = 0.f;
	ClearNotice();

	SetVisibility(ESlateVisibility::Visible);

	Items->RequestOpenBag(BagId);
	Refresh();

	UE_LOG(LogTemp, Log, TEXT("[ITEMUI] loot window open bag=%llu"), BagId);
}

void US1LootWindowWidget::CloseSilently()
{
	if (BagId != 0)
	{
		UE_LOG(LogTemp, Log, TEXT("[ITEMUI] loot window closed bag=%llu"), BagId);

		US1ItemSubsystem* Items = GetItems();
		if (Items != nullptr && Items->GetOpenBagId() == BagId)
			Items->CloseBag();
	}

	BagId = 0;
	HoveredId = 0;
	SetVisibility(ESlateVisibility::Collapsed);
}

// ── Refresh ──────────────────────────────────────────────────────────────

void US1LootWindowWidget::RefreshHeader()
{
	US1ItemSubsystem* Items = GetItems();
	if (Items == nullptr || IsOpen() == false)
		return;

	const FS1BagView* Bag = Items->FindBag(BagId);

	// A chest is a bag that never expires: remaining_ms 0 → "상자" and no countdown (§ States — 기본 · 상자).
	// Until the contents arrive we cannot tell, so it reads "가방" for that moment.
	const bool bChest = Bag != nullptr && Bag->bHasContents && Bag->RemainingMsAtReceive == 0;
	FText Title = Text(bChest ? TEXT("bag.title_chest") : TEXT("bag.title_bag"));
	// A death bag and a monster's drop read differently - "someone died here" is information (monster-ai.md OQ-3, 2026-10-18)
	if (bChest == false && Bag != nullptr && Bag->Kind == ES1BagKind::Death)
		Title = Text(TEXT("bag.title_death"));
	else if (bChest == false && Bag != nullptr && Bag->Kind == ES1BagKind::Monster)
		Title = Text(TEXT("bag.title_monster"));
	bool bUrgent = false;
	if (Bag != nullptr && Bag->bHasContents && Bag->RemainingMsAtReceive > 0)
	{
		const uint64 Remaining = S1ItemLogic::RemainingMs(*Bag, FPlatformTime::Seconds());
		Title = FText::Format(INVTEXT("{0} · {1}"), Title, FormatRemaining(Remaining));
		bUrgent = Remaining < ExpiryEmphasisMs;
	}

	// The last minute reads bigger, not just redder — never color alone (§ Accessibility).
	TitleText->SetText(Title);
	TitleText->SetFont(Font(bUrgent ? FontEmphasis + 3 : FontEmphasis));

	const int32 Carried = Items->CountByState(ES1ItemState::Carried);
	const int32 Capacity = Items->GetCarriedCapacity();
	FText Mine = FText::Format(INVTEXT("{0} {1}/{2}"), Text(TEXT("bag.my_bag")), FText::AsNumber(Carried), FText::AsNumber(Capacity));
	if (Carried >= Capacity)
		Mine = FText::Format(INVTEXT("{0} · ■ {1}"), Mine, Text(TEXT("label.full")));
	MyBagText->SetText(Mine);

	// Status line: a live notice wins; otherwise "여는 중" while the contents are on their way.
	// An empty bag says nothing — the empty grid and the dimmed 전체 획득 already show it.
	if (bNoticeShowing == false)
	{
		FText Status = FText::GetEmpty();
		if ((Bag == nullptr || Bag->bHasContents == false)
			&& FPlatformTime::Seconds() - OpenedAtSeconds > OpeningHintSeconds)
		{
			Status = Text(TEXT("bag.opening"));
		}
		StatusText->SetText(Status);
		StatusText->SetColorAndOpacity(FSlateColor(TextSecondary()));
	}
}

void US1LootWindowWidget::Refresh()
{
	US1ItemSubsystem* Items = GetItems();
	if (Items == nullptr || IsOpen() == false)
		return;

	RefreshHeader();

	const FS1ItemCatalog& Catalog = Items->GetCatalog();
	const FS1BagView* Bag = Items->FindBag(BagId);
	const TArray<FS1ItemView>* Contents = (Bag != nullptr && Bag->bHasContents) ? &Bag->Items : nullptr;
	const int32 Count = Contents != nullptr ? Contents->Num() : 0;

	if (Count > GridSlots.Num())
		UE_LOG(LogTemp, Warning, TEXT("[ITEMUI] bag %llu holds %d items, the window shows %d"), BagId, Count, GridSlots.Num());

	for (int32 i = 0; i < GridSlots.Num(); i++)
	{
		if (i < Count)
		{
			const FS1ItemView& Item = (*Contents)[i];
			GridSlots[i]->SetItem(Item.InstanceId, Item.Grade, SlotLabel(Catalog.Find(Item.ItemTypeId), Item), Items->IsPending(Item.InstanceId));
		}
		else
		{
			GridSlots[i]->SetEmpty(false);
		}
	}

	// 전체 획득 is dead on an empty bag (§ States — "비어 있다").
	const bool bCanTakeAll = Count > 0;
	TakeAllButton->SetBrushColor(bCanTakeAll ? CellHover() : CellBackground());
	TakeAllText->SetColorAndOpacity(FSlateColor(bCanTakeAll ? TextPrimary() : TextShadowed()));

	if (HoveredId != 0 && (Contents == nullptr || Contents->ContainsByPredicate([this](const FS1ItemView& I) { return I.InstanceId == HoveredId; }) == false))
		HoveredId = 0;

	RefreshDetail();
}

void US1LootWindowWidget::RefreshDetail()
{
	US1ItemSubsystem* Items = GetItems();
	if (Items == nullptr || Detail == nullptr)
		return;

	const FS1BagView* Bag = Items->FindBag(BagId);
	const FS1ItemView* Item = nullptr;
	if (Bag != nullptr && HoveredId != 0)
		Item = Bag->Items.FindByPredicate([this](const FS1ItemView& I) { return I.InstanceId == HoveredId; });

	if (Item == nullptr)
	{
		Detail->Clear();
		return;
	}

	// Compared with what you wear in that part — the "do I swap?" read before taking it (R5).
	const FS1ItemTypeDef* Type = Items->GetCatalog().Find(Item->ItemTypeId);
	const FS1ItemView* Worn = Type != nullptr ? Items->GetEquipped(Type->EquipSlot) : nullptr;
	Detail->ShowItem(*Items, *Item, Worn);
}

// ── Tick — range and countdown ───────────────────────────────────────────

void US1LootWindowWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	if (IsOpen() == false)
		return;

	TickAccumulator += InDeltaTime;
	if (TickAccumulator < TickSeconds)
		return;
	TickAccumulator = 0.f;

	US1ItemSubsystem* Items = GetItems();
	const APawn* Pawn = GetOwningPlayerPawn();
	const FS1BagView* Bag = Items != nullptr ? Items->FindBag(BagId) : nullptr;

	// Walked away: the server already stopped sending us this bag, so stop showing it.
	if (Bag != nullptr && Bag->bHasLocation && Pawn != nullptr
		&& S1ItemLogic::IsWithinLootRange(Bag->Location, Pawn->GetActorLocation(), Items->GetLootRangeCm()) == false)
	{
		CloseSilently();
		return;
	}

	RefreshHeader();
}

// ── Input ────────────────────────────────────────────────────────────────

FReply US1LootWindowWidget::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (InMouseEvent.GetEffectingButton() == EKeys::LeftMouseButton && TakeAllButton != nullptr
		&& TakeAllButton->GetCachedGeometry().IsUnderLocation(InMouseEvent.GetScreenSpacePosition()))
	{
		US1ItemSubsystem* Items = GetItems();
		const FS1BagView* Bag = Items != nullptr ? Items->FindBag(BagId) : nullptr;
		if (Bag != nullptr && Bag->bHasContents && Bag->Items.Num() > 0)
		{
			Items->RequestLootAll(BagId);
			Refresh();
		}
	}

	// The window swallows the click — it must never confirm an aimed skill behind it.
	return FReply::Handled();
}

void US1LootWindowWidget::HandleSlotClicked(uint64 InstanceId, FKey Button, bool bCtrl)
{
	// S2's grammar: left = take. Right and Ctrl+left mean nothing here (§ Interaction Map).
	if (InstanceId == 0 || Button != EKeys::LeftMouseButton || bCtrl)
		return;

	US1ItemSubsystem* Items = GetItems();
	if (Items == nullptr || Items->IsPending(InstanceId))
		return;

	Items->RequestLoot(BagId, InstanceId);
	Refresh();
}

void US1LootWindowWidget::HandleSlotHovered(uint64 InstanceId)
{
	HoveredId = InstanceId;
	RefreshDetail();
}

// ── Model events ─────────────────────────────────────────────────────────

void US1LootWindowWidget::HandleBagChanged(uint64 InBagId)
{
	if (InBagId == BagId)
		Refresh();
}

void US1LootWindowWidget::HandleBagClosed(uint64 InBagId, ES1BagCloseReason Reason)
{
	// Expired, refilled or emptied — the window simply goes, with no reason line (2026-10-04).
	if (InBagId == BagId)
		CloseSilently();
}

void US1LootWindowWidget::HandleNotice(ES1ItemNotice InNotice, uint64 InstanceId, int32 Count)
{
	if (IsOpen() == false)
		return;

	const FText Message = Notice(InNotice, Count);
	if (Message.IsEmpty())
		return;

	bNoticeShowing = true;
	StatusText->SetText(Message);
	StatusText->SetColorAndOpacity(FSlateColor(TextPrimary()));

	for (US1ItemSlotWidget* Cell : GridSlots)
	{
		if (InstanceId != 0 && Cell->GetInstanceId() == InstanceId)
			Cell->Shake();
	}

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(NoticeTimer, FTimerDelegate::CreateUObject(this, &US1LootWindowWidget::ClearNotice),
			NoticeSeconds, false);
	}
}

void US1LootWindowWidget::ClearNotice()
{
	bNoticeShowing = false;
	if (StatusText != nullptr)
		StatusText->SetText(FText::GetEmpty());
	RefreshHeader();
}
