// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/UI/S1ItemUIComponent.h"
#include "Game/UI/S1ItemHudRootWidget.h"
#include "Game/UI/S1InventoryPanelWidget.h"
#include "Game/UI/S1PartyWidgets.h"
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
	PartyAction = CreateDefaultSubobject<UInputAction>(TEXT("IA_Party"));
	Context = CreateDefaultSubobject<UInputMappingContext>(TEXT("IMC_ItemUI"));

	Context->MapKey(InventoryAction, EKeys::Tab);
	Context->MapKey(OpenAction, EKeys::F);
	Context->MapKey(CloseAction, EKeys::Escape);
	Context->MapKey(PartyAction, EKeys::P);
}

void US1ItemUIComponent::SetupInput(UEnhancedInputComponent* Input)
{
	if (Input == nullptr)
		return;

	Input->BindAction(InventoryAction, ETriggerEvent::Started, this, &US1ItemUIComponent::OnInventoryKey);
	Input->BindAction(OpenAction, ETriggerEvent::Started, this, &US1ItemUIComponent::OnOpenKey);
	Input->BindAction(CloseAction, ETriggerEvent::Started, this, &US1ItemUIComponent::OnCloseKey);
	Input->BindAction(PartyAction, ETriggerEvent::Started, this, &US1ItemUIComponent::OnPartyKey);
}

void US1ItemUIComponent::BeginPlay()
{
	Super::BeginPlay();

	if (US1BaseCampSubsystem* BaseCamp = GetWorld() != nullptr && GetWorld()->GetGameInstance() != nullptr
		? GetWorld()->GetGameInstance()->GetSubsystem<US1BaseCampSubsystem>() : nullptr)
	{
		PlaceChangedHandle = BaseCamp->OnPlaceChanged.AddUObject(this, &US1ItemUIComponent::HandlePlaceChanged);
		LocalDeathHandle = BaseCamp->OnLocalDeath.AddUObject(this, &US1ItemUIComponent::HandleLocalDeath);
	}

	InitForLocalController();
}

void US1ItemUIComponent::InitForLocalController()
{
	if (Root != nullptr)
		return;

	APlayerController* PC = GetLocalController();
	if (PC == nullptr)
	{
		UE_LOG(LogTemp, Log, TEXT("[ITEMUI] ui waits - owner=%s has no local controller yet"), *GetNameSafe(GetOwner()));
		return;
	}

	if (UEnhancedInputLocalPlayerSubsystem* Input = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
		Input->AddMappingContext(Context, ItemUIContextPriority);

	Root = CreateWidget<US1ItemHudRootWidget>(PC, US1ItemHudRootWidget::StaticClass());
	if (Root == nullptr)
		return;

	Root->SetKeyNameResolver(
		[this](int32 KeySlot) { return KeyNameForController(GetLocalController(), KeySlot); },
		[this]() { return KeyNameOf(GetLocalController(), OpenAction, FText::AsCultureInvariant(TEXT("F"))); });

	Root->AddToViewport(ItemUIZOrder);

	UE_LOG(LogTemp, Log, TEXT("[ITEMUI] ui root on viewport=%d owner=%s"), Root->IsInViewport() ? 1 : 0, *GetNameSafe(GetOwner()));
}

void US1ItemUIComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		if (US1BaseCampSubsystem* BaseCamp = World->GetGameInstance() != nullptr ? World->GetGameInstance()->GetSubsystem<US1BaseCampSubsystem>() : nullptr)
		{
			BaseCamp->OnPlaceChanged.Remove(PlaceChangedHandle);
			BaseCamp->OnLocalDeath.Remove(LocalDeathHandle);
		}
	}

	// The pawn goes when the server despawns us at the start of an escape commit — the panels go
	// with it, which is exactly the close design/ux/item-ui.md § Entry & Exit asks for (B2).
	if (Root != nullptr)
	{
		Root->RemoveFromParent();
		Root = nullptr;
	}

	// 던전 파티 창(P)도 몸과 같이 간다 - 예전엔 열린 채 탈출 · 사망하면 베이스캠프 화면(Z 20) 위(Z 30)에 남아
	// 전체 화면 어둠이 클릭을 삼켰고, 다음 몸이 창을 하나 더 만들었다 (2026-10-17 리뷰)
	if (PartyPanel != nullptr)
	{
		PartyPanel->Close();
		PartyPanel = nullptr;
	}

	if (const APlayerController* PC = GetLocalController())
	{
		if (UEnhancedInputLocalPlayerSubsystem* Input = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
			Input->RemoveMappingContext(Context);
	}

	Super::EndPlay(EndPlayReason);
}

bool US1ItemUIComponent::CanUseDungeonKeys() const
{
	const UWorld* World = GetWorld();
	const US1BaseCampSubsystem* BaseCamp = World != nullptr && World->GetGameInstance() != nullptr
		? World->GetGameInstance()->GetSubsystem<US1BaseCampSubsystem>() : nullptr;

	// No flow subsystem (a test map) — behave as before stage 4.
	return BaseCamp == nullptr || (BaseCamp->GetPlace() == ES1Place::Dungeon && BaseCamp->IsLocalDead() == false);
}

void US1ItemUIComponent::HandlePlaceChanged(ES1Place Place)
{
	// Leaving the dungeon any way at all shuts S1 and S2 (§ Entry & Exit).
	if (Place != ES1Place::Dungeon && Root != nullptr)
		Root->CloseAll();
}

void US1ItemUIComponent::HandleLocalDeath()
{
	// S1 and S2 close the moment we die — the death itself is the reason (§ Entry & Exit).
	if (Root != nullptr)
		Root->CloseAll();
}

void US1ItemUIComponent::OnInventoryKey()
{
	if (Root != nullptr && CanUseDungeonKeys())
		Root->SetInventoryOpen(Root->IsInventoryOpen() == false);
}

void US1ItemUIComponent::OnPartyKey()
{
	// 던전 파티 창 (party-betrayal R10): 초대(이름) · 파티원 · [배신하고 나가기]. 베이스캠프에선 S3 의 [파티] 버튼이 연다
	if (CanUseDungeonKeys() == false)
		return;
	if (PartyPanel != nullptr && PartyPanel->IsInViewport())
	{
		PartyPanel->Close();
		return;
	}
	APlayerController* PC = GetLocalController();
	if (PC == nullptr)
		return;
	if (PartyPanel == nullptr)
		PartyPanel = CreateWidget<US1PartyPanelWidget>(PC, US1PartyPanelWidget::StaticClass());
	if (PartyPanel != nullptr)
		PartyPanel->AddToViewport(30);
}

void US1ItemUIComponent::OnOpenKey()
{
	if (Root != nullptr && CanUseDungeonKeys())
	{
		Root->ToggleLootNearest();
		return;
	}

	// Camp map (2026-10-17): the same F talks to the NPC in reach - "열기" in the dungeon, "대화" in the camp
	const UWorld* World = GetWorld();
	if (US1BaseCampSubsystem* BaseCamp = World != nullptr && World->GetGameInstance() != nullptr
		? World->GetGameInstance()->GetSubsystem<US1BaseCampSubsystem>() : nullptr; BaseCamp != nullptr && BaseCamp->IsOnCampMap())
	{
		BaseCamp->InteractNearestNpc();
	}
}

void US1ItemUIComponent::OnCloseKey()
{
	if (Root != nullptr)
		Root->CloseTopmost();
}

APlayerController* US1ItemUIComponent::GetLocalController() const
{
	const APawn* Pawn = Cast<APawn>(GetOwner());
	APlayerController* PC = Pawn != nullptr ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
	return (PC != nullptr && PC->IsLocalController()) ? PC : nullptr;
}

FText US1ItemUIComponent::KeyNameForController(const APlayerController* PC, int32 KeySlot)
{
	// Fixed names only when nothing is bound — equipment-skill-binding.md B1 order.
	static const TCHAR* Fallback[] = { TEXT("?"), TEXT("Shift"), TEXT("Q"), TEXT("E"), TEXT("R"), TEXT("Z"), TEXT("X") };
	const FText FallbackName = FText::AsCultureInvariant(Fallback[FMath::Clamp(KeySlot, 0, 6)]);

	const AS1MyPlayer* Player = PC != nullptr ? Cast<AS1MyPlayer>(PC->GetPawn()) : nullptr;
	return KeyNameOf(PC, Player != nullptr ? Player->GetSkillAction(KeySlot) : nullptr, FallbackName);
}

FText US1ItemUIComponent::KeyNameOf(const APlayerController* PC, const UInputAction* Action, const FText& Fallback)
{
	if (Action == nullptr || PC == nullptr)
		return Fallback;

	const UEnhancedInputLocalPlayerSubsystem* Input = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer());
	if (Input == nullptr)
		return Fallback;

	const TArray<FKey> Keys = Input->QueryKeysMappedToAction(Action);
	return Keys.Num() > 0 ? Keys[0].GetDisplayName(/*bLongDisplayName*/ false) : Fallback;
}
