// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/UI/S1WorldLabelWidget.h"

#include "Blueprint/WidgetLayoutLibrary.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "Game/BaseCamp/S1BaseCampSubsystem.h"
#include "Game/S1Player.h"
#include "Game/UI/S1ItemUIStyle.h"
#include "Rendering/DrawElements.h"
#include "S1GameInstance.h"

namespace
{
	// Above the head: a fixed lift plus a share of the actor's scale, so the boss (radius 110 → ×2.6) keeps its
	// name clear of its larger silhouette. Reference units — UMG's DPI scale carries them to other resolutions
	constexpr float LiftPx = 22.f;
	constexpr float LiftPerScalePx = 12.f;

	const FLinearColor MonsterTeal(FColor(25, 158, 112));		// art bible ⑩ — #199E70

	FSlateFontInfo LabelFont()
	{
		FSlateFontInfo Font = S1ItemUIStyle::Font(S1ItemUIStyle::FontSmall);
		Font.OutlineSettings.OutlineSize = 1;
		Font.OutlineSettings.OutlineColor = FLinearColor(0.f, 0.f, 0.f, 0.75f);	// the floor is light grey — teal alone washes out
		return Font;
	}
}

void US1WorldLabelWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	SetVisibility(ESlateVisibility::HitTestInvisible);		// clicks go through to the world
}

void US1WorldLabelWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	Labels.Reset();

	const US1BaseCampSubsystem* BaseCamp = US1BaseCampSubsystem::Get();
	if (BaseCamp == nullptr || BaseCamp->GetPlace() != ES1Place::Dungeon)
		return;

	const US1GameInstance* GameInstance = Cast<US1GameInstance>(GetGameInstance());
	const APlayerController* PC = GetOwningPlayer();
	if (GameInstance == nullptr || PC == nullptr)
		return;

	for (const TPair<uint64, TWeakObjectPtr<AS1Player>>& Pair : GameInstance->Players)
	{
		const AS1Player* Creature = Pair.Value.Get();
		if (Creature == nullptr || Creature->IsMonster() == false || Creature->IsDown() || Creature->GetMonsterName().IsEmpty())
			continue;

		FVector2D Position;
		if (UWidgetLayoutLibrary::ProjectWorldLocationToWidgetPosition(PC, Creature->GetActorLocation(), Position, /*bPlayerViewportRelative*/ true) == false)
			continue;

		FLabel& Label = Labels.AddDefaulted_GetRef();
		Label.Text = Creature->GetMonsterName();
		Label.Anchor = Position - FVector2D(0.f, LiftPx + LiftPerScalePx * Creature->GetActorScale3D().X);
	}
}

int32 US1WorldLabelWidget::NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const int32 Layer = Super::NativePaint(Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId, InWidgetStyle, bParentEnabled);
	if (Labels.Num() == 0 || FSlateApplication::IsInitialized() == false)
		return Layer;

	const FSlateFontInfo Font = LabelFont();
	const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();

	for (const FLabel& Label : Labels)
	{
		const FVector2D Size = Measure->Measure(Label.Text, Font);
		const FVector2D TopLeft = Label.Anchor - FVector2D(Size.X * 0.5f, Size.Y);

		FSlateDrawElement::MakeText(OutDrawElements, Layer + 1,
			AllottedGeometry.ToPaintGeometry(Size, FSlateLayoutTransform(TopLeft)),
			Label.Text, Font, ESlateDrawEffect::None, MonsterTeal);
	}

	return Layer + 1;
}
