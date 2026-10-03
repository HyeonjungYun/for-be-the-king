// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/UI/S1ItemUIComponent.h"
#include "Game/UI/S1ItemHudRootWidget.h"
#include "Game/UI/S1InventoryPanelWidget.h"
#include "Game/S1MyPlayer.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputMappingContext.h"

namespace
{
	/** Above the gameplay contexts, so Tab and Esc reach the UI first. */
	constexpr int32 ItemUIContextPriority = 1;

	/** Above gameplay HUD, below menus. */
	constexpr int32 ItemUIZOrder = 10;
}

US1ItemUIComponent::US1ItemUIComponent()
{
	PrimaryComponentTick.bCanEverTick = false;

	// Built here rather than in BeginPlay so they exist before SetupPlayerInputComponent binds them.
	InventoryAction = CreateDefaultSubobject<UInputAction>(TEXT("IA_ItemInventory"));
	OpenAction = CreateDefaultSubobject<UInputAction>(TEXT("IA_ItemOpen"));
	CloseAction = CreateDefaultSubobject<UInputAction>(TEXT("IA_ItemClose"));
	Context = CreateDefaultSubobject<UInputMappingContext>(TEXT("IMC_ItemUI"));

	Context->MapKey(InventoryAction, EKeys::Tab);
	Context->MapKey(OpenAction, EKeys::F);
	Context->MapKey(CloseAction, EKeys::Escape);
}

void US1ItemUIComponent::SetupInput(UEnhancedInputComponent* Input)
{
	if (Input == nullptr)
		return;

	Input->BindAction(InventoryAction, ETriggerEvent::Started, this, &US1ItemUIComponent::OnInventoryKey);
	Input->BindAction(OpenAction, ETriggerEvent::Started, this, &US1ItemUIComponent::OnOpenKey);
	Input->BindAction(CloseAction, ETriggerEvent::Started, this, &US1ItemUIComponent::OnCloseKey);
}

void US1ItemUIComponent::BeginPlay()
{
	Super::BeginPlay();

	const APawn* Pawn = Cast<APawn>(GetOwner());
	APlayerController* PC = Pawn != nullptr ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
	if (PC == nullptr || PC->IsLocalController() == false)
	{
		UE_LOG(LogTemp, Log, TEXT("[ITEMUI] ui skipped - owner=%s has no local controller"), *GetNameSafe(GetOwner()));
		return;
	}

	if (UEnhancedInputLocalPlayerSubsystem* Input = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
		Input->AddMappingContext(Context, ItemUIContextPriority);

	Root = CreateWidget<US1ItemHudRootWidget>(PC, US1ItemHudRootWidget::StaticClass());
	if (Root == nullptr)
		return;

	Root->SetKeyNameResolver(
		[this](int32 KeySlot) { return KeyNameFor(KeySlot); },
		[this]() { return KeyNameOf(OpenAction, FText::AsCultureInvariant(TEXT("F"))); });

	Root->AddToViewport(ItemUIZOrder);

	UE_LOG(LogTemp, Log, TEXT("[ITEMUI] ui root on viewport=%d owner=%s"), Root->IsInViewport() ? 1 : 0, *GetNameSafe(GetOwner()));
}

void US1ItemUIComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// The pawn goes when the server despawns us at the start of an escape commit — the panels go
	// with it, which is exactly the close design/ux/item-ui.md § Entry & Exit asks for (B2).
	if (Root != nullptr)
	{
		Root->RemoveFromParent();
		Root = nullptr;
	}

	const APawn* Pawn = Cast<APawn>(GetOwner());
	if (const APlayerController* PC = Pawn != nullptr ? Cast<APlayerController>(Pawn->GetController()) : nullptr)
	{
		if (UEnhancedInputLocalPlayerSubsystem* Input = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
			Input->RemoveMappingContext(Context);
	}

	Super::EndPlay(EndPlayReason);
}

void US1ItemUIComponent::OnInventoryKey()
{
	if (Root != nullptr)
		Root->SetInventoryOpen(Root->IsInventoryOpen() == false);
}

void US1ItemUIComponent::OnOpenKey()
{
	if (Root != nullptr)
		Root->ToggleLootNearest();
}

void US1ItemUIComponent::OnCloseKey()
{
	if (Root != nullptr)
		Root->CloseTopmost();
}

FText US1ItemUIComponent::KeyNameFor(int32 KeySlot) const
{
	// Fixed names only when nothing is bound — equipment-skill-binding.md B1 order.
	static const TCHAR* Fallback[] = { TEXT("?"), TEXT("Shift"), TEXT("Q"), TEXT("E"), TEXT("R"), TEXT("Z"), TEXT("X") };
	const FText FallbackName = FText::AsCultureInvariant(Fallback[FMath::Clamp(KeySlot, 0, 6)]);

	const AS1MyPlayer* Player = Cast<AS1MyPlayer>(GetOwner());
	return KeyNameOf(Player != nullptr ? Player->GetSkillAction(KeySlot) : nullptr, FallbackName);
}

FText US1ItemUIComponent::KeyNameOf(const UInputAction* Action, const FText& Fallback) const
{
	const APawn* Pawn = Cast<APawn>(GetOwner());
	const APlayerController* PC = Pawn != nullptr ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
	if (Action == nullptr || PC == nullptr)
		return Fallback;

	const UEnhancedInputLocalPlayerSubsystem* Input = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer());
	if (Input == nullptr)
		return Fallback;

	const TArray<FKey> Keys = Input->QueryKeysMappedToAction(Action);
	return Keys.Num() > 0 ? Keys[0].GetDisplayName(/*bLongDisplayName*/ false) : Fallback;
}
