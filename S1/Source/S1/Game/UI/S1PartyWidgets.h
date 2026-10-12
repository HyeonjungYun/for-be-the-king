// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "S1PartyWidgets.generated.h"

class UBorder;
class UEditableTextBox;
class UTextBlock;
class UVerticalBox;
class US1PartySubsystem;

/**
 * "{0} 님이 협동 파티에 초대했습니다" [수락] [거절] — PVE co-op invite (ADR-0014). A small box at the top of the screen,
 * over S3 and S4; clicks outside it go through to them. Shown and hidden by US1PartySubsystem.
 */
UCLASS()
class S1_API US1PartyInviteWidget : public UUserWidget
{
	GENERATED_BODY()

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;

private:
	UPROPERTY() TObjectPtr<UTextBlock>	Line;
	UPROPERTY() TObjectPtr<UBorder>		AcceptButton;
	UPROPERTY() TObjectPtr<UBorder>		DeclineButton;
};

/**
 * 협동 파티 창 (n/3) — opened from S3's [파티] (ADR-0014). Members with their place (베이스캠프 / 던전 / 접속 끊김),
 * a name box to invite (Enter or [초대]), [내보내기] for the leader, [파티 나가기] and [닫기] (Esc). The last request's
 * outcome stays on one line for a few seconds.
 */
UCLASS()
class S1_API US1PartyPanelWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	void Close();

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

private:
	void Refresh();
	void SendInvite();
	UFUNCTION() void HandleNameCommitted(const FText& InText, ETextCommit::Type CommitMethod);

	US1PartySubsystem* GetParty() const;

	UPROPERTY() TObjectPtr<UTextBlock>			Title;
	UPROPERTY() TObjectPtr<UVerticalBox>		MemberList;
	UPROPERTY() TObjectPtr<UEditableTextBox>	NameBox;
	UPROPERTY() TObjectPtr<UBorder>				InviteButton;
	UPROPERTY() TObjectPtr<UTextBlock>			ResultLine;
	UPROPERTY() TObjectPtr<UBorder>				LeaveButton;
	UPROPERTY() TObjectPtr<UTextBlock>			LeaveText;
	double										LeaveArmedAt = 0.0;		// 던전 파티: 첫 클릭은 경고, 3 s 안의 두 번째 클릭이 배신 (party-betrayal R10)
	UPROPERTY() TObjectPtr<UBorder>				CloseButton;
	UPROPERTY() TArray<TObjectPtr<UBorder>>		KickButtons;
	TArray<uint64>								KickIds;		// parallel to KickButtons

	FDelegateHandle	ChangedHandle;
	double			ShownResultAt = 0.0;
};
