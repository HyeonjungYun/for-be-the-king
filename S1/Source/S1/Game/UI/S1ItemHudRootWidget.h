// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "S1ItemHudRootWidget.generated.h"

class UCanvasPanel;
class US1InventoryPanelWidget;

/**
 * The one item-UI widget on the viewport. It places the panels — design/ux/item-ui.md § Layout Zones.
 *
 *   Z-R  S1 inventory — right edge, ≤ 25% wide, ≤ 70% tall, vertically centred
 *   Z-C  the middle 50% stays empty
 *
 * The root itself never takes a click (SelfHitTestInvisible), so empty screen still belongs to the game.
 */
UCLASS()
class S1_API US1ItemHudRootWidget : public UUserWidget
{
	GENERATED_BODY()

public:

	US1InventoryPanelWidget* GetInventory() const { return Inventory; }

	bool IsInventoryOpen() const;
	void SetInventoryOpen(bool bOpen);

	/** Esc — closes the last opened item window. Returns false when nothing was open. */
	bool CloseTopmost();

protected:

	virtual void NativeOnInitialized() override;

private:

	UPROPERTY() TObjectPtr<UCanvasPanel>				Canvas;
	UPROPERTY() TObjectPtr<US1InventoryPanelWidget>		Inventory;
};
