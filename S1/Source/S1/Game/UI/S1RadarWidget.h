// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Game/Radar/S1RadarLogic.h"
#include "S1RadarWidget.generated.h"

/**
 * The radar disc in the top-right corner (design/gdd/radar.md § UI · Z-Rd). North is up and never turns —
 * the camera is fixed. It draws exactly what S1RadarLogic::CollectBlips returns: towers, sealed chests,
 * party members. Painted directly (no child widgets) so it costs a handful of draw elements (AC-RD-26).
 */
UCLASS()
class S1_API US1RadarWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** The last frame's blips and how many players / monsters were in range but not drawn — for DebugRadar. */
	const TArray<FS1RadarBlip>& GetBlips() const { return Blips; }
	bool IsShowing() const { return bShowing; }

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

private:
	TArray<FS1RadarBlip>	Blips;
	bool					bShowing = false;
};
