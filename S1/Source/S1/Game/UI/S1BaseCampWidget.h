// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "InputCoreTypes.h"
#include "Game/Item/S1ItemTypes.h"
#include "S1BaseCampWidget.generated.h"

class UBorder;
class UTextBlock;
class UVerticalBox;
class US1BaseCampSubsystem;
class US1ItemDetailWidget;
class US1ItemSlotWidget;
class US1ItemSubsystem;
enum class ES1Place : uint8;

/**
 * S3 — 출정 선택, the base-camp screen. design/ux/item-ui.md (B-L | B-C | B-R).
 *
 *   B-L 28%  보관 n/25 · sort      B-C 44%  six keys over the five worn parts    B-R 28%  반입 n/25 · sort
 *            notice                         장비 합산 — the worn stats, summed           notice
 *            vault grid 5×5                 detail pane                                 carried grid 5×5
 *                                                                                       [입장]
 *
 * Click grammar (§ Interaction Map): left = move between vault and carried, or take off a worn
 * part; right = wear from carried — from the vault it says why not (AC-UI-30); Ctrl+left = discard.
 * Every action goes straight to the DB on the server, and nothing changes here until it answers.
 */
UCLASS()
class S1_API US1BaseCampWidget : public UUserWidget
{
	GENERATED_BODY()

protected:

	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;

private:

	enum class EColumn : uint8 { Vault = 0, Center, Carried, Count };

	US1ItemSubsystem* GetItems() const;
	US1BaseCampSubsystem* GetBaseCamp() const;

	/** Redraws everything from the item model. Cheap enough to run on every change. */
	void Refresh();
	void RefreshGrid(bool bVault);
	void RefreshStrip();
	void RefreshTotals();
	void RefreshEnter();
	void RefreshDetail();

	void HandleSlotClicked(uint64 InstanceId, FKey Button, bool bCtrl);
	void HandleSlotHovered(uint64 InstanceId);
	void HandleNotice(ES1ItemNotice Notice, uint64 InstanceId, int32 Count);
	void HandlePlaceChanged(ES1Place Place);
	void HandleEnterRejected();

	void Discard(const FS1ItemView& Item);
	void CancelConfirm();
	void ShowNotice(EColumn Column, const FText& Text, uint64 InstanceId);
	void ClearNotices();
	void ShakeSlot(uint64 InstanceId);

	/** Before the base-camp inventory arrives, and while [입장] is in flight — no clicks count. */
	bool IsLocked() const;

	FText KeyName(int32 KeySlot) const;

	UPROPERTY() TObjectPtr<UTextBlock>						VaultHeader;
	UPROPERTY() TObjectPtr<UTextBlock>						VaultSort;
	UPROPERTY() TObjectPtr<UTextBlock>						CarriedHeader;
	UPROPERTY() TObjectPtr<UTextBlock>						CarriedSort;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>>				Notices;		// indexed by EColumn
	UPROPERTY() TArray<TObjectPtr<US1ItemSlotWidget>>		VaultSlots;
	UPROPERTY() TArray<TObjectPtr<US1ItemSlotWidget>>		CarriedSlots;
	UPROPERTY() TArray<TObjectPtr<US1ItemSlotWidget>>		StripSlots;		// parts 1, 3, 4, 5, 6
	UPROPERTY() TArray<TObjectPtr<UTextBlock>>				StripPartLabels;	// archetype, or part name when empty
	UPROPERTY() TArray<TObjectPtr<UTextBlock>>				StripKeys;		// one line per key
	TArray<int32>											StripKeySlots;
	UPROPERTY() TObjectPtr<UTextBlock>						TotalsTitle;
	UPROPERTY() TObjectPtr<UVerticalBox>					TotalLines;
	UPROPERTY() TObjectPtr<US1ItemDetailWidget>				Detail;
	UPROPERTY() TObjectPtr<UBorder>							EnterButton;
	UPROPERTY() TObjectPtr<UTextBlock>						EnterText;

	bool			bVaultBySkill = false;
	bool			bCarriedBySkill = false;
	uint64			HoveredId = 0;
	uint64			ConfirmId = 0;
	FTimerHandle	ConfirmTimer;
	FTimerHandle	NoticeTimer;

	FDelegateHandle	ItemsChangedHandle;
	FDelegateHandle	KeysChangedHandle;
	FDelegateHandle	NoticeHandle;
	FDelegateHandle	PlaceChangedHandle;
	FDelegateHandle	ReadyHandle;
	FDelegateHandle	RejectedHandle;
};
