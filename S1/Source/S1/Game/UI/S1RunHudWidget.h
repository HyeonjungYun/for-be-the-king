// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "S1RunHudWidget.generated.h"

class UTextBlock;
class UProgressBar;
class UVerticalBox;

/**
 * The run strip at the top centre — time left, warnings, the escape gauge, the alarm line, and the
 * result banner after a run (design/gdd/escape-timer.md § UI Requirements).
 *
 * Built in C++ like the item UI. US1EscapeSubsystem pushes values every frame; the widget holds no
 * rules of its own — so the same data the tests check is what the player reads.
 */
UCLASS()
class S1_API US1RunHudWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Empty text hides the line. */
	void SetTimer(const FText& Text, bool bUrgent);
	void SetWarning(const FText& Text);
	void SetGauge(bool bVisible, float Progress, const FText& Label, bool bDraining);
	void SetAlarm(const FText& Text, bool bSelf);
	void SetResult(const FText& Text);
	/** 짧은 알림 줄 (파티 배신 등) - 빈 텍스트로 지운다 (party-betrayal 2026-10-12) */
	void SetNotice(const FText& Text, bool bUrgent);

protected:
	virtual void NativeOnInitialized() override;

private:
	UTextBlock* MakeLine(int32 FontSize);

	UPROPERTY() TObjectPtr<UVerticalBox>	Column;
	UPROPERTY() TObjectPtr<UTextBlock>		TimerText;
	UPROPERTY() TObjectPtr<UTextBlock>		WarningText;
	UPROPERTY() TObjectPtr<UTextBlock>		GaugeText;
	UPROPERTY() TObjectPtr<UProgressBar>	GaugeBar;
	UPROPERTY() TObjectPtr<UTextBlock>		AlarmText;
	UPROPERTY() TObjectPtr<UTextBlock>		ResultText;
	UPROPERTY() TObjectPtr<UTextBlock>		NoticeText;
};
