// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Game/Item/S1ItemTypes.h"
#include "S1ItemDetailWidget.generated.h"

class UVerticalBox;
class US1ItemSubsystem;

/** Display name of the key bound to an equip slot ("Shift", or whatever it was remapped to). */
using FS1KeyNameResolver = TFunction<FText(int32 /*KeySlot*/)>;

/**
 * The fixed detail pane at the bottom of a panel — design/ux/item-ui.md § Component Inventory.
 *
 * Fixed rather than a floating tooltip: it never covers the middle of the screen and the eye
 * always goes to the same place. Order is skill first, stats after (EB:529).
 */
UCLASS()
class S1_API US1ItemDetailWidget : public UUserWidget
{
	GENERATED_BODY()

public:

	void SetKeyNameResolver(FS1KeyNameResolver InResolver) { KeyName = MoveTemp(InResolver); }

	/**
	 * Shows Item. When it is not the worn item, stats are compared against Worn (the item in the
	 * same part, may be null) and the keys that would change are listed.
	 */
	void ShowItem(const US1ItemSubsystem& Items, const FS1ItemView& Item, const FS1ItemView* Worn);

	/** The inline discard confirm takes the pane's place (§ Interaction Map). */
	void ShowDiscardConfirm(const US1ItemSubsystem& Items, const FS1ItemView& Item);

	/** Nothing hovered — an empty frame (§ Data Requirements, "값이 없을 때"). */
	void Clear();

protected:

	virtual void NativeOnInitialized() override;

private:

	void AddLine(const FText& Text, int32 FontSize, const FLinearColor& Color);
	void AddItemHeader(const US1ItemSubsystem& Items, const FS1ItemView& Item);
	FText KeyLabel(int32 KeySlot) const;

	UPROPERTY() TObjectPtr<UVerticalBox> Lines;

	FS1KeyNameResolver KeyName;
};
