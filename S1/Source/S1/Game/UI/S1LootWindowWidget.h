// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "InputCoreTypes.h"
#include "Game/Item/S1ItemTypes.h"
#include "Game/UI/S1ItemDetailWidget.h"
#include "S1LootWindowWidget.generated.h"

class UBorder;
class UTextBlock;
class US1ItemSlotWidget;
class US1ItemSubsystem;

/**
 * S2 — the loot window for a death bag (and, once the server has them, a chest).
 * design/ux/item-ui.md.
 *
 *   header      가방 · m:ss            내 가방 n/25
 *   status      notice, or 여는 중 (height always reserved)
 *   grid        5 × 6 — a death bag holds up to 30 (25 carried + 5 worn, GDD AC-IE-31)
 *   전체 획득
 *   detail      the hovered item, compared with what you wear
 *
 * Left click takes an item. The window closes itself when you walk out of range — the server
 * drops out-of-range viewers without a packet — when "전체 획득" emptied it, and when the bag
 * expires. It never says why.
 */
UCLASS()
class S1_API US1LootWindowWidget : public UUserWidget
{
	GENERATED_BODY()

public:

	void SetKeyNameResolver(FS1KeyNameResolver InResolver);

	void Open(uint64 InBagId);

	/** F again, Esc, walking away, emptied by take-all, or expired. No reason is shown. */
	void CloseSilently();

	bool IsOpen() const { return BagId != 0; }
	uint64 GetBagId() const { return BagId; }

protected:

	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;

private:

	US1ItemSubsystem* GetItems() const;

	void Refresh();
	void RefreshHeader();
	void RefreshDetail();

	void HandleSlotClicked(uint64 InstanceId, FKey Button, bool bCtrl);
	void HandleSlotHovered(uint64 InstanceId);
	void HandleBagChanged(uint64 InBagId);
	void HandleBagClosed(uint64 InBagId, ES1BagCloseReason Reason);
	void HandleNotice(ES1ItemNotice Notice, uint64 InstanceId, int32 Count);
	void ClearNotice();

	UPROPERTY() TObjectPtr<UTextBlock>						TitleText;
	UPROPERTY() TObjectPtr<UTextBlock>						MyBagText;
	UPROPERTY() TObjectPtr<UTextBlock>						StatusText;
	UPROPERTY() TArray<TObjectPtr<US1ItemSlotWidget>>		GridSlots;
	UPROPERTY() TObjectPtr<UBorder>							TakeAllButton;
	UPROPERTY() TObjectPtr<UTextBlock>						TakeAllText;
	UPROPERTY() TObjectPtr<US1ItemDetailWidget>				Detail;

	FS1KeyNameResolver	KeyName;
	uint64				BagId = 0;
	uint64				HoveredId = 0;
	double				OpenedAtSeconds = 0.0;
	bool				bNoticeShowing = false;
	float				TickAccumulator = 0.f;
	FTimerHandle		NoticeTimer;

	FDelegateHandle		BagChangedHandle;
	FDelegateHandle		BagClosedHandle;
	FDelegateHandle		ItemsChangedHandle;
	FDelegateHandle		NoticeHandle;
};
