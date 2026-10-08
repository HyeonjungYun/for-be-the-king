// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/UI/S1LoginWidget.h"
#include "Game/UI/S1ItemUIStyle.h"
#include "Game/Login/S1LoginSubsystem.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/GameInstance.h"
#include "Styling/SlateBrush.h"

using namespace S1ItemUIStyle;

namespace
{
	/** The form column, in 1080p units — wide enough for a 32-character id at FontBody. */
	constexpr float FormWidth = 360.f;
}

US1LoginSubsystem* US1LoginWidget::GetLogin() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance != nullptr ? GameInstance->GetSubsystem<US1LoginSubsystem>() : nullptr;
}

UTextBlock* US1LoginWidget::MakeText(int32 FontSize, const FLinearColor& Color)
{
	UTextBlock* Line = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Line->SetFont(Font(FontSize));
	Line->SetColorAndOpacity(FSlateColor(Color));
	return Line;
}

UEditableTextBox* US1LoginWidget::MakeBox(UVerticalBox* Parent, const TCHAR* LabelKey, bool bPassword)
{
	UTextBlock* Label = MakeText(FontSmall, TextSecondary());
	Label->SetText(Text(LabelKey));
	if (UVerticalBoxSlot* LabelSlot = Parent->AddChildToVerticalBox(Label))
		LabelSlot->SetPadding(FMargin(0.f, 6.f, 0.f, 2.f));

	// Same look as the S3 skill-name filter - one text-box style across the UI
	UEditableTextBox* Box = WidgetTree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass());
	const FSlateColorBrush Plain(FLinearColor::White);
	FEditableTextBoxStyle Style = Box->GetWidgetStyle();
	Style
		.SetFont(Font(FontBody))
		.SetForegroundColor(FSlateColor(TextPrimary()))
		.SetBackgroundColor(FSlateColor(CellBackground()))
		.SetBackgroundImageNormal(Plain)
		.SetBackgroundImageHovered(Plain)
		.SetBackgroundImageFocused(Plain)
		.SetPadding(FMargin(8.f, 5.f));
	Box->SetWidgetStyle(Style);
	Box->SetIsPassword(bPassword);

	// Enter submits but keeps the caret - after "비밀번호가 틀렸다" the next keystroke goes back into the box.
	// The default clears focus on commit, and the retyped password went nowhere (found 2026-10-09 in the client)
	Box->SetClearKeyboardFocusOnCommit(false);

	Parent->AddChildToVerticalBox(Box);
	return Box;
}

UBorder* US1LoginWidget::MakeButton(UHorizontalBox* Row, const TCHAR* Key, TObjectPtr<UTextBlock>& OutText)
{
	UBorder* Button = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Button->SetPadding(FMargin(16.f, 8.f));
	Button->SetHorizontalAlignment(HAlign_Center);
	Button->SetBrushColor(CellHover());

	OutText = MakeText(FontEmphasis, TextPrimary());
	OutText->SetText(Text(Key));
	Button->SetContent(OutText);

	if (UHorizontalBoxSlot* ButtonSlot = Row->AddChildToHorizontalBox(Button))
	{
		ButtonSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		ButtonSlot->SetPadding(FMargin(0.f, 0.f, 6.f, 0.f));
	}
	return Button;
}

void US1LoginWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	// ── The whole screen, opaque - there is nothing to see behind it before login ──
	UBorder* Background = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	FLinearColor Opaque = PanelBackground();
	Opaque.A = 1.f;
	Background->SetBrushColor(Opaque);
	Background->SetHorizontalAlignment(HAlign_Center);
	Background->SetVerticalAlignment(VAlign_Center);
	WidgetTree->RootWidget = Background;

	USizeBox* Form = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Form->SetWidthOverride(FormWidth);
	Background->SetContent(Form);

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Form->SetContent(Column);

	Title = MakeText(FontEmphasis + 5, TextPrimary());
	if (UVerticalBoxSlot* TitleSlot = Column->AddChildToVerticalBox(Title))
		TitleSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));

	// ── L1 ──
	CredentialsPage = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Column->AddChildToVerticalBox(CredentialsPage);

	UsernameBox = MakeBox(CredentialsPage, TEXT("login.username"), false);
	PasswordBox = MakeBox(CredentialsPage, TEXT("login.password"), true);
	UsernameBox->OnTextCommitted.AddDynamic(this, &US1LoginWidget::HandleCredentialsCommitted);
	PasswordBox->OnTextCommitted.AddDynamic(this, &US1LoginWidget::HandleCredentialsCommitted);

	UHorizontalBox* CredentialButtons = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	if (UVerticalBoxSlot* RowSlot = CredentialsPage->AddChildToVerticalBox(CredentialButtons))
		RowSlot->SetPadding(FMargin(0.f, 12.f, 0.f, 0.f));
	LoginButton = MakeButton(CredentialButtons, TEXT("login.submit"), LoginText);
	RegisterButton = MakeButton(CredentialButtons, TEXT("login.register"), RegisterText);

	// ── L2 ──
	NamePage = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Column->AddChildToVerticalBox(NamePage);

	UTextBlock* NameHint = MakeText(FontSmall, TextTertiary());
	NameHint->SetText(Text(TEXT("character.hint")));
	NamePage->AddChildToVerticalBox(NameHint);

	NameBox = MakeBox(NamePage, TEXT("character.name"), false);
	NameBox->OnTextCommitted.AddDynamic(this, &US1LoginWidget::HandleNameCommitted);

	UHorizontalBox* NameButtons = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	if (UVerticalBoxSlot* RowSlot = NamePage->AddChildToVerticalBox(NameButtons))
		RowSlot->SetPadding(FMargin(0.f, 12.f, 0.f, 0.f));
	CreateButton = MakeButton(NameButtons, TEXT("character.create"), CreateText);

	// ── One message line under either page. Height kept even when empty - no layout jump ──
	USizeBox* MessageBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	MessageBox->SetHeightOverride(NoticeLineHeight + 6.f);
	MessageLine = MakeText(FontBody, TextSecondary());
	MessageLine->SetAutoWrapText(true);
	MessageBox->SetContent(MessageLine);
	if (UVerticalBoxSlot* MessageSlot = Column->AddChildToVerticalBox(MessageBox))
		MessageSlot->SetPadding(FMargin(0.f, 10.f, 0.f, 0.f));

	SetIsFocusable(true);
	Refresh();
}

void US1LoginWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (US1LoginSubsystem* Login = GetLogin())
		ChangedHandle = Login->OnChanged.AddUObject(this, &US1LoginWidget::Refresh);

	Refresh();
}

void US1LoginWidget::NativeDestruct()
{
	if (US1LoginSubsystem* Login = GetLogin())
		Login->OnChanged.Remove(ChangedHandle);

	Super::NativeDestruct();
}

void US1LoginWidget::Refresh()
{
	const US1LoginSubsystem* Login = GetLogin();
	if (Login == nullptr || Title == nullptr)
		return;

	const ES1LoginPage Page = Login->GetPage();
	const bool bBusy = Login->IsBusy();

	Title->SetText(Text(Page == ES1LoginPage::CharacterName ? TEXT("character.title") : TEXT("login.title")));

	// "접속 중…" shows no form - there is nothing to press until S_LOGIN answers
	CredentialsPage->SetVisibility(Page == ES1LoginPage::Credentials ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	NamePage->SetVisibility(Page == ES1LoginPage::CharacterName ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);

	// While a request is out the buttons read as dead - a second click is ignored anyway (US1LoginSubsystem::bBusy)
	for (UTextBlock* ButtonText : { LoginText.Get(), RegisterText.Get(), CreateText.Get() })
		ButtonText->SetColorAndOpacity(FSlateColor(bBusy ? TextShadowed() : TextPrimary()));
	for (UBorder* Button : { LoginButton.Get(), RegisterButton.Get(), CreateButton.Get() })
		Button->SetBrushColor(bBusy ? CellBackground() : CellHover());

	MessageLine->SetText(Login->GetMessage());
}

void US1LoginWidget::ResetForRetry()
{
	const US1LoginSubsystem* Login = GetLogin();

	// The usual login form: a refused password is wiped, the id stays - typing starts a fresh password
	if (Login != nullptr && Login->GetPage() == ES1LoginPage::Credentials)
		PasswordBox->SetText(FText::GetEmpty());

	FocusFirstField();
}

void US1LoginWidget::FocusFirstField()
{
	const US1LoginSubsystem* Login = GetLogin();
	if (Login == nullptr)
		return;

	if (Login->GetPage() == ES1LoginPage::CharacterName)
	{
		NameBox->SetKeyboardFocus();
		return;
	}

	// Back on L1 after a failure the id is usually right - put the caret where the next keystroke belongs
	UEditableTextBox* First = UsernameBox->GetText().IsEmpty() ? UsernameBox.Get() : PasswordBox.Get();
	First->SetKeyboardFocus();
}

void US1LoginWidget::SubmitCredentials(bool bRegister)
{
	if (US1LoginSubsystem* Login = GetLogin())
		Login->SubmitCredentials(UsernameBox->GetText().ToString(), PasswordBox->GetText().ToString(), bRegister);
}

void US1LoginWidget::SubmitName()
{
	if (US1LoginSubsystem* Login = GetLogin())
		Login->SubmitCharacterName(NameBox->GetText().ToString());
}

void US1LoginWidget::HandleCredentialsCommitted(const FText& InText, ETextCommit::Type CommitMethod)
{
	if (CommitMethod == ETextCommit::OnEnter)
		SubmitCredentials(false);
}

void US1LoginWidget::HandleNameCommitted(const FText& InText, ETextCommit::Type CommitMethod)
{
	if (CommitMethod == ETextCommit::OnEnter)
		SubmitName();
}

FReply US1LoginWidget::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (InMouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
		return Super::NativeOnMouseButtonDown(InGeometry, InMouseEvent);

	const FVector2D At = InMouseEvent.GetScreenSpacePosition();
	auto IsUnder = [&At](const UWidget* Widget)
		{
			return Widget != nullptr && Widget->IsVisible() && Widget->GetCachedGeometry().IsUnderLocation(At);
		};

	const US1LoginSubsystem* Login = GetLogin();
	const ES1LoginPage Page = Login != nullptr ? Login->GetPage() : ES1LoginPage::Credentials;

	if (Page == ES1LoginPage::Credentials && IsUnder(LoginButton))
	{
		SubmitCredentials(false);
		return FReply::Handled();
	}

	if (Page == ES1LoginPage::Credentials && IsUnder(RegisterButton))
	{
		SubmitCredentials(true);
		return FReply::Handled();
	}

	if (Page == ES1LoginPage::CharacterName && IsUnder(CreateButton))
	{
		SubmitName();
		return FReply::Handled();
	}

	return Super::NativeOnMouseButtonDown(InGeometry, InMouseEvent);
}
