// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/UI/S1ItemSlotWidget.h"
#include "Game/UI/S1ItemUIStyle.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Rendering/DrawElementTypes.h"

namespace
{
	constexpr double ShakeSeconds = 0.15;		// art-bible § 7 ⑧ — item feedback 150 ms
	constexpr float ShakeAmplitude = 2.f;
	constexpr float PendingOpacity = 0.45f;
	constexpr float Inset = 2.f;

	void Close(TArray<FVector2f>& Points)
	{
		// Copy first: Add may reallocate, and a reference into the old storage would dangle.
		if (Points.Num() > 0)
		{
			const FVector2f First = Points[0];
			Points.Add(First);
		}
	}

	/** The outline for a grade. Shape carries the grade; color only backs it up. */
	TArray<FVector2f> OutlineFor(ES1ItemGrade Grade, const FVector2f& Size, float Grow)
	{
		const float L = Inset - Grow;
		const float T = Inset - Grow;
		const float R = Size.X - Inset + Grow;
		const float B = Size.Y - Inset + Grow;
		const float W = R - L;
		const float H = B - T;

		TArray<FVector2f> P;

		switch (Grade)
		{
		case ES1ItemGrade::Rare:		// Chamfered corners
		{
			const float C = FMath::Min(W, H) * 0.18f;
			P = { {L + C, T}, {R - C, T}, {R, T + C}, {R, B - C}, {R - C, B}, {L + C, B}, {L, B - C}, {L, T + C} };
			break;
		}
		case ES1ItemGrade::Epic:		// Hexagon, points left and right
		{
			const float C = W * 0.2f;
			P = { {L + C, T}, {R - C, T}, {R, T + H * 0.5f}, {R - C, B}, {L + C, B}, {L, T + H * 0.5f} };
			break;
		}
		case ES1ItemGrade::Legendary:	// Five-pointed star
		{
			const FVector2f Center(L + W * 0.5f, T + H * 0.5f);
			const float Outer = FMath::Min(W, H) * 0.5f;
			const float Inner = Outer * 0.55f;
			for (int32 i = 0; i < 10; i++)
			{
				const float Angle = static_cast<float>(-UE_HALF_PI + i * (UE_PI / 5.0));
				const float Radius = (i % 2 == 0) ? Outer : Inner;
				P.Add(Center + FVector2f(FMath::Cos(Angle), FMath::Sin(Angle)) * Radius);
			}
			break;
		}
		default:						// Common and empty — rectangle
			P = { {L, T}, {R, T}, {R, B}, {L, B} };
			break;
		}

		Close(P);
		return P;
	}
}

void US1ItemSlotWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	Box = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Box->SetWidthOverride(CellSize);
	Box->SetHeightOverride(CellSize);

	Background = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Background->SetPadding(FMargin(4.f));
	Background->SetHorizontalAlignment(HAlign_Center);
	Background->SetVerticalAlignment(VAlign_Center);

	Label = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Label->SetFont(S1ItemUIStyle::Font(S1ItemUIStyle::FontSmall));
	Label->SetJustification(ETextJustify::Center);
	Label->SetAutoWrapText(true);
	Label->SetVisibility(ESlateVisibility::HitTestInvisible);

	Background->SetContent(Label);
	Box->SetContent(Background);
	WidgetTree->RootWidget = Box;

	UpdateBackground();
}

void US1ItemSlotWidget::SetItem(uint64 InInstanceId, ES1ItemGrade InGrade, const FText& InLabel, bool bInPending)
{
	InstanceId = InInstanceId;
	Grade = InGrade;
	bDotted = false;

	if (Label != nullptr)
	{
		Label->SetText(InLabel);
		Label->SetColorAndOpacity(FSlateColor(S1ItemUIStyle::TextPrimary()));
	}

	// Waiting on the server: dim it. Nothing changes until the answer arrives (no optimistic update).
	SetRenderOpacity(bInPending ? PendingOpacity : 1.f);
	UpdateBackground();
}

void US1ItemSlotWidget::SetEmpty(bool bInDotted)
{
	InstanceId = 0;
	Grade = ES1ItemGrade::None;
	bDotted = bInDotted;

	if (Label != nullptr)
	{
		// An empty key reads "—" so it is not mistaken for a Shadowed one (grey name).
		Label->SetText(bInDotted ? FText::AsCultureInvariant(TEXT("—")) : FText::GetEmpty());
		Label->SetColorAndOpacity(FSlateColor(S1ItemUIStyle::TextTertiary()));
	}

	SetRenderOpacity(1.f);
	UpdateBackground();
}

void US1ItemSlotWidget::Shake()
{
	ShakeStartSeconds = FPlatformTime::Seconds();
}

void US1ItemSlotWidget::UpdateBackground()
{
	if (Background != nullptr)
		Background->SetBrushColor(bHovered && InstanceId != 0 ? S1ItemUIStyle::CellHover() : S1ItemUIStyle::CellBackground());
}

void US1ItemSlotWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	if (ShakeStartSeconds < 0.0)
		return;

	const double Elapsed = FPlatformTime::Seconds() - ShakeStartSeconds;
	if (Elapsed >= ShakeSeconds)
	{
		ShakeStartSeconds = -1.0;
		SetRenderTranslation(FVector2D::ZeroVector);
		return;
	}

	// Two full swings inside the window — a nudge, not a flash (≤ 3 Hz rule does not apply to motion,
	// but it stays small: 2 px).
	const double Phase = Elapsed / ShakeSeconds * 2.0 * UE_TWO_PI;
	SetRenderTranslation(FVector2D(FMath::Sin(Phase) * ShakeAmplitude, 0.0));
}

int32 US1ItemSlotWidget::NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	int32 Layer = Super::NativePaint(Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId, InWidgetStyle, bParentEnabled);

	const FVector2f Size = FVector2f(AllottedGeometry.GetLocalSize());
	const FPaintGeometry Paint = AllottedGeometry.ToPaintGeometry();

	if (InstanceId == 0)
	{
		const FLinearColor Color = S1ItemUIStyle::Divider();

		if (bDotted)
		{
			// Dashed rectangle — an empty key.
			const TArray<FVector2f> Corners = OutlineFor(ES1ItemGrade::None, Size, 0.f);
			for (int32 Edge = 0; Edge + 1 < Corners.Num(); Edge++)
			{
				const FVector2f From = Corners[Edge];
				const FVector2f To = Corners[Edge + 1];
				const float Length = FVector2f::Distance(From, To);
				const int32 Dashes = FMath::Max(1, FMath::FloorToInt(Length / 8.f));

				for (int32 d = 0; d < Dashes; d++)
				{
					const float A = static_cast<float>(d) / Dashes;
					const float Bt = (static_cast<float>(d) + 0.5f) / Dashes;
					FSlateDrawElement::MakeLines(OutDrawElements, Layer + 1, Paint,
						TArray<FVector2f>{ FMath::Lerp(From, To, A), FMath::Lerp(From, To, Bt) },
						ESlateDrawEffect::None, Color, true, 1.f);
				}
			}
		}
		else
		{
			FSlateDrawElement::MakeLines(OutDrawElements, Layer + 1, Paint, OutlineFor(ES1ItemGrade::None, Size, 0.f),
				ESlateDrawEffect::None, Color, true, 1.f);
		}

		return Layer + 1;
	}

	const FLinearColor Color = S1ItemUIStyle::GradeColor(Grade);
	FSlateDrawElement::MakeLines(OutDrawElements, Layer + 1, Paint, OutlineFor(Grade, Size, 0.f),
		ESlateDrawEffect::None, Color, true, 2.f);

	// Epic and Legendary "glow" — a faint second outline just outside. Shape, not a gradient (§ 7 ④).
	if (Grade == ES1ItemGrade::Epic || Grade == ES1ItemGrade::Legendary)
	{
		FSlateDrawElement::MakeLines(OutDrawElements, Layer + 1, Paint, OutlineFor(Grade, Size, 2.f),
			ESlateDrawEffect::None, Color.CopyWithNewOpacity(0.35f), true, 1.f);
	}

	return Layer + 1;
}

FReply US1ItemSlotWidget::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	OnClicked.ExecuteIfBound(InstanceId, InMouseEvent.GetEffectingButton(), InMouseEvent.IsControlDown());

	// Always handled — otherwise the click reaches the game and confirms an aimed skill.
	return FReply::Handled();
}

void US1ItemSlotWidget::NativeOnMouseEnter(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	Super::NativeOnMouseEnter(InGeometry, InMouseEvent);

	bHovered = true;
	UpdateBackground();
	OnHovered.ExecuteIfBound(InstanceId);
}

void US1ItemSlotWidget::NativeOnMouseLeave(const FPointerEvent& InMouseEvent)
{
	Super::NativeOnMouseLeave(InMouseEvent);

	bHovered = false;
	UpdateBackground();
	OnHovered.ExecuteIfBound(0);
}
