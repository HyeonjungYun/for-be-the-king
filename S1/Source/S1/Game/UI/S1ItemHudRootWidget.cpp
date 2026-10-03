// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/UI/S1ItemHudRootWidget.h"
#include "Game/UI/S1InventoryPanelWidget.h"
#include "Game/UI/S1LootWindowWidget.h"
#include "Game/UI/S1ItemUIStyle.h"
#include "Game/Item/S1ItemSubsystem.h"
#include "Blueprint/WidgetLayoutLibrary.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"
#include "Engine/GameInstance.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"

namespace
{
	// Anchors, not pixels, so the 25% / 70% limits hold at any resolution (AC-UI-10).
	const FAnchors InventoryAnchors(0.75f, 0.15f, 1.0f, 0.85f);		// Z-R
	const FMargin InventoryInset(0.f, 0.f, 12.f, 0.f);
	const FAnchors LootAnchors(0.0f, 0.15f, 0.25f, 0.85f);			// Z-L
	const FMargin LootInset(12.f, 0.f, 0.f, 0.f);

	/** The badge sits a little above the bag so the character does not hide it. */
	const FVector PromptLift(0.0, 0.0, 60.0);
}

void US1ItemHudRootWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	Canvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass());
	WidgetTree->RootWidget = Canvas;

	Inventory = CreateWidget<US1InventoryPanelWidget>(this, US1InventoryPanelWidget::StaticClass());
	if (UCanvasPanelSlot* InventorySlot = Canvas->AddChildToCanvas(Inventory))
	{
		InventorySlot->SetAnchors(InventoryAnchors);
		InventorySlot->SetOffsets(InventoryInset);
	}
	Inventory->SetVisibility(ESlateVisibility::Collapsed);

	Loot = CreateWidget<US1LootWindowWidget>(this, US1LootWindowWidget::StaticClass());
	if (UCanvasPanelSlot* LootSlot = Canvas->AddChildToCanvas(Loot))
	{
		LootSlot->SetAnchors(LootAnchors);
		LootSlot->SetOffsets(LootInset);
	}
	Loot->SetVisibility(ESlateVisibility::Collapsed);

	// ── "F 열기" — a key badge over the bag F would open. Says nothing about what is inside (P4). ──
	PromptBadge = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	PromptBadge->SetBrushColor(S1ItemUIStyle::PanelBackground());
	PromptBadge->SetPadding(FMargin(6.f, 2.f));
	PromptText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	PromptText->SetFont(S1ItemUIStyle::Font(S1ItemUIStyle::FontBody));
	PromptText->SetColorAndOpacity(FSlateColor(S1ItemUIStyle::TextPrimary()));
	PromptBadge->SetContent(PromptText);
	if (UCanvasPanelSlot* PromptSlot = Canvas->AddChildToCanvas(PromptBadge))
	{
		PromptSlot->SetAutoSize(true);
		PromptSlot->SetAlignment(FVector2D(0.5, 1.0));
	}
	PromptBadge->SetVisibility(ESlateVisibility::Collapsed);

	SetVisibility(ESlateVisibility::SelfHitTestInvisible);
}

void US1ItemHudRootWidget::SetKeyNameResolver(FS1KeyNameResolver InSkillKeys, TFunction<FText()> InOpenKey)
{
	Inventory->SetKeyNameResolver(InSkillKeys);
	Loot->SetKeyNameResolver(InSkillKeys);
	OpenKeyName = MoveTemp(InOpenKey);
}

// ── Inventory (S1) ───────────────────────────────────────────────────────

bool US1ItemHudRootWidget::IsInventoryOpen() const
{
	return Inventory != nullptr && Inventory->GetVisibility() == ESlateVisibility::Visible;
}

void US1ItemHudRootWidget::SetInventoryOpen(bool bOpen)
{
	if (Inventory == nullptr || IsInventoryOpen() == bOpen)
		return;

	if (bOpen)
	{
		Inventory->Refresh();
		Inventory->SetVisibility(ESlateVisibility::Visible);
		LastOpened = EOpened::Inventory;
	}
	else
	{
		Inventory->OnClosed();
		Inventory->SetVisibility(ESlateVisibility::Collapsed);
	}

	UE_LOG(LogTemp, Log, TEXT("[ITEMUI] inventory %s - panel_size=%s of %s"),
		bOpen ? TEXT("open") : TEXT("closed"),
		*Inventory->GetCachedGeometry().GetLocalSize().ToString(), *GetCachedGeometry().GetLocalSize().ToString());
}

// ── Loot (S2) ────────────────────────────────────────────────────────────

bool US1ItemHudRootWidget::IsLootOpen() const
{
	return Loot != nullptr && Loot->IsOpen();
}

void US1ItemHudRootWidget::ToggleLootNearest()
{
	if (Loot == nullptr)
		return;

	// F again closes it — the same key opens and shuts (§ Interaction Map).
	if (Loot->IsOpen())
	{
		Loot->CloseSilently();
		return;
	}

	const uint64 BagId = FindPromptBag();
	if (BagId == 0)
		return;		// Nothing in range. The server would refuse anyway, so say nothing.

	Loot->Open(BagId);
	LastOpened = EOpened::Loot;
}

uint64 US1ItemHudRootWidget::FindPromptBag() const
{
	UGameInstance* GameInstance = GetGameInstance();
	const US1ItemSubsystem* Items = GameInstance != nullptr ? GameInstance->GetSubsystem<US1ItemSubsystem>() : nullptr;
	const APawn* Pawn = GetOwningPlayerPawn();
	if (Items == nullptr || Pawn == nullptr)
		return 0;

	return Items->FindNearestBag(Pawn->GetActorLocation());
}

// ── Esc ──────────────────────────────────────────────────────────────────

bool US1ItemHudRootWidget::CloseTopmost()
{
	// Both open: Esc closes the one opened last (§ Entry & Exit).
	if (IsLootOpen() && (LastOpened == EOpened::Loot || IsInventoryOpen() == false))
	{
		Loot->CloseSilently();
		LastOpened = IsInventoryOpen() ? EOpened::Inventory : EOpened::None;
		return true;
	}

	if (IsInventoryOpen())
	{
		SetInventoryOpen(false);
		LastOpened = IsLootOpen() ? EOpened::Loot : EOpened::None;
		return true;
	}

	return false;
}

// ── Prompt badge ─────────────────────────────────────────────────────────

void US1ItemHudRootWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	if (PromptBadge == nullptr)
		return;

	// No badge while the window is already open — the badge's job is done.
	const uint64 BagId = IsLootOpen() ? 0 : FindPromptBag();

	UGameInstance* GameInstance = GetGameInstance();
	const US1ItemSubsystem* Items = GameInstance != nullptr ? GameInstance->GetSubsystem<US1ItemSubsystem>() : nullptr;
	const FS1BagView* Bag = (BagId != 0 && Items != nullptr) ? Items->FindBag(BagId) : nullptr;

	FVector2D ScreenPosition;
	const bool bOnScreen = Bag != nullptr
		&& UWidgetLayoutLibrary::ProjectWorldLocationToWidgetPosition(GetOwningPlayer(), Bag->Location + PromptLift, ScreenPosition, false);

	if (bOnScreen == false)
	{
		PromptBadge->SetVisibility(ESlateVisibility::Collapsed);
		return;
	}

	PromptText->SetText(FText::Format(INVTEXT("[{0}]"), OpenKeyName ? OpenKeyName() : FText::AsCultureInvariant(TEXT("F"))));
	if (UCanvasPanelSlot* PromptSlot = Cast<UCanvasPanelSlot>(PromptBadge->Slot))
		PromptSlot->SetPosition(ScreenPosition);
	PromptBadge->SetVisibility(ESlateVisibility::HitTestInvisible);
}
