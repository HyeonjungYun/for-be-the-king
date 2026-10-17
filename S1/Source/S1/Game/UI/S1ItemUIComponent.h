// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "S1ItemUIComponent.generated.h"

class APlayerController;
class UEnhancedInputComponent;
class UInputAction;
class UInputMappingContext;
class US1ItemHudRootWidget;
enum class ES1Place : uint8;

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

	/**
	 * Puts the HUD up and the key context on, once the pawn has a local controller. From BeginPlay
	 * for the level's own pawn, and from AS1MyPlayer::NotifyControllerChanged for a pawn spawned
	 * on re-entry — that one has no controller yet when BeginPlay runs. Safe to call twice.
	 */
	void InitForLocalController();

	/**
	 * The key bound to an equip slot's skill for this controller's pawn, as the player would read it.
	 * The fixed name (equipment-skill-binding.md B1) when there is no pawn or nothing is bound —
	 * S3 asks from base camp, where our pawn may be gone.
	 */
	static FText KeyNameForController(const APlayerController* PC, int32 KeySlot);

protected:

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:

	void OnInventoryKey();
	void OnPartyKey();		// P - 던전 파티 창 (party-betrayal 2026-10-12 R10)
	void OnOpenKey();
	void OnCloseKey();

	/** Tab and F belong to the dungeon. Not in base camp, not while we lie dead. */
	bool CanUseDungeonKeys() const;

	/** Walking around the base-camp map (not a panel, not the dungeon) - Tab opens the inventory there too. */
	bool IsOnCampMap() const;
	void HandlePlaceChanged(ES1Place Place);
	void HandleLocalDeath();

	/** The first key mapped to Action, or Fallback when nothing is. */
	static FText KeyNameOf(const APlayerController* PC, const UInputAction* Action, const FText& Fallback);

	APlayerController* GetLocalController() const;

	UPROPERTY() TObjectPtr<UInputAction>			InventoryAction;
	UPROPERTY() TObjectPtr<UInputAction>			OpenAction;
	UPROPERTY() TObjectPtr<UInputAction>			CloseAction;
	UPROPERTY() TObjectPtr<UInputAction>			PartyAction;
	UPROPERTY() TObjectPtr<class US1PartyPanelWidget>	PartyPanel;
	UPROPERTY() TObjectPtr<UInputMappingContext>	Context;
	UPROPERTY() TObjectPtr<US1ItemHudRootWidget>	Root;

	FDelegateHandle		PlaceChangedHandle;
	FDelegateHandle		LocalDeathHandle;
};
