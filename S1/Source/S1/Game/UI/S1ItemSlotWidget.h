// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "InputCoreTypes.h"
#include "Game/Item/S1ItemTypes.h"
#include "S1ItemSlotWidget.generated.h"

class USizeBox;
class UBorder;
class UTextBlock;

DECLARE_DELEGATE_ThreeParams(FOnS1SlotClicked, uint64 /*InstanceId, 0 = empty cell*/, FKey /*Button*/, bool /*bCtrl*/);
DECLARE_DELEGATE_OneParam(FOnS1SlotHovered, uint64 /*InstanceId, 0 = the cursor left*/);

/**
 * One cell — a grid square or an equipment square.
 *
 * The grade is drawn as the outline's shape (rectangle · chamfered · hexagon · star) and backed
 * up by color, so a grayscale screenshot still tells grades apart (AC-UI-23). Clicks are always
 * Handled: a click on the panel must never fall through to the game as a skill confirm.
 */
UCLASS()
class S1_API US1ItemSlotWidget : public UUserWidget
{
	GENERATED_BODY()

public:

	/** Call before the widget is first shown. */
	void SetCellSize(float InSize) { CellSize = InSize; }

	void SetItem(uint64 InInstanceId, ES1ItemGrade InGrade, const FText& InLabel, bool bInPending);

	/** bDotted — an empty key slot (no skill). Otherwise a plain empty cell. */
	void SetEmpty(bool bDotted);

	/** 150 ms horizontal shake on a rejected action. */
	void Shake();

	uint64 GetInstanceId() const { return InstanceId; }

	FOnS1SlotClicked OnClicked;
	FOnS1SlotHovered OnHovered;

protected:

	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual void NativeOnMouseEnter(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual void NativeOnMouseLeave(const FPointerEvent& InMouseEvent) override;

private:

	void UpdateBackground();

	UPROPERTY() TObjectPtr<USizeBox>	Box;
	UPROPERTY() TObjectPtr<UBorder>		Background;
	UPROPERTY() TObjectPtr<UTextBlock>	Label;

	float			CellSize = 64.f;
	uint64			InstanceId = 0;
	ES1ItemGrade	Grade = ES1ItemGrade::None;
	bool			bDotted = false;
	bool			bHovered = false;
	double			ShakeStartSeconds = -1.0;
};
