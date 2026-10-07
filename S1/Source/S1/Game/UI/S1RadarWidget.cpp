// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/UI/S1RadarWidget.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Game/BaseCamp/S1BaseCampSubsystem.h"
#include "Game/Escape/S1EscapeSubsystem.h"
#include "Game/Item/S1ItemSubsystem.h"
#include "Game/S1Player.h"
#include "Rendering/DrawElements.h"
#include "S1GameInstance.h"
#include "Styling/CoreStyle.h"

namespace
{
	// radar.md § Tuning Knobs — reference units at 1080p; UMG's DPI scale carries them to 720p / 1440p
	constexpr float RadiusCm = 2250.f;			// registry radar_radius 22.5 m
	constexpr float FadeCm = 150.f;
	constexpr float RingDiameter = 128.f;
	constexpr float RingPadding = 9.f;				// half the largest icon
	constexpr float Margin = 8.f;
	constexpr float TowerSize = 18.f;
	constexpr float ChestSize = 14.f;
	constexpr float PartySize = 14.f;
	constexpr float SelfSize = 8.f;

	// Neutral tokens. 🔴 Never the enemy (#D95926) or monster (#199E70) colours — shape is the code (AC-RD-13)
	const FLinearColor DiscColor(0.06f, 0.06f, 0.055f, 0.55f);
	const FLinearColor RimColor(FColor(195, 194, 183, 160));
	const FLinearColor TowerColor(FColor(240, 236, 220));
	const FLinearColor ChestColor(FColor(220, 186, 96));
	const FLinearColor PartyColor(FColor(150, 190, 230));
	const FLinearColor SelfColor(FColor(255, 255, 255));

	void Polyline(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry, const TArray<FVector2D>& Points, const FLinearColor& Color, float Thickness)
	{
		FSlateDrawElement::MakeLines(Out, Layer, Geometry.ToPaintGeometry(), Points, ESlateDrawEffect::None, Color, true, Thickness);
	}
}

void US1RadarWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	SetVisibility(ESlateVisibility::HitTestInvisible);		// clicks go through to the world (AC-RD-21)
}

void US1RadarWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	Blips.Reset();
	bShowing = false;

	const US1BaseCampSubsystem* BaseCamp = US1BaseCampSubsystem::Get();
	const US1GameInstance* GameInstance = Cast<US1GameInstance>(GetGameInstance());
	const AS1Player* Me = GameInstance != nullptr ? GameInstance->MyPlayer.Get() : nullptr;

	const bool bInDungeon = BaseCamp != nullptr && BaseCamp->GetPlace() == ES1Place::Dungeon;
	const bool bAlive = BaseCamp != nullptr && BaseCamp->IsLocalDead() == false;
	if (S1RadarLogic::IsRadarVisible(bInDungeon, bAlive, Me != nullptr, /*bEscaping*/ false) == false)
		return;

	FS1RadarInput Input;
	Input.Self = FVector2D(Me->GetActorLocation());
	Input.RadiusCm = RadiusCm;
	Input.FadeCm = FadeCm;
	Input.RingRadiusPx = RingDiameter * 0.5f;

	if (const US1EscapeSubsystem* Escape = US1EscapeSubsystem::Get())
	{
		for (const FS1TowerView& Tower : Escape->GetTowers())
			Input.Towers.Emplace(Tower.WireId, Tower.Position);
	}

	// Only chests ever enter the input — players and monsters are not a kind the radar can hold (radar.md D2)
	if (const US1ItemSubsystem* Items = US1ItemSubsystem::Get())
	{
		for (const TPair<uint64, FS1BagView>& Pair : Items->GetBags())
		{
			if (Pair.Value.bHasLocation == false || S1RadarLogic::IsChestObjectId(Pair.Key) == false)
				continue;

			FS1RadarChest& Chest = Input.Chests.AddDefaulted_GetRef();
			Chest.Id = Pair.Key;
			Chest.Position = FVector2D(Pair.Value.Location);
			Chest.bKnownLooted = Items->IsChestLooted(Pair.Key);
		}
	}

	Blips = S1RadarLogic::CollectBlips(Input);
	bShowing = true;
}

int32 US1RadarWidget::NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const int32 Layer = Super::NativePaint(Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId, InWidgetStyle, bParentEnabled);
	if (bShowing == false)
		return Layer;

	// Top-right corner of the viewport, in reference units (the widget fills the viewport)
	const FVector2D Size = AllottedGeometry.GetLocalSize();
	const float Widget = RingDiameter + 2.f * RingPadding;
	const FVector2D Centre(Size.X - Margin - Widget * 0.5f, Margin + Widget * 0.5f);
	const float R = RingDiameter * 0.5f;

	// Disc and rim
	static const FSlateRoundedBoxBrush DiscBrush(FLinearColor::White, R);
	FSlateDrawElement::MakeBox(OutDrawElements, Layer + 1,
		AllottedGeometry.ToPaintGeometry(FVector2D(RingDiameter, RingDiameter), FSlateLayoutTransform(Centre - FVector2D(R, R))),
		&DiscBrush, ESlateDrawEffect::None, DiscColor);

	TArray<FVector2D> Rim;
	for (int32 i = 0; i <= 48; i++)
	{
		const float A = 2.f * PI * i / 48.f;
		Rim.Add(Centre + FVector2D(FMath::Cos(A), FMath::Sin(A)) * R);
	}
	Polyline(OutDrawElements, Layer + 2, AllottedGeometry, Rim, RimColor, 1.5f);

	const FSlateBrush* White = FCoreStyle::Get().GetBrush(TEXT("GenericWhiteBox"));
	static const FSlateRoundedBoxBrush DotBrush(FLinearColor::White, PartySize * 0.5f);

	for (const FS1RadarBlip& Blip : Blips)
	{
		const FVector2D P = Centre + Blip.OffsetPx;
		switch (Blip.Kind)
		{
		case ES1RadarBlip::Tower:		// hollow diamond
		{
			const float H = TowerSize * 0.5f;
			TArray<FVector2D> Diamond = { P + FVector2D(0, -H), P + FVector2D(H, 0), P + FVector2D(0, H), P + FVector2D(-H, 0), P + FVector2D(0, -H) };
			Polyline(OutDrawElements, Layer + 3, AllottedGeometry, Diamond, TowerColor * FLinearColor(1, 1, 1, Blip.Alpha), 2.f);
			break;
		}
		case ES1RadarBlip::Chest:		// filled square
		{
			const float H = ChestSize * 0.5f;
			FSlateDrawElement::MakeBox(OutDrawElements, Layer + 3,
				AllottedGeometry.ToPaintGeometry(FVector2D(ChestSize, ChestSize), FSlateLayoutTransform(P - FVector2D(H, H))),
				White, ESlateDrawEffect::None, ChestColor * FLinearColor(1, 1, 1, Blip.Alpha));
			break;
		}
		case ES1RadarBlip::Party:		// filled circle
		{
			const float H = PartySize * 0.5f;
			FSlateDrawElement::MakeBox(OutDrawElements, Layer + 3,
				AllottedGeometry.ToPaintGeometry(FVector2D(PartySize, PartySize), FSlateLayoutTransform(P - FVector2D(H, H))),
				&DotBrush, ESlateDrawEffect::None, PartyColor * FLinearColor(1, 1, 1, Blip.Alpha));
			break;
		}
		}
	}

	// Self — a small plus at the centre
	const float S = SelfSize * 0.5f;
	Polyline(OutDrawElements, Layer + 4, AllottedGeometry, { Centre - FVector2D(S, 0), Centre + FVector2D(S, 0) }, SelfColor, 2.f);
	Polyline(OutDrawElements, Layer + 4, AllottedGeometry, { Centre - FVector2D(0, S), Centre + FVector2D(0, S) }, SelfColor, 2.f);

	return Layer + 4;
}
