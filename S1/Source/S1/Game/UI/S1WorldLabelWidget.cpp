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
	const FLinearColor CcAmber(FColor(255, 176, 64));			// CC words - warm, apart from the teal names and the white numbers
	const FLinearColor NoticeWhite(FColor(240, 240, 240));
	constexpr float LineStepPx = 19.f;							// one label line higher per extra line (16 overlapped the name - 2026-10-11 screenshot)

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
		if (Creature == nullptr || Creature->IsDown())
			continue;

		FVector2D Position;
		if (UWidgetLayoutLibrary::ProjectWorldLocationToWidgetPosition(PC, Creature->GetActorLocation(), Position, /*bPlayerViewportRelative*/ true) == false)
			continue;

		float Lift = LiftPx + LiftPerScalePx * Creature->GetActorScale3D().X;

		if (Creature->IsMonster() && Creature->GetMonsterName().IsEmpty() == false)
		{
			FLabel& Label = Labels.AddDefaulted_GetRef();
			Label.Text = Creature->GetMonsterName();
			Label.Anchor = Position - FVector2D(0.f, Lift);
			Label.Colour = MonsterTeal;
			Lift += LineStepPx;
		}

		// Active CC in words over every creature - stun · slow · blind on a fixed dummy show nothing else (2026-10-11)
		const FText Cc = Creature->GetCcLabel();
		if (Cc.IsEmpty() == false)
		{
			FLabel& Label = Labels.AddDefaulted_GetRef();
			Label.Text = Cc;
			Label.Anchor = Position - FVector2D(0.f, Lift);
			Label.Colour = CcAmber;
			Lift += LineStepPx;
		}

		// Why a key did nothing - "대상을 클릭하세요" (owner only: only our own pawn ever sets one)
		if (const FText* Notice = Creature->GetFloatingNotice())
		{
			FLabel& Label = Labels.AddDefaulted_GetRef();
			Label.Text = *Notice;
			Label.Anchor = Position - FVector2D(0.f, Lift + 6.f);
			Label.Colour = NoticeWhite;
		}
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
			Label.Text, Font, ESlateDrawEffect::None, Label.Colour);
	}

	return Layer + 1;
}
