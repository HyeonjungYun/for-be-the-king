// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Styling/SlateBrush.h"
#include "S1BlindOverlayWidget.generated.h"

/**
 * Blind (soft CC, CC_TYPE_BLIND) — everything but a small circle around the local player goes black.
 * User decision 2026-10-11 ("플레이어 주위를 까맣게 칠해서 주변을 안 보이게"), replacing the stage-4 camera shrink.
 *
 * How much stays visible comes from the server: CcEventInfo.magnitude = skill_rules blind_view_scale, read as a fraction
 * of the normal view radius. When FoW lands the same scale shrinks the vision radius itself ("주변을 전부 암흑시야로");
 * this widget stays as the look. Painted directly like the radar — a few rounded boxes, no child widgets.
 */
UCLASS()
class S1_API US1BlindOverlayWidget : public UUserWidget
{
	GENERATED_BODY()

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

private:
	/** 0 = clear, 1 = black outside the circle. Rises fast when the blind lands, falls slowly as sight returns. */
	float Strength = 0.f;

	/** Circle centre in widget units (the widget fills the viewport). */
	FVector2D Centre = FVector2D::ZeroVector;

	/**
	 * One ring per feather step, outermost solid. Each is a circle whose outline (drawn inside the edge) is so thick it
	 * covers the whole screen outside its inner radius. Rebuilt every tick: Slate takes the outline colour from the
	 * brush, not the tint, so the fade has to live in the brush.
	 */
	TArray<FSlateBrush> Rings;
	TArray<float> RingHalfSizes;
};
