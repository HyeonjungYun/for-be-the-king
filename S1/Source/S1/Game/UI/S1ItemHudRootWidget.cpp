// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/UI/S1ItemHudRootWidget.h"
#include "Game/UI/S1InventoryPanelWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"

namespace
{
	// Z-R: anchors, not pixels, so the 25% / 70% limits hold at any resolution (AC-UI-10).
	const FAnchors InventoryAnchors(0.75f, 0.15f, 1.0f, 0.85f);
	const FMargin InventoryInset(0.f, 0.f, 12.f, 0.f);
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

	SetVisibility(ESlateVisibility::SelfHitTestInvisible);
}

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
	}
	else
	{
		Inventory->OnClosed();
		Inventory->SetVisibility(ESlateVisibility::Collapsed);
	}

	UE_LOG(LogTemp, Log, TEXT("[ITEMUI] inventory %s - root in_viewport=%d root_size=%s panel_size=%s panel_desired=%s"),
		bOpen ? TEXT("open") : TEXT("closed"), IsInViewport() ? 1 : 0,
		*GetCachedGeometry().GetLocalSize().ToString(), *Inventory->GetCachedGeometry().GetLocalSize().ToString(),
		*Inventory->GetDesiredSize().ToString());
}

bool US1ItemHudRootWidget::CloseTopmost()
{
	// S2 joins this in stage 3 — then "topmost" means the last one opened.
	if (IsInventoryOpen())
	{
		SetInventoryOpen(false);
		return true;
	}
	return false;
}
