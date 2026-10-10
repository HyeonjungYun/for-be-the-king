// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "S1TestModeComponent.generated.h"

class APlayerController;
class UEnhancedInputComponent;
class UInputAction;
class UInputMappingContext;
class US1TestModeWidget;

/**
 * 테스트 모드 패널의 키 — F7 열기 / 닫기, F6 커서 위치에 더미 소환 (F9 는 엔진 기본 "shot showui" 라 피했다) (design/quick-specs/test-mode-2026-10-11.md).
 *
 * Same pattern as US1ItemUIComponent: the actions and the mapping context are built at runtime, no editor asset.
 * The panel is created on the first F7 and lives until the pawn goes. Dungeon only — base camp has nothing to test.
 * The server decides whether the commands do anything (FBTK_TEST_MODE=1); the panel says so when they do not.
 */
UCLASS(ClassGroup = (S1))
class S1_API US1TestModeComponent : public UActorComponent
{
	GENERATED_BODY()

public:

	US1TestModeComponent();

	/** From AS1MyPlayer::SetupPlayerInputComponent. */
	void SetupInput(UEnhancedInputComponent* Input);

	/** Adds the key context once the pawn has a local controller (BeginPlay, or NotifyControllerChanged on re-entry). */
	void InitForLocalController();

	bool IsPanelOpen() const;

protected:

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:

	void OnToggleKey();
	void OnSpawnKey();
	bool InDungeon() const;
	APlayerController* GetLocalController() const;

	UPROPERTY() TObjectPtr<UInputAction>			ToggleAction;
	UPROPERTY() TObjectPtr<UInputAction>			SpawnAction;
	UPROPERTY() TObjectPtr<UInputMappingContext>	Context;
	UPROPERTY() TObjectPtr<US1TestModeWidget>		Panel;

	bool bContextAdded = false;
};
