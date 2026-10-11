// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "S1EntryMapWidget.generated.h"

class UBorder;
class UCanvasPanel;
class UTextBlock;
class US1BaseCampSubsystem;

/**
 * S4 — 진입 지점 지도. design/ux/entry-point.md
 *
 * Opened by S3's [입장], drawn over it. Pick a point (click · 1~9) or [무작위] (R) and C_ENTER_GAME
 * goes out once; [취소] (Esc) closes and sends nothing. The map faces the way the dungeon camera does —
 * screen up is world +X, screen right is world +Y — so the place you picked is where it looked to be.
 *
 * A floor with a coast (Server/Data/floor_outline.csv — the island, 2026-10-11) gets a map in its proportions with the
 * coastline drawn; one without stays the square ± MapHalfExtentCm.
 */
UCLASS()
class S1_API US1EntryMapWidget : public UUserWidget
{
	GENERATED_BODY()

public:

	/** Close without sending anything — [취소], Esc, a rejected entry, or S3 going away. */
	void Close();

protected:

	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

private:

	US1BaseCampSubsystem* GetBaseCamp() const;

	/** Point index into Markers, or INDEX_NONE for [무작위]. Sends once; later picks are ignored. */
	void Choose(int32 MarkerIndex);
	void SetHovered(int32 MarkerIndex);
	void Refresh();

	struct FMarker
	{
		uint32	EntryId = 0;
		FText	Letter;
		FText	Name;
	};

	TArray<FMarker>										Markers;
	UPROPERTY() TArray<TObjectPtr<UBorder>>				MarkerWidgets;
	UPROPERTY() TObjectPtr<UTextBlock>					Title;
	UPROPERTY() TObjectPtr<UTextBlock>					HoverLine;
	UPROPERTY() TObjectPtr<UBorder>						RandomButton;
	UPROPERTY() TObjectPtr<UBorder>						CancelButton;

	int32		HoveredIndex = INDEX_NONE;
	int32		ChosenIndex = INDEX_NONE;		// INDEX_NONE with bRequested = [무작위]
	bool		bRequested = false;
};

/** S4's coastline — one closed line in map units, painted under the houses and markers. */
UCLASS()
class S1_API US1EntryMapCoastWidget : public UUserWidget
{
	GENERATED_BODY()

public:

	TArray<FVector2D> MapPoints;

protected:

	virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
};
