// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "S1ItemUIComponent.generated.h"

class UEnhancedInputComponent;
class UInputAction;
class UInputMappingContext;
class US1ItemHudRootWidget;

/**
 * Puts the item UI on screen for the local player and routes its keys.
 *
 * Tab, F and Esc are Enhanced Input actions built at runtime, so no editor asset is needed.
 * They still go through a mapping context — a rebinding screen can change them later and the
 * key badges follow (AC-UI-24).
 *
 * The input mode is never touched: the panels do not pause or block, and WASD and the skill
 * keys keep working while they are open (AC-UI-11).
 */
UCLASS(ClassGroup = (S1))
class S1_API US1ItemUIComponent : public UActorComponent
{
	GENERATED_BODY()

public:

	US1ItemUIComponent();

	/** From AS1MyPlayer::SetupPlayerInputComponent. */
	void SetupInput(UEnhancedInputComponent* Input);

protected:

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:

	void OnInventoryKey();
	void OnOpenKey();
	void OnCloseKey();

	/** The key currently bound to an equip slot's skill action, as the player would read it. */
	FText KeyNameFor(int32 KeySlot) const;

	/** The first key mapped to Action, or Fallback when nothing is. */
	FText KeyNameOf(const UInputAction* Action, const FText& Fallback) const;

	UPROPERTY() TObjectPtr<UInputAction>			InventoryAction;
	UPROPERTY() TObjectPtr<UInputAction>			OpenAction;
	UPROPERTY() TObjectPtr<UInputAction>			CloseAction;
	UPROPERTY() TObjectPtr<UInputMappingContext>	Context;
	UPROPERTY() TObjectPtr<US1ItemHudRootWidget>	Root;
};
