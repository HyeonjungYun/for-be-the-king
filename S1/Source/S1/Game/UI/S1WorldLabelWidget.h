// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "S1WorldLabelWidget.generated.h"

/**
 * Names over monsters, painted in screen space with the UI font (monster-ai.md § H · art bible ⑩ monster teal).
 *
 * Why not a UTextRenderComponent on the actor: its font is an offline distance-field atlas (RobotoDistanceField)
 * with no Hangul, and "떠돌이 들개" drew as boxes (2026-10-07). Slate fonts fall back to CJK, so the label is
 * projected here once a frame — one widget for every monster, no per-actor components (same idea as the radar).
 */
UCLASS()
class S1_API US1WorldLabelWidget : public UUserWidget
{
	GENERATED_BODY()

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

private:
	struct FLabel
	{
		FText		Text;
		FVector2D	Anchor = FVector2D::ZeroVector;		// widget space — bottom centre of the text
	};

	TArray<FLabel>	Labels;
};
