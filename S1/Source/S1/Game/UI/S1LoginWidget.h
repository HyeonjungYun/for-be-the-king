// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "S1LoginWidget.generated.h"

class UBorder;
class UEditableTextBox;
class UHorizontalBox;
class UTextBlock;
class UVerticalBox;
class US1LoginSubsystem;

/**
 * The login screen — design/quick-specs/account-login-mvp-2026-10-09.md Core Rules 2 · 4.
 *
 *   L1  로그인                      L2  캐릭터 이름
 *       아이디    [            ]        한글 · 영문 · 숫자 2~12자
 *       비밀번호  [●●●●●●●●    ]        [            ]
 *       [로그인] [가입]                 [만들기]
 *       message line                    message line
 *
 * Enter submits the page. All state lives in US1LoginSubsystem; this only draws it and forwards input.
 */
UCLASS()
class S1_API US1LoginWidget : public UUserWidget
{
	GENERATED_BODY()

public:

	/** Puts the caret in the page's first empty box — the UI-only input mode alone does not. */
	void FocusFirstField();

	/** After a refusal: L1 wipes the password and puts the caret there; L2 keeps the name for editing. */
	void ResetForRetry();

protected:

	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;

private:

	US1LoginSubsystem* GetLogin() const;
	void Refresh();

	UTextBlock* MakeText(int32 FontSize, const FLinearColor& Color);
	UEditableTextBox* MakeBox(UVerticalBox* Parent, const TCHAR* LabelKey, bool bPassword);
	UBorder* MakeButton(UHorizontalBox* Row, const TCHAR* Key, TObjectPtr<UTextBlock>& OutText);

	void SubmitCredentials(bool bRegister);
	void SubmitName();

	UFUNCTION() void HandleCredentialsCommitted(const FText& InText, ETextCommit::Type CommitMethod);
	UFUNCTION() void HandleNameCommitted(const FText& InText, ETextCommit::Type CommitMethod);

	UPROPERTY() TObjectPtr<UTextBlock>			Title;
	UPROPERTY() TObjectPtr<UVerticalBox>		CredentialsPage;
	UPROPERTY() TObjectPtr<UVerticalBox>		NamePage;
	UPROPERTY() TObjectPtr<UEditableTextBox>	UsernameBox;
	UPROPERTY() TObjectPtr<UEditableTextBox>	PasswordBox;
	UPROPERTY() TObjectPtr<UEditableTextBox>	NameBox;
	UPROPERTY() TObjectPtr<UBorder>				LoginButton;
	UPROPERTY() TObjectPtr<UBorder>				RegisterButton;
	UPROPERTY() TObjectPtr<UBorder>				CreateButton;
	UPROPERTY() TObjectPtr<UTextBlock>			LoginText;
	UPROPERTY() TObjectPtr<UTextBlock>			RegisterText;
	UPROPERTY() TObjectPtr<UTextBlock>			CreateText;
	UPROPERTY() TObjectPtr<UTextBlock>			MessageLine;

	FDelegateHandle ChangedHandle;
};
