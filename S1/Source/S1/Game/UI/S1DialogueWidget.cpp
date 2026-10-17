// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/UI/S1DialogueWidget.h"
#include "Game/UI/S1ItemUIStyle.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"

using namespace S1ItemUIStyle;

namespace S1DialogueLook
{
	constexpr float BoxWidth = 680.f;
	constexpr float BottomGap = 72.f;				// above the skill bar
	constexpr float CharsPerSecond = 45.f;
	const FLinearColor NameGold(FColor(0xE8, 0xC1, 0x6A));
	const FLinearColor BoxFill(0.06f, 0.055f, 0.05f, 0.94f);
	const FLinearColor RowFill(0.12f, 0.11f, 0.10f, 1.f);
	const FLinearColor NumberGrey(0.62f, 0.62f, 0.60f);
}

void US1DialogueWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	using namespace S1DialogueLook;
	SetIsFocusable(true);

	// Full-screen overlay so the box can sit bottom-centre at any resolution; only the box takes clicks
	UOverlay* Root = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass());
	WidgetTree->RootWidget = Root;

	USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Size->SetWidthOverride(BoxWidth);
	if (UOverlaySlot* SizeSlot = Root->AddChildToOverlay(Size))
	{
		SizeSlot->SetHorizontalAlignment(HAlign_Center);
		SizeSlot->SetVerticalAlignment(VAlign_Bottom);
		SizeSlot->SetPadding(FMargin(0.f, 0.f, 0.f, BottomGap));
	}

	UBorder* Box = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Box->SetBrushColor(BoxFill);
	Box->SetPadding(FMargin(22.f, 16.f));
	Size->SetContent(Box);

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Box->SetContent(Column);

	NameText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	NameText->SetFont(Font(FontEmphasis));
	NameText->SetColorAndOpacity(FSlateColor(NameGold));
	if (UVerticalBoxSlot* NameSlot = Column->AddChildToVerticalBox(NameText))
		NameSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));

	LineText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	LineText->SetFont(Font(FontBody));
	LineText->SetColorAndOpacity(FSlateColor(TextPrimary()));
	LineText->SetAutoWrapText(true);
	if (UVerticalBoxSlot* LineSlot = Column->AddChildToVerticalBox(LineText))
		LineSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 12.f));

	OptionBox = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Column->AddChildToVerticalBox(OptionBox);
}

void US1DialogueWidget::Show(const FS1CampDialogue& Dialogue)
{
	using namespace S1DialogueLook;
	if (NameText == nullptr)
		return;

	NameText->SetText(Dialogue.NameKey != nullptr ? Text(Dialogue.NameKey) : FText::GetEmpty());
	FullLine = Dialogue.LineKey != nullptr ? Text(Dialogue.LineKey).ToString() : FString();
	Shown = 0.f;
	LineText->SetText(FText::GetEmpty());

	OptionBox->ClearChildren();
	OptionRows.Reset();
	Actions.Reset();
	for (int32 i = 0; i < Dialogue.Options.Num(); i++)
	{
		const FS1CampOption& Option = Dialogue.Options[i];

		UBorder* Row = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
		Row->SetBrushColor(RowFill);
		Row->SetPadding(FMargin(10.f, 6.f));

		UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		UTextBlock* Number = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Number->SetFont(Font(FontBody));
		Number->SetColorAndOpacity(FSlateColor(NumberGrey));
		Number->SetText(FText::AsNumber(i + 1));
		if (UHorizontalBoxSlot* NumberSlot = Line->AddChildToHorizontalBox(Number))
			NumberSlot->SetPadding(FMargin(0.f, 0.f, 12.f, 0.f));

		UTextBlock* Label = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Label->SetFont(Font(FontBody));
		Label->SetColorAndOpacity(FSlateColor(TextPrimary()));
		Label->SetText(Text(Option.LabelKey));
		Line->AddChildToHorizontalBox(Label);
		Row->SetContent(Line);

		if (UVerticalBoxSlot* RowSlot = OptionBox->AddChildToVerticalBox(Row))
			RowSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));
		OptionRows.Add(Row);
		Actions.Add(Option.Action);
	}

	// Options appear once the line is out (NativeTick)
	OptionBox->SetVisibility(FullLine.IsEmpty() ? ESlateVisibility::Visible : ESlateVisibility::Hidden);
	UE_LOG(LogTemp, Log, TEXT("[DIALOGUE] show %s options=%d"), Dialogue.NameKey != nullptr ? Dialogue.NameKey : TEXT("?"), Actions.Num());
}

void US1DialogueWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	if (IsTyping() == false)
		return;

	Shown = FMath::Min(Shown + InDeltaTime * S1DialogueLook::CharsPerSecond, static_cast<float>(FullLine.Len()));
	LineText->SetText(FText::FromString(FullLine.Left(FMath::FloorToInt(Shown))));
	if (IsTyping() == false)
		OptionBox->SetVisibility(ESlateVisibility::Visible);
}

FReply US1DialogueWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	if (Key == EKeys::Escape)
	{
		OnChosen.ExecuteIfBound(ES1CampAction::Close);		// leaves at once, typing or not
		return FReply::Handled();
	}

	static const FKey Numbers[] = { EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four, EKeys::NumPadOne, EKeys::NumPadTwo, EKeys::NumPadThree, EKeys::NumPadFour };
	for (int32 i = 0; i < UE_ARRAY_COUNT(Numbers); i++)
	{
		if (Key == Numbers[i])
		{
			Choose(i % 4);
			return FReply::Handled();
		}
	}

	if (Key == EKeys::SpaceBar || Key == EKeys::Enter || Key == EKeys::F)
	{
		Choose(IsTyping() ? -1 : 0);
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

FReply US1DialogueWidget::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	const FVector2D At = InMouseEvent.GetScreenSpacePosition();
	for (int32 i = 0; i < OptionRows.Num(); i++)
	{
		if (OptionRows[i] != nullptr && OptionRows[i]->GetCachedGeometry().IsUnderLocation(At))
		{
			Choose(i);
			return FReply::Handled();
		}
	}

	// A click anywhere else finishes the line - it never walks the pawn away mid-conversation
	if (IsTyping())
		Choose(-1);
	return FReply::Handled();
}

void US1DialogueWidget::Choose(int32 Index)
{
	// Still typing: the first press shows the whole line - choosing waits until the options are on screen
	if (IsTyping())
	{
		Shown = static_cast<float>(FullLine.Len());
		LineText->SetText(FText::FromString(FullLine));
		OptionBox->SetVisibility(ESlateVisibility::Visible);
		return;
	}

	if (Actions.IsValidIndex(Index) == false)
		return;

	UE_LOG(LogTemp, Log, TEXT("[DIALOGUE] chose %d action=%d"), Index + 1, static_cast<int32>(Actions[Index]));
	OnChosen.ExecuteIfBound(Actions[Index]);
}
