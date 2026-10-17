// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Rendering/RenderingCommon.h"
#include "S1FogOfWarWidget.generated.h"

/**
 * Fog of war — the dark outside our sight (design/gdd/fog-of-war.md, 2026-10-11).
 *
 * Display only: the server already withholds every creature we cannot see. This paints the ground the same way, so a
 * player reads "nobody there" as "I cannot see there", not "it is empty".
 *
 * How: a screen-space grid (cell = fog_cell_px). Every grid vertex is deprojected onto the ground plane under the local
 * player and tested against the vision sources — our own sight (radius from S_VISION, walls from the server's 2D boxes
 * via ray casts) plus any extra sources (shadow clone, paper hawk, beast search). The vertex alpha is 0 inside, fog_alpha
 * outside, with a short ramp at the edge; Slate interpolates across each cell so the edge comes out soft. One
 * MakeCustomVerts call, no materials — same approach as the blind overlay, so nothing here needs an asset.
 */
UCLASS()
class S1_API US1FogOfWarWidget : public UUserWidget
{
	GENERATED_BODY()

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

private:
	/** One vision source resolved for this frame — position, reach, and (if walls matter) a ray table. */
	struct FSourceView
	{
		FVector2D Pos = FVector2D::ZeroVector;
		float RadiusCm = 0.f;
		TArray<float> Rays;				// empty = plain circle (sees over walls)
	};

	/** 0 = fully seen, 1 = fully fogged, for a ground point. */
	float FogAt(const FVector2D& Ground) const;

	TArray<FSourceView> Views;

	/** Grid in widget-local units, rebuilt every tick; NativePaint only transforms and draws. */
	TArray<FVector2f> GridLocal;
	TArray<FColor> GridColour;
	mutable TArray<FSlateVertex> VertexScratch;		// NativePaint 가 매 프레임 채운다 - 할당을 다시 쓴다
	TArray<SlateIndex> Indices;
	int32 Cols = 0;
	int32 Rows = 0;

	bool bWasInDungeon = false;
	bool bHasFog = false;
};
