// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "InputCoreTypes.h"
#include "Game/Item/S1ItemTypes.h"
#include "Game/UI/S1ItemDetailWidget.h"
#include "S1InventoryPanelWidget.generated.h"

class UTextBlock;
class US1ItemSlotWidget;
class US1ItemSubsystem;

/**
 * S1 — the dungeon inventory and equipment panel. design/ux/item-ui.md.
 *
 *   header      소지 n/25 · 가득 참, then a notice line
 *   key strip   five worn parts; under each, its keys and their skills
 *   grid        25 carried cells
 *   detail      fixed pane — the hovered item, or the discard confirm
 *   sort        이름순 / 스킬순
 *
 * Click grammar (§ Interaction Map): left = this panel's action (wear / take off),
 * right = always the equipment toggle, Ctrl+left = always discard.
 */
UCLASS()
class S1_API US1InventoryPanelWidget : public UUserWidget
{
	GENERATED_BODY()

public:

	void SetKeyNameResolver(FS1KeyNameResolver InResolver);

	/** Redraws from the item model. Cheap enough to run on every change. */
	void Refresh();

	/** The panel is going away (Tab, Esc). A pending discard confirm does not survive it. */
	void OnClosed();

protected:

	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;

private:

	US1ItemSubsystem* GetItems() const;

	void HandleSlotClicked(uint64 InstanceId, FKey Button, bool bCtrl);
	void HandleSlotHovered(uint64 InstanceId);
	void HandleNotice(ES1ItemNotice Notice, uint64 InstanceId, int32 Count);

	void Discard(const FS1ItemView& Item);
	void CancelConfirm();
	void ShowNotice(const FText& Text, uint64 InstanceId);
	void ClearNotice();
	void ShakeSlot(uint64 InstanceId);
	void RefreshDetail();

	UPROPERTY() TObjectPtr<UTextBlock>						HeaderText;
	UPROPERTY() TObjectPtr<UTextBlock>						NoticeText;
	UPROPERTY() TArray<TObjectPtr<US1ItemSlotWidget>>		StripSlots;		// parts 1, 3, 4, 5, 6
	UPROPERTY() TArray<TObjectPtr<UTextBlock>>				StripKeys;		// one line per key
	TArray<int32>											StripKeySlots;	// the key each line shows
	UPROPERTY() TArray<TObjectPtr<US1ItemSlotWidget>>		GridSlots;
	UPROPERTY() TObjectPtr<US1ItemDetailWidget>				Detail;
	UPROPERTY() TObjectPtr<UTextBlock>						SortText;

	FS1KeyNameResolver	KeyName;
	bool				bSortBySkill = false;
	uint64				HoveredId = 0;
	uint64				ConfirmId = 0;
	FTimerHandle		ConfirmTimer;
	FTimerHandle		NoticeTimer;

	FDelegateHandle		ItemsChangedHandle;
	FDelegateHandle		KeysChangedHandle;
	FDelegateHandle		NoticeHandle;
};
