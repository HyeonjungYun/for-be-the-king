// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/UI/S1PartyWidgets.h"
#include "Game/Party/S1PartySubsystem.h"
#include "Game/UI/S1ItemUIStyle.h"
#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Components/Border.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "InputCoreTypes.h"

using namespace S1ItemUIStyle;

namespace S1PartyLayout
{
	constexpr float PanelWidth = 420.f;
	constexpr double ResultSeconds = 4.0;		// how long the outcome of a request stays

	UTextBlock* MakeText(UWidgetTree* Tree, int32 Size, const FLinearColor& Color)
	{
		UTextBlock* Text = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Text->SetFont(Font(Size));
		Text->SetColorAndOpacity(FSlateColor(Color));
		return Text;
	}

	UBorder* MakeButton(UWidgetTree* Tree, const FText& Label)
	{
		UBorder* Button = Tree->ConstructWidget<UBorder>(UBorder::StaticClass());
		Button->SetBrushColor(CellHover());
		Button->SetPadding(FMargin(14.f, 5.f));
		UTextBlock* Text = MakeText(Tree, FontBody, TextPrimary());
		Text->SetText(Label);
		Button->SetContent(Text);
		return Button;
	}

	FText StateText(int32 State)
	{
		switch (State)
		{
		case 1:		return Text(TEXT("party.state.in_run"));
		case 2:		return Text(TEXT("party.state.offline"));
		default:	return Text(TEXT("party.state.basecamp"));
		}
	}

	bool IsUnder(const UWidget* Widget, const FVector2D& At)
	{
		return Widget != nullptr && Widget->IsVisible() && Widget->GetCachedGeometry().IsUnderLocation(At);
	}
}

// ── Invite popup ────────────────────────────────────────────────────────

void US1PartyInviteWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	using namespace S1PartyLayout;

	// The whole screen, but only the box takes clicks
	UBorder* Root = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Root->SetBrushColor(FLinearColor::Transparent);
	Root->SetHorizontalAlignment(HAlign_Center);
	Root->SetVerticalAlignment(VAlign_Top);
	Root->SetPadding(FMargin(0.f, 60.f, 0.f, 0.f));
	Root->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	WidgetTree->RootWidget = Root;

	UBorder* Box = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Box->SetBrush(FSlateRoundedBoxBrush(PanelBackground(), 6.f, FLinearColor(FColor(0x39, 0x87, 0xE5)), 2.f));
	Box->SetPadding(FMargin(18.f, 12.f));
	Root->SetContent(Box);

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Box->SetContent(Column);

	Line = MakeText(WidgetTree, FontBody, TextPrimary());
	if (UVerticalBoxSlot* LineSlot = Column->AddChildToVerticalBox(Line))
	{
		LineSlot->SetHorizontalAlignment(HAlign_Center);
		LineSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 10.f));
	}

	UHorizontalBox* Buttons = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	AcceptButton = MakeButton(WidgetTree, Text(TEXT("party.accept")));
	DeclineButton = MakeButton(WidgetTree, Text(TEXT("party.decline")));
	if (UHorizontalBoxSlot* AcceptSlot = Buttons->AddChildToHorizontalBox(AcceptButton))
		AcceptSlot->SetPadding(FMargin(0.f, 0.f, 10.f, 0.f));
	Buttons->AddChildToHorizontalBox(DeclineButton);
	if (UVerticalBoxSlot* ButtonsSlot = Column->AddChildToVerticalBox(Buttons))
		ButtonsSlot->SetHorizontalAlignment(HAlign_Center);
}

void US1PartyInviteWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	const US1PartySubsystem* Party = US1PartySubsystem::Get();
	if (Party == nullptr || Line == nullptr || Party->HasInvite() == false)
		return;

	const int32 Left = FMath::Max(0, FMath::CeilToInt(Party->GetInviteExpiresAt() - FPlatformTime::Seconds()));
	Line->SetText(FText::Format(Text(TEXT("party.invited")), FText::FromString(Party->GetInviterName()), FText::AsNumber(Left)));
}

FReply US1PartyInviteWidget::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	const FVector2D At = InMouseEvent.GetScreenSpacePosition();
	US1PartySubsystem* Party = US1PartySubsystem::Get();

	if (Party != nullptr && InMouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		if (S1PartyLayout::IsUnder(AcceptButton, At))
			Party->RespondToInvite(true);
		else if (S1PartyLayout::IsUnder(DeclineButton, At))
			Party->RespondToInvite(false);
	}
	return FReply::Handled();
}

// ── Party panel ─────────────────────────────────────────────────────────

US1PartySubsystem* US1PartyPanelWidget::GetParty() const
{
	return US1PartySubsystem::Get();
}

void US1PartyPanelWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	using namespace S1PartyLayout;

	SetIsFocusable(true);		// Esc closes

	UBorder* Dim = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Dim->SetBrushColor(FLinearColor(0.f, 0.f, 0.f, 0.65f));
	Dim->SetHorizontalAlignment(HAlign_Center);
	Dim->SetVerticalAlignment(VAlign_Center);
	WidgetTree->RootWidget = Dim;

	USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Size->SetWidthOverride(PanelWidth);
	Dim->SetContent(Size);

	UBorder* Panel = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Panel->SetBrush(FSlateRoundedBoxBrush(PanelBackground(), 6.f, Divider(), 2.f));
	Panel->SetPadding(FMargin(PanelPadding + 6.f));
	Size->SetContent(Panel);

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Panel->SetContent(Column);

	Title = MakeText(WidgetTree, FontEmphasis, TextPrimary());
	if (UVerticalBoxSlot* TitleSlot = Column->AddChildToVerticalBox(Title))
		TitleSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 10.f));

	MemberList = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	if (UVerticalBoxSlot* ListSlot = Column->AddChildToVerticalBox(MemberList))
		ListSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 12.f));

	// Invite row - name box + [초대]
	UTextBlock* Hint = MakeText(WidgetTree, FontSmall, TextSecondary());
	Hint->SetText(Text(TEXT("party.name_hint")));
	if (UVerticalBoxSlot* HintSlot = Column->AddChildToVerticalBox(Hint))
		HintSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 2.f));

	UHorizontalBox* InviteRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	NameBox = WidgetTree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass());
	{
		const FSlateColorBrush Plain(FLinearColor::White);
		FEditableTextBoxStyle Style = NameBox->GetWidgetStyle();
		Style
			.SetFont(Font(FontBody))
			.SetForegroundColor(FSlateColor(TextPrimary()))
			.SetBackgroundColor(FSlateColor(CellBackground()))
			.SetBackgroundImageNormal(Plain)
			.SetBackgroundImageHovered(Plain)
			.SetBackgroundImageFocused(Plain)
			.SetPadding(FMargin(8.f, 5.f));
		NameBox->SetWidgetStyle(Style);
		NameBox->SetClearKeyboardFocusOnCommit(false);
		NameBox->OnTextCommitted.AddDynamic(this, &US1PartyPanelWidget::HandleNameCommitted);
	}
	if (UHorizontalBoxSlot* BoxSlot = InviteRow->AddChildToHorizontalBox(NameBox))
	{
		BoxSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		BoxSlot->SetPadding(FMargin(0.f, 0.f, 8.f, 0.f));
	}
	InviteButton = MakeButton(WidgetTree, Text(TEXT("party.invite")));
	InviteRow->AddChildToHorizontalBox(InviteButton);
	Column->AddChildToVerticalBox(InviteRow);

	ResultLine = MakeText(WidgetTree, FontSmall, TextSecondary());
	ResultLine->SetAutoWrapText(true);
	if (UVerticalBoxSlot* ResultSlot = Column->AddChildToVerticalBox(ResultLine))
		ResultSlot->SetPadding(FMargin(0.f, 6.f, 0.f, 12.f));

	UHorizontalBox* Bottom = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	LeaveButton = MakeButton(WidgetTree, Text(TEXT("party.leave")));
	CloseButton = MakeButton(WidgetTree, Text(TEXT("party.close")));
	if (UHorizontalBoxSlot* LeaveSlot = Bottom->AddChildToHorizontalBox(LeaveButton))
		LeaveSlot->SetPadding(FMargin(0.f, 0.f, 10.f, 0.f));
	Bottom->AddChildToHorizontalBox(CloseButton);
	if (UVerticalBoxSlot* BottomSlot = Column->AddChildToVerticalBox(Bottom))
		BottomSlot->SetHorizontalAlignment(HAlign_Right);
}

void US1PartyPanelWidget::NativeConstruct()
{
	Super::NativeConstruct();
	if (US1PartySubsystem* Party = GetParty())
		ChangedHandle = Party->OnPartyChanged.AddUObject(this, &US1PartyPanelWidget::Refresh);
	ShownResultAt = FPlatformTime::Seconds();		// an old outcome does not reappear when the window opens
	Refresh();
}

void US1PartyPanelWidget::NativeDestruct()
{
	if (US1PartySubsystem* Party = GetParty())
		Party->OnPartyChanged.Remove(ChangedHandle);
	Super::NativeDestruct();
}

void US1PartyPanelWidget::Close()
{
	RemoveFromParent();
}

void US1PartyPanelWidget::Refresh()
{
	using namespace S1PartyLayout;
	const US1PartySubsystem* Party = GetParty();
	if (Party == nullptr || Title == nullptr)
		return;

	Title->SetText(FText::Format(Text(TEXT("party.title")), FText::AsNumber(Party->Size()), FText::AsNumber(3)));

	MemberList->ClearChildren();
	KickButtons.Reset();
	KickIds.Reset();

	if (Party->IsInParty() == false)
	{
		UTextBlock* Empty = MakeText(WidgetTree, FontBody, TextTertiary());
		Empty->SetText(Text(TEXT("party.empty")));
		MemberList->AddChildToVerticalBox(Empty);
	}

	const bool bLeader = Party->IsLeader();
	for (const FS1PartyMemberView& M : Party->GetMembers())
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());

		UTextBlock* Name = MakeText(WidgetTree, FontBody, M.CharacterId == Party->GetMyCharacterId() ? TextPrimary() : TextSecondary());
		Name->SetText(FText::FromString((M.bLeader ? FString(TEXT("★ ")) : FString(TEXT("   "))) + M.Name));
		if (UHorizontalBoxSlot* NameSlot = Row->AddChildToHorizontalBox(Name))
		{
			NameSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			NameSlot->SetVerticalAlignment(VAlign_Center);
		}

		UTextBlock* State = MakeText(WidgetTree, FontSmall, TextTertiary());
		State->SetText(StateText(M.State));
		if (UHorizontalBoxSlot* StateSlot = Row->AddChildToHorizontalBox(State))
		{
			StateSlot->SetVerticalAlignment(VAlign_Center);
			StateSlot->SetPadding(FMargin(8.f, 0.f));
		}

		if (bLeader && M.CharacterId != Party->GetMyCharacterId())
		{
			UBorder* Kick = MakeButton(WidgetTree, Text(TEXT("party.kick")));
			Kick->SetPadding(FMargin(8.f, 2.f));
			Row->AddChildToHorizontalBox(Kick);
			KickButtons.Add(Kick);
			KickIds.Add(M.CharacterId);
		}

		if (UVerticalBoxSlot* RowSlot = MemberList->AddChildToVerticalBox(Row))
			RowSlot->SetPadding(FMargin(0.f, 2.f));
	}

	LeaveButton->SetVisibility(Party->IsInParty() ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
}

void US1PartyPanelWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	const US1PartySubsystem* Party = GetParty();
	if (Party == nullptr || ResultLine == nullptr)
		return;

	const double At = Party->GetLastResultAt();
	const bool bFresh = At >= ShownResultAt && FPlatformTime::Seconds() - At < S1PartyLayout::ResultSeconds;
	ResultLine->SetText(bFresh ? Party->GetLastResultText() : FText::GetEmpty());
}

void US1PartyPanelWidget::SendInvite()
{
	US1PartySubsystem* Party = GetParty();
	if (Party == nullptr || NameBox == nullptr)
		return;

	Party->Invite(NameBox->GetText().ToString());
	NameBox->SetText(FText::GetEmpty());
}

void US1PartyPanelWidget::HandleNameCommitted(const FText& InText, ETextCommit::Type CommitMethod)
{
	if (CommitMethod == ETextCommit::OnEnter)
		SendInvite();
}

FReply US1PartyPanelWidget::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	using namespace S1PartyLayout;
	const FVector2D At = InMouseEvent.GetScreenSpacePosition();
	US1PartySubsystem* Party = GetParty();

	if (Party != nullptr && InMouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		if (IsUnder(InviteButton, At))
			SendInvite();
		else if (IsUnder(LeaveButton, At))
			Party->Leave();
		else if (IsUnder(CloseButton, At))
			Close();
		else
		{
			for (int32 i = 0; i < KickButtons.Num(); i++)
			{
				if (IsUnder(KickButtons[i], At))
				{
					Party->Kick(KickIds[i]);
					break;
				}
			}
		}
	}

	// The dim layer swallows everything - S3 underneath must not react while the window is up
	return FReply::Handled();
}

FReply US1PartyPanelWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		Close();
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}
