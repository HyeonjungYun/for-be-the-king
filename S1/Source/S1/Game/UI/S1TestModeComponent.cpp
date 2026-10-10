// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/UI/S1TestModeComponent.h"
#include "Game/UI/S1TestModeWidget.h"
#include "Game/BaseCamp/S1BaseCampSubsystem.h"
#include "Game/S1MyPlayer.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputMappingContext.h"

namespace
{
	/** Same level as the item UI keys - F7 and F6 collide with nothing there (F9 is the engine's "shot showui" debug binding). */
	constexpr int32 TestModeContextPriority = 1;

	/** Above the item HUD (10), below menus. */
	constexpr int32 TestModeZOrder = 12;
}

US1TestModeComponent::US1TestModeComponent()
{
	PrimaryComponentTick.bCanEverTick = false;

	ToggleAction = CreateDefaultSubobject<UInputAction>(TEXT("IA_TestModeToggle"));
	SpawnAction = CreateDefaultSubobject<UInputAction>(TEXT("IA_TestModeSpawn"));
	Context = CreateDefaultSubobject<UInputMappingContext>(TEXT("IMC_TestMode"));

	Context->MapKey(ToggleAction, EKeys::F7);
	Context->MapKey(SpawnAction, EKeys::F6);
}

void US1TestModeComponent::SetupInput(UEnhancedInputComponent* Input)
{
	if (Input == nullptr)
		return;

	Input->BindAction(ToggleAction, ETriggerEvent::Started, this, &US1TestModeComponent::OnToggleKey);
	Input->BindAction(SpawnAction, ETriggerEvent::Started, this, &US1TestModeComponent::OnSpawnKey);
}

void US1TestModeComponent::BeginPlay()
{
	Super::BeginPlay();
	InitForLocalController();
}

void US1TestModeComponent::InitForLocalController()
{
	if (bContextAdded)
		return;

	APlayerController* PC = GetLocalController();
	if (PC == nullptr)
		return;

	if (UEnhancedInputLocalPlayerSubsystem* Input = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
	{
		Input->AddMappingContext(Context, TestModeContextPriority);
		bContextAdded = true;
	}
}

void US1TestModeComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (Panel != nullptr)
	{
		Panel->RemoveFromParent();
		Panel = nullptr;
	}

	if (const APlayerController* PC = GetLocalController())
	{
		if (UEnhancedInputLocalPlayerSubsystem* Input = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
			Input->RemoveMappingContext(Context);
	}
	bContextAdded = false;

	Super::EndPlay(EndPlayReason);
}

bool US1TestModeComponent::IsPanelOpen() const
{
	return Panel != nullptr && Panel->IsInViewport() && Panel->GetVisibility() != ESlateVisibility::Collapsed;
}

bool US1TestModeComponent::InDungeon() const
{
	const UWorld* World = GetWorld();
	const US1BaseCampSubsystem* BaseCamp = World != nullptr && World->GetGameInstance() != nullptr
		? World->GetGameInstance()->GetSubsystem<US1BaseCampSubsystem>() : nullptr;

	// No flow subsystem (a test map) - allow it
	return BaseCamp == nullptr || (BaseCamp->GetPlace() == ES1Place::Dungeon && BaseCamp->IsLocalDead() == false);
}

void US1TestModeComponent::OnToggleKey()
{
	if (IsPanelOpen())
	{
		Panel->SetVisibility(ESlateVisibility::Collapsed);
		UE_LOG(LogTemp, Log, TEXT("[TESTMODE] panel closed"));
		return;
	}

	if (InDungeon() == false)
	{
		UE_LOG(LogTemp, Log, TEXT("[TESTMODE] F7 ignored - not in the dungeon"));
		return;
	}

	APlayerController* PC = GetLocalController();
	if (PC == nullptr)
		return;

	if (Panel == nullptr)
	{
		Panel = CreateWidget<US1TestModeWidget>(PC, US1TestModeWidget::StaticClass());
		if (Panel == nullptr)
			return;
		Panel->AddToViewport(TestModeZOrder);
	}

	// Every open starts from what we actually wear
	Panel->SyncFromEquipment();
	Panel->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	UE_LOG(LogTemp, Log, TEXT("[TESTMODE] panel open"));
}

void US1TestModeComponent::OnSpawnKey()
{
	if (IsPanelOpen() == false || InDungeon() == false)
		return;

	const AS1MyPlayer* Me = Cast<AS1MyPlayer>(GetOwner());
	FVector Where;
	if (Me == nullptr || Me->GetCursorGroundPoint(Where) == false)
		return;

	Panel->SpawnDummiesAt(Where);
}

APlayerController* US1TestModeComponent::GetLocalController() const
{
	const APawn* Pawn = Cast<APawn>(GetOwner());
	APlayerController* PC = Pawn != nullptr ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
	return (PC != nullptr && PC->IsLocalController()) ? PC : nullptr;
}
