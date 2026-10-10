// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/UI/S1BlindOverlayWidget.h"

#include "Blueprint/WidgetLayoutLibrary.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Game/S1Player.h"
#include "GameFramework/PlayerController.h"
#include "Rendering/DrawElements.h"
#include "S1GameInstance.h"

namespace
{
	// Half the normal view width — AS1MyPlayer's camera (arm 2600, FOV 60) shows about 30 m across.
	// blind_view_scale multiplies this: 0.15 leaves about 2.3 m around the player.
	constexpr float BaseViewRadiusCm = 1500.f;

	constexpr int32 FeatherSteps = 6;			// soft edge: rings stacked over the feather width, then one solid ring
	constexpr float FeatherAlpha = 0.35f;		// 1 - 0.65^6 ≈ 0.92 at the last soft ring, black beyond it
	constexpr float FeatherOfRadius = 0.6f;		// feather width as a fraction of the clear radius
	constexpr float MinClearRadius = 8.f;		// widget units — the circle never collapses to a point

	// Fast in (the hit lands), slow out (sight returning) — both follow the server's blind deadline, nothing predicted
	constexpr float FadeInSpeed = 12.f;
	constexpr float FadeOutSpeed = 3.f;
}

void US1BlindOverlayWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	SetVisibility(ESlateVisibility::HitTestInvisible);		// clicks still reach the world
}

void US1BlindOverlayWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	const US1GameInstance* GameInstance = Cast<US1GameInstance>(GetGameInstance());
	const AS1Player* Me = GameInstance != nullptr ? GameInstance->MyPlayer.Get() : nullptr;
	const APlayerController* PC = GetOwningPlayer();

	const bool bBlind = Me != nullptr && Me->IsBlinded();
	Strength = FMath::FInterpTo(Strength, bBlind ? 1.f : 0.f, InDeltaTime, bBlind ? FadeInSpeed : FadeOutSpeed);

	Rings.Reset();
	RingHalfSizes.Reset();

	if (Strength < 0.01f || Me == nullptr || PC == nullptr)
		return;

	// Screen right is world +Y (the camera looks along +X, pitched down), so a +Y offset gives the radius without foreshortening
	const FVector Feet = Me->GetActorLocation();
	const float RadiusCm = BaseViewRadiusCm * FMath::Clamp(Me->GetBlindViewScale(), 0.05f, 1.f);

	FVector2D Edge;
	if (UWidgetLayoutLibrary::ProjectWorldLocationToWidgetPosition(PC, Feet, Centre, false) == false
		|| UWidgetLayoutLibrary::ProjectWorldLocationToWidgetPosition(PC, Feet + FVector(0.f, RadiusCm, 0.f), Edge, false) == false)
		return;

	const float Clear = FMath::Max(MinClearRadius, static_cast<float>(FVector2D::Distance(Centre, Edge)));
	const float Feather = Clear * FeatherOfRadius;

	// The outline has to reach every corner of the screen from wherever the player stands
	const FVector2D Size = MyGeometry.GetLocalSize();
	const float Cover = 2.f * static_cast<float>(Size.X + Size.Y);

	for (int32 i = 0; i <= FeatherSteps; i++)
	{
		const float Inner = Clear + Feather * static_cast<float>(i) / FeatherSteps;
		const float Alpha = (i == FeatherSteps) ? 1.f : FeatherAlpha;
		const float Half = Inner + Cover;		// circle radius — the outline is drawn inside it and fills [Inner, Half]

		Rings.Add(FSlateRoundedBoxBrush(FLinearColor::Transparent, Half, FLinearColor(0.f, 0.f, 0.f, Alpha * Strength), Cover));
		RingHalfSizes.Add(Half);
	}
}

int32 US1BlindOverlayWidget::NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const int32 Layer = Super::NativePaint(Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId, InWidgetStyle, bParentEnabled);
	if (Rings.Num() == 0)
		return Layer;

	// Black over black blends the same in any order, so every ring can share one layer
	for (int32 i = 0; i < Rings.Num(); i++)
	{
		const float Half = RingHalfSizes[i];
		FSlateDrawElement::MakeBox(OutDrawElements, Layer + 1,
			AllottedGeometry.ToPaintGeometry(FVector2D(2.f * Half, 2.f * Half), FSlateLayoutTransform(Centre - FVector2D(Half, Half))),
			&Rings[i], ESlateDrawEffect::None, FLinearColor::White);
	}

	return Layer + 1;
}
