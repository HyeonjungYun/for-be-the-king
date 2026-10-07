// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/UI/S1RunHudWidget.h"
#include "Game/UI/S1ItemUIStyle.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ProgressBar.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"

using namespace S1ItemUIStyle;

namespace
{
	// Tokens — art-bible § 4 ⑤ UI palette. Urgent is the warning amber the item UI uses for discard
	const FLinearColor UrgentColor(FColor(232, 160, 60));
	const FLinearColor GaugeFill(FColor(220, 200, 120));
	const FLinearColor GaugeDrain(FColor(150, 140, 110));

	void SetLine(UTextBlock* Line, const FText& Text)
	{
		if (Line == nullptr)
			return;

		Line->SetText(Text);
		Line->SetVisibility(Text.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	}
}

UTextBlock* US1RunHudWidget::MakeLine(int32 FontSize)
{
	UTextBlock* Line = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Line->SetFont(Font(FontSize));
	Line->SetColorAndOpacity(FSlateColor(TextPrimary()));
	Line->SetShadowOffset(FVector2D(1.f, 1.f));
	Line->SetShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.8f));
	Line->SetJustification(ETextJustify::Center);
	Line->SetVisibility(ESlateVisibility::Collapsed);

	if (UVerticalBoxSlot* LineSlot = Column->AddChildToVerticalBox(Line))
	{
		LineSlot->SetHorizontalAlignment(HAlign_Center);
		LineSlot->SetPadding(FMargin(0.f, 2.f));
	}
	return Line;
}

void US1RunHudWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	UOverlay* Root = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass());
	WidgetTree->RootWidget = Root;

	Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	if (UOverlaySlot* ColumnSlot = Root->AddChildToOverlay(Column))
	{
		// Top centre — the radar takes the top-right band, the item panels the sides (item-ui.md § Layout Zones)
		ColumnSlot->SetHorizontalAlignment(HAlign_Center);
		ColumnSlot->SetVerticalAlignment(VAlign_Top);
		ColumnSlot->SetPadding(FMargin(0.f, 10.f, 0.f, 0.f));
	}

	TimerText = MakeLine(FontEmphasis + 5);
	WarningText = MakeLine(FontEmphasis);
	WarningText->SetColorAndOpacity(FSlateColor(UrgentColor));
	GaugeText = MakeLine(FontBody);

	USizeBox* BarBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	BarBox->SetWidthOverride(260.f);
	BarBox->SetHeightOverride(10.f);
	GaugeBar = WidgetTree->ConstructWidget<UProgressBar>(UProgressBar::StaticClass());
	GaugeBar->SetFillColorAndOpacity(GaugeFill);
	BarBox->AddChild(GaugeBar);
	BarBox->SetVisibility(ESlateVisibility::Collapsed);
	if (UVerticalBoxSlot* BarSlot = Column->AddChildToVerticalBox(BarBox))
	{
		BarSlot->SetHorizontalAlignment(HAlign_Center);
		BarSlot->SetPadding(FMargin(0.f, 2.f));
	}

	AlarmText = MakeLine(FontBody);
	ResultText = MakeLine(FontEmphasis + 3);

	SetVisibility(ESlateVisibility::HitTestInvisible);
}

void US1RunHudWidget::SetTimer(const FText& Text, bool bUrgent)
{
	SetLine(TimerText, Text);
	if (TimerText != nullptr)
		TimerText->SetColorAndOpacity(FSlateColor(bUrgent ? UrgentColor : TextPrimary()));
}

void US1RunHudWidget::SetWarning(const FText& Text)
{
	SetLine(WarningText, Text);
}

void US1RunHudWidget::SetGauge(bool bVisible, float Progress, const FText& Label, bool bDraining)
{
	SetLine(GaugeText, bVisible ? Label : FText::GetEmpty());
	if (GaugeBar == nullptr)
		return;

	GaugeBar->SetPercent(Progress);
	GaugeBar->SetFillColorAndOpacity(bDraining ? GaugeDrain : GaugeFill);
	if (UWidget* Box = GaugeBar->GetParent())
		Box->SetVisibility(bVisible ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
}

void US1RunHudWidget::SetAlarm(const FText& Text, bool bSelf)
{
	SetLine(AlarmText, Text);
	if (AlarmText != nullptr)
		AlarmText->SetColorAndOpacity(FSlateColor(bSelf ? UrgentColor : TextSecondary()));
}

void US1RunHudWidget::SetResult(const FText& Text)
{
	SetLine(ResultText, Text);
}
