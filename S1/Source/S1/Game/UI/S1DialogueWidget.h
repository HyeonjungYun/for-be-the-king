// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Game/BaseCamp/S1CampLayout.h"
#include "S1DialogueWidget.generated.h"

class UBorder;
class UTextBlock;
class UVerticalBox;

DECLARE_DELEGATE_OneParam(FOnS1DialogueChosen, ES1CampAction);

/**
 * The NPC conversation box (design/quick-specs/basecamp-map-2026-10-17.md § 대화 UI) — bottom centre, over the world:
 *
 *   ┌──────────────────────────────────────────────┐
 *   │ 창고지기 오르트                                 │  name, warm gold
 *   │ 맡길 건 맡기고, 들고 갈 건 들고 가. …            │  the line, typed out a few characters a frame
 *   │  1  창고를 연다                                 │  options - click, or the number key
 *   │  2  됐어                                       │
 *   └──────────────────────────────────────────────┘
 *
 * The first click (or key) while the line is still typing finishes it instead of choosing — nobody picks
 * an answer to a sentence they have not read. Esc = the last option (always "됐어").
 */
UCLASS()
class S1_API US1DialogueWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	void Show(const FS1CampDialogue& Dialogue);
	bool IsTyping() const { return Shown < FullLine.Len(); }

	FOnS1DialogueChosen OnChosen;

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;

private:
	void Choose(int32 Index);

	UPROPERTY() TObjectPtr<UTextBlock>				NameText;
	UPROPERTY() TObjectPtr<UTextBlock>				LineText;
	UPROPERTY() TObjectPtr<UVerticalBox>			OptionBox;
	UPROPERTY() TArray<TObjectPtr<UBorder>>			OptionRows;

	TArray<ES1CampAction>	Actions;
	FString					FullLine;
	float					Shown = 0.f;		// characters of the line shown so far
};
