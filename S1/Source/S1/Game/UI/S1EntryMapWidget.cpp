// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/UI/S1EntryMapWidget.h"
#include "Game/BaseCamp/S1BaseCampSubsystem.h"
#include "Game/UI/S1ItemUIStyle.h"
#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/GameInstance.h"
#include "InputCoreTypes.h"

using namespace S1ItemUIStyle;

// Named, not anonymous — a unity build puts this file next to the other widgets.
namespace S1EntryMapLayout
{
	constexpr float MapSize = 460.f;		// 1080p units — fits the 720p height with the title and buttons
	constexpr float MarkerSize = 36.f;
	constexpr int32 MaxNumberKeys = 9;

	/** World → map. Screen up is world +X, screen right is world +Y — the dungeon camera's way. */
	FVector2D ToMap(const FS1EntryPoint& Point, float HalfExtentCm)
	{
		const float U = (Point.Y + HalfExtentCm) / (2.f * HalfExtentCm);
		const float V = (HalfExtentCm - Point.X) / (2.f * HalfExtentCm);
		return FVector2D(FMath::Clamp(U, 0.f, 1.f) * MapSize, FMath::Clamp(V, 0.f, 1.f) * MapSize);
	}

	const FKey& NumberKey(int32 Index)
	{
		static const FKey Keys[MaxNumberKeys] = { EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four, EKeys::Five,
			EKeys::Six, EKeys::Seven, EKeys::Eight, EKeys::Nine };
		return Keys[Index];
	}

	const FKey& NumPadKey(int32 Index)
	{
		static const FKey Keys[MaxNumberKeys] = { EKeys::NumPadOne, EKeys::NumPadTwo, EKeys::NumPadThree, EKeys::NumPadFour,
			EKeys::NumPadFive, EKeys::NumPadSix, EKeys::NumPadSeven, EKeys::NumPadEight, EKeys::NumPadNine };
		return Keys[Index];
	}
}

US1BaseCampSubsystem* US1EntryMapWidget::GetBaseCamp() const
{
	UGameInstance* GameInstance = GetGameInstance();
	return GameInstance != nullptr ? GameInstance->GetSubsystem<US1BaseCampSubsystem>() : nullptr;
}

// ── Build ────────────────────────────────────────────────────────────────

void US1EntryMapWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	// Number keys, R and Esc land here — S3 hands keyboard focus over when it opens us
	SetIsFocusable(true);

	using namespace S1EntryMapLayout;

	auto MakeText = [this](int32 Size, const FLinearColor& Color) -> UTextBlock*
	{
		UTextBlock* Text = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Text->SetFont(Font(Size));
		Text->SetColorAndOpacity(FSlateColor(Color));
		return Text;
	};

	auto MakeButton = [this, &MakeText](const FText& Label) -> UBorder*
	{
		UBorder* Button = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
		Button->SetBrushColor(CellHover());
		Button->SetPadding(FMargin(18.f, 6.f));
		UTextBlock* Text = MakeText(FontBody, TextPrimary());
		Text->SetText(Label);
		Button->SetContent(Text);
		return Button;
	};

	// The whole screen, dimmed — S3 stays visible underneath but takes no clicks while S4 is up
	UBorder* Dim = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Dim->SetBrushColor(FLinearColor(0.f, 0.f, 0.f, 0.65f));
	Dim->SetHorizontalAlignment(HAlign_Center);
	Dim->SetVerticalAlignment(VAlign_Center);
	WidgetTree->RootWidget = Dim;

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Dim->SetContent(Column);

	Title = MakeText(FontEmphasis, TextPrimary());
	if (UVerticalBoxSlot* TitleSlot = Column->AddChildToVerticalBox(Title))
	{
		TitleSlot->SetHorizontalAlignment(HAlign_Center);
		TitleSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 10.f));
	}

	// The floor — a square until the real map has a picture
	USizeBox* MapBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	MapBox->SetWidthOverride(MapSize);
	MapBox->SetHeightOverride(MapSize);

	UBorder* MapFrame = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	MapFrame->SetBrush(FSlateRoundedBoxBrush(PanelBackground(), 4.f, Divider(), 2.f));
	MapFrame->SetPadding(FMargin(0.f));
	MapBox->SetContent(MapFrame);

	UCanvasPanel* Canvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass());
	MapFrame->SetContent(Canvas);

	if (UVerticalBoxSlot* MapSlot = Column->AddChildToVerticalBox(MapBox))
		MapSlot->SetHorizontalAlignment(HAlign_Center);

	const US1BaseCampSubsystem* BaseCamp = GetBaseCamp();
	const float HalfExtent = BaseCamp != nullptr ? FMath::Max(BaseCamp->GetMapHalfExtentCm(), 1.f) : 3000.f;
	const TArray<FS1EntryPoint> Points = BaseCamp != nullptr ? BaseCamp->GetEntryPoints().OnFloor(BaseCamp->GetFloorId()) : TArray<FS1EntryPoint>();

	for (int32 i = 0; i < Points.Num(); i++)
	{
		FMarker Marker;
		Marker.EntryId = Points[i].EntryId;
		Marker.Letter = FText::FromString(FString::Chr(TEXT('A') + (i % 26)));
		Marker.Name = Text(*FString::Printf(TEXT("entry.%u"), Points[i].EntryId));
		Markers.Add(Marker);

		const FVector2D At = ToMap(Points[i], HalfExtent);

		UBorder* Circle = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
		Circle->SetBrush(FSlateRoundedBoxBrush(CellHover(), MarkerSize * 0.5f, TextTertiary(), 1.5f));
		Circle->SetHorizontalAlignment(HAlign_Center);
		Circle->SetVerticalAlignment(VAlign_Center);
		UTextBlock* Letter = MakeText(FontEmphasis, TextPrimary());
		Letter->SetText(Marker.Letter);
		Circle->SetContent(Letter);

		if (UCanvasPanelSlot* CircleSlot = Canvas->AddChildToCanvas(Circle))
		{
			CircleSlot->SetAutoSize(false);
			CircleSlot->SetSize(FVector2D(MarkerSize));
			CircleSlot->SetAlignment(FVector2D(0.5f, 0.5f));
			CircleSlot->SetPosition(At);
		}
		MarkerWidgets.Add(Circle);

		// The number key under the circle — color is never the only cue
		if (i < MaxNumberKeys)
		{
			UTextBlock* KeyLabel = MakeText(FontSmall, TextTertiary());
			KeyLabel->SetText(FText::AsNumber(i + 1));
			KeyLabel->SetVisibility(ESlateVisibility::HitTestInvisible);
			if (UCanvasPanelSlot* KeySlot = Canvas->AddChildToCanvas(KeyLabel))
			{
				KeySlot->SetAutoSize(true);
				KeySlot->SetAlignment(FVector2D(0.5f, 0.f));
				KeySlot->SetPosition(At + FVector2D(0.f, MarkerSize * 0.5f + 2.f));
			}
		}
	}

	if (Points.Num() == 0)
		UE_LOG(LogTemp, Error, TEXT("[BASECAMP] S4 has no entry point on floor %u - only [random] works"), BaseCamp != nullptr ? BaseCamp->GetFloorId() : 0u);

	HoverLine = MakeText(FontBody, TextSecondary());
	if (UVerticalBoxSlot* HoverSlot = Column->AddChildToVerticalBox(HoverLine))
	{
		HoverSlot->SetHorizontalAlignment(HAlign_Center);
		HoverSlot->SetPadding(FMargin(0.f, 10.f, 0.f, 10.f));
	}

	UHorizontalBox* Buttons = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	RandomButton = MakeButton(Text(TEXT("label.entry_random")));
	CancelButton = MakeButton(Text(TEXT("label.entry_cancel")));
	if (UHorizontalBoxSlot* RandomSlot = Buttons->AddChildToHorizontalBox(RandomButton))
		RandomSlot->SetPadding(FMargin(0.f, 0.f, 12.f, 0.f));
	Buttons->AddChildToHorizontalBox(CancelButton);
	if (UVerticalBoxSlot* ButtonsSlot = Column->AddChildToVerticalBox(Buttons))
		ButtonsSlot->SetHorizontalAlignment(HAlign_Center);
}

void US1EntryMapWidget::NativeConstruct()
{
	Super::NativeConstruct();

	// Reopened after a rejected entry or a cancel — start clean
	HoveredIndex = INDEX_NONE;
	ChosenIndex = INDEX_NONE;
	bRequested = false;
	Refresh();
}

void US1EntryMapWidget::Close()
{
	RemoveFromParent();
}

// ── State ────────────────────────────────────────────────────────────────

void US1EntryMapWidget::Choose(int32 MarkerIndex)
{
	if (bRequested)
		return;

	US1BaseCampSubsystem* BaseCamp = GetBaseCamp();
	if (BaseCamp == nullptr || BaseCamp->CanEnter() == false)
		return;

	const uint32 EntryId = Markers.IsValidIndex(MarkerIndex) ? Markers[MarkerIndex].EntryId : 0;

	// Once only — repeated clicks and keys while the request is out do nothing (AC-EP-06)
	bRequested = true;
	ChosenIndex = Markers.IsValidIndex(MarkerIndex) ? MarkerIndex : INDEX_NONE;
	BaseCamp->RequestEnter(EntryId);
	Refresh();
}

void US1EntryMapWidget::SetHovered(int32 MarkerIndex)
{
	if (HoveredIndex == MarkerIndex)
		return;

	HoveredIndex = MarkerIndex;
	Refresh();
}

void US1EntryMapWidget::Refresh()
{
	if (Title == nullptr)
		return;

	Title->SetText(Text(bRequested ? TEXT("label.entering") : TEXT("label.entry_title")));

	for (int32 i = 0; i < MarkerWidgets.Num(); i++)
	{
		const bool bLit = bRequested ? (i == ChosenIndex) : (i == HoveredIndex);
		const float Alpha = (bRequested && i != ChosenIndex) ? 0.35f : 1.f;
		MarkerWidgets[i]->SetBrushColor(bLit ? FLinearColor::White : FLinearColor(0.75f, 0.75f, 0.75f, Alpha));
	}

	const FLinearColor ButtonTint = bRequested ? FLinearColor(1.f, 1.f, 1.f, 0.35f) : FLinearColor::White;
	RandomButton->SetBrushColor(CellHover() * ButtonTint);
	CancelButton->SetBrushColor(CellHover() * ButtonTint);

	if (Markers.IsValidIndex(HoveredIndex) && bRequested == false)
	{
		const FMarker& Marker = Markers[HoveredIndex];
		HoverLine->SetText(FText::Format(INVTEXT("{0} · {1} — {2}"), Marker.Letter, FText::AsNumber(HoveredIndex + 1), Marker.Name));
	}
	else
	{
		HoverLine->SetText(bRequested ? FText::GetEmpty() : Text(TEXT("label.entry_hint")));
	}
}

// ── Input ────────────────────────────────────────────────────────────────

FReply US1EntryMapWidget::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	const FVector2D At = InMouseEvent.GetScreenSpacePosition();
	auto IsUnder = [&At](const UWidget* Widget) { return Widget != nullptr && Widget->GetCachedGeometry().IsUnderLocation(At); };

	if (InMouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		for (int32 i = 0; i < MarkerWidgets.Num(); i++)
		{
			if (IsUnder(MarkerWidgets[i]))
			{
				Choose(i);
				return FReply::Handled();
			}
		}

		if (IsUnder(RandomButton))
			Choose(INDEX_NONE);
		else if (IsUnder(CancelButton) && bRequested == false)
			Close();
	}

	// The dim layer swallows everything — S3 underneath must not react to clicks meant for S4
	return FReply::Handled();
}

FReply US1EntryMapWidget::NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	const FVector2D At = InMouseEvent.GetScreenSpacePosition();

	int32 Hovered = INDEX_NONE;
	for (int32 i = 0; i < MarkerWidgets.Num(); i++)
	{
		if (MarkerWidgets[i]->GetCachedGeometry().IsUnderLocation(At))
		{
			Hovered = i;
			break;
		}
	}

	SetHovered(Hovered);
	return Super::NativeOnMouseMove(InGeometry, InMouseEvent);
}

FReply US1EntryMapWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	using namespace S1EntryMapLayout;

	const FKey Key = InKeyEvent.GetKey();

	if (Key == EKeys::Escape)
	{
		if (bRequested == false)
			Close();
		return FReply::Handled();
	}

	if (Key == EKeys::R)
	{
		Choose(INDEX_NONE);
		return FReply::Handled();
	}

	for (int32 i = 0; i < FMath::Min(Markers.Num(), MaxNumberKeys); i++)
	{
		if (Key == NumberKey(i) || Key == NumPadKey(i))
		{
			Choose(i);
			return FReply::Handled();
		}
	}

	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}
