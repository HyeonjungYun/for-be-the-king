// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Game/UI/S1ItemDetailWidget.h"
#include "S1ItemHudRootWidget.generated.h"

class UBorder;
class UCanvasPanel;
class UTextBlock;
class US1InventoryPanelWidget;
class US1LootWindowWidget;

/**
 * The one item-UI widget on the viewport. It places the panels — design/ux/item-ui.md § Layout Zones.
 *
 *   Z-L  S2 loot window — left edge, ≤ 25% wide, ≤ 70% tall
 *   Z-R  S1 inventory   — right edge, same limits
 *   Z-C  the middle 50% stays empty, except the small "F" badge over a bag in range
 *
 * The root itself never takes a click (SelfHitTestInvisible), so empty screen still belongs to the game.
 */
UCLASS()
class S1_API US1ItemHudRootWidget : public UUserWidget
{
	GENERATED_BODY()

public:

	/** Skill keys for the badges, and the key that opens a bag for the prompt. */
	void SetKeyNameResolver(FS1KeyNameResolver InSkillKeys, TFunction<FText()> InOpenKey);

	bool IsInventoryOpen() const;
	void SetInventoryOpen(bool bOpen);

	bool IsLootOpen() const;

	/** F — opens the nearest bag in range, or closes the open one. */
	void ToggleLootNearest();

	/** Esc — closes the last opened item window. Returns false when nothing was open. */
	bool CloseTopmost();

	/** Death, or leaving the dungeon — both windows go at once. */
	void CloseAll();

protected:

	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:

	enum class EOpened : uint8 { None, Inventory, Loot };

	/** The bag F would open right now, or 0. */
	uint64 FindPromptBag() const;

	UPROPERTY() TObjectPtr<UCanvasPanel>				Canvas;
	UPROPERTY() TObjectPtr<US1InventoryPanelWidget>		Inventory;
	UPROPERTY() TObjectPtr<US1LootWindowWidget>			Loot;
	UPROPERTY() TObjectPtr<UBorder>						PromptBadge;
	UPROPERTY() TObjectPtr<UTextBlock>					PromptText;

	TFunction<FText()>	OpenKeyName;
	EOpened				LastOpened = EOpened::None;
};
