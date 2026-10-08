// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/BaseCamp/S1BaseCampSubsystem.h"
#include "Game/Item/S1ItemSubsystem.h"
#include "Game/Login/S1LoginSubsystem.h"
#include "Game/UI/S1BaseCampWidget.h"
#include "S1.h"
#include "S1GameInstance.h"
#include "Protocol.pb.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Misc/Paths.h"

namespace
{
	/** Above the dungeon item HUD (10) — S3 is the whole screen in base camp. */
	constexpr int32 BaseCampZOrder = 20;

	const TCHAR* PlaceName(ES1Place Place)
	{
		switch (Place)
		{
		case ES1Place::BaseCamp:	return TEXT("BaseCamp");
		case ES1Place::Entering:	return TEXT("Entering");
		case ES1Place::Dungeon:		return TEXT("Dungeon");
		default:					return TEXT("Connecting");
		}
	}
}

US1BaseCampSubsystem* US1BaseCampSubsystem::Get()
{
	if (GWorld == nullptr)
		return nullptr;

	UGameInstance* GameInstance = GWorld->GetGameInstance();
	return GameInstance != nullptr ? GameInstance->GetSubsystem<US1BaseCampSubsystem>() : nullptr;
}

void US1BaseCampSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	if (US1ItemSubsystem* Items = Collection.InitializeDependency<US1ItemSubsystem>())
		SyncedHandle = Items->OnInventorySynced.AddUObject(this, &US1BaseCampSubsystem::HandleInventorySynced);

	// S4 (진입 지점 지도) — the server's own entry_points.csv, read the way the item catalog reads item_types.csv
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), EntryDataDirectory));
	FString Error;
	if (EntryDataDirectory.IsEmpty() || EntryPoints.LoadFromDirectory(Directory, Error) == false)
	{
		// Not fatal: S4 shows an empty map and [무작위] still works — the server picks.
		UE_LOG(LogTemp, Error, TEXT("[BASECAMP] entry points not loaded - dir=%s error=%s"),
			*Directory, EntryDataDirectory.IsEmpty() ? TEXT("EntryDataDirectory is not set") : *Error);
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("[BASECAMP] entry points loaded - %d"), EntryPoints.Num());
	}
}

void US1BaseCampSubsystem::Deinitialize()
{
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		GameInstance->GetTimerManager().ClearTimer(DeathReturnTimer);

		if (US1ItemSubsystem* Items = GameInstance->GetSubsystem<US1ItemSubsystem>())
			Items->OnInventorySynced.Remove(SyncedHandle);
	}

	Widget = nullptr;
	Super::Deinitialize();
}

// ── Netcode hooks ────────────────────────────────────────────────────────

void US1BaseCampSubsystem::HandleLogin(const Protocol::S_LOGIN& Pkt)
{
	UE_LOG(LogTemp, Log, TEXT("[BASECAMP] login - characters=%d"), Pkt.characters_size());

	// The login screen steps aside - S3 takes the whole screen next (ADR-0011)
	if (US1LoginSubsystem* Login = GetGameInstance()->GetSubsystem<US1LoginSubsystem>())
		Login->HandleGameLoginSucceeded();

	SetPlace(ES1Place::BaseCamp);
}

void US1BaseCampSubsystem::HandleEnterAccepted(const Protocol::ObjectInfo& Player)
{
	MyCharacterId = Player.object_id();
	bLocalDead = false;

	EnsureLocalPawn(Player);
	SetPlace(ES1Place::Dungeon);
}

void US1BaseCampSubsystem::HandleEnterRejected()
{
	// A stray rejection while already in the dungeon (a reattach race) is not ours to show.
	if (Place != ES1Place::Entering)
	{
		UE_LOG(LogTemp, Warning, TEXT("[BASECAMP] enter rejected while %s - ignored"), PlaceName(Place));
		return;
	}

	UE_LOG(LogTemp, Warning, TEXT("[BASECAMP] enter rejected"));
	SetPlace(ES1Place::BaseCamp);
	OnEnterRejected.Broadcast();
}

void US1BaseCampSubsystem::HandleLeftGame()
{
	if (UGameInstance* GameInstance = GetGameInstance())
		GameInstance->GetTimerManager().ClearTimer(DeathReturnTimer);

	bLocalDead = false;
	SetPlace(ES1Place::BaseCamp);
}

void US1BaseCampSubsystem::HandleDied(uint64 VictimId)
{
	if (Place != ES1Place::Dungeon || MyCharacterId == 0 || VictimId != MyCharacterId || bLocalDead)
		return;

	bLocalDead = true;
	OnLocalDeath.Broadcast();

	UE_LOG(LogTemp, Log, TEXT("[BASECAMP] we died - back to base camp in %.1fs"), DeathReturnSeconds);

	// The death presentation will live inside this wait. Until then it is just a pause.
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		GameInstance->GetTimerManager().SetTimer(DeathReturnTimer, FTimerDelegate::CreateUObject(this, &US1BaseCampSubsystem::SendLeave),
			FMath::Max(DeathReturnSeconds, 0.01f), false);
	}
}

// ── UI ───────────────────────────────────────────────────────────────────

bool US1BaseCampSubsystem::CanEnter() const
{
	return Place == ES1Place::BaseCamp && bInventoryReady;
}

void US1BaseCampSubsystem::RequestEnter(uint32 EntryPointId)
{
	if (CanEnter() == false)
		return;

	// One character per account, one floor for now. entry_point_id 0 = the server picks one of the
	// floor's points (S4 [무작위]); an id it does not know on this floor is treated the same way.
	Protocol::C_ENTER_GAME Pkt;
	Pkt.set_playerindex(0);
	Pkt.set_floor_id(FloorId);
	Pkt.set_entry_point_id(EntryPointId);
	SEND_PACKET(Pkt);

	UE_LOG(LogTemp, Log, TEXT("[BASECAMP] enter requested - entry point %u%s"), EntryPointId, EntryPointId == 0 ? TEXT(" (random)") : TEXT(""));
	SetPlace(ES1Place::Entering);
}

// ── Internals ────────────────────────────────────────────────────────────

void US1BaseCampSubsystem::SetPlace(ES1Place NewPlace)
{
	const ES1Place Old = Place;
	Place = NewPlace;

	// Coming back from the dungeon (or logging in): the old inventory view is stale until the
	// server's base-camp sync lands. A rejected entry keeps it — nothing moved.
	if (NewPlace == ES1Place::BaseCamp && Old != ES1Place::Entering)
		bInventoryReady = false;

	ShowBaseCamp(IsInBaseCamp());

	UE_LOG(LogTemp, Log, TEXT("[BASECAMP] place %s -> %s ready=%d"), PlaceName(Old), PlaceName(NewPlace), bInventoryReady ? 1 : 0);
	OnPlaceChanged.Broadcast(NewPlace);
}

void US1BaseCampSubsystem::HandleInventorySynced(bool bBaseCamp)
{
	if (bBaseCamp == false || IsInBaseCamp() == false)
		return;

	bInventoryReady = true;
	OnInventoryReady.Broadcast();
}

void US1BaseCampSubsystem::ShowBaseCamp(bool bShow)
{
	APlayerController* PC = GetLocalPlayerController();
	if (PC == nullptr)
		return;

	if (bShow)
	{
		if (Widget == nullptr)
			Widget = CreateWidget<US1BaseCampWidget>(PC, US1BaseCampWidget::StaticClass());

		if (Widget == nullptr)
			return;

		if (Widget->IsInViewport() == false)
			Widget->AddToViewport(BaseCampZOrder);

		// S3 is the whole screen. UI only — WASD and the skill keys must not reach a pawn standing behind it.
		PC->bShowMouseCursor = true;
		PC->SetInputMode(FInputModeUIOnly().SetLockMouseToViewportBehavior(EMouseLockMode::LockAlways));
		return;
	}

	if (Widget != nullptr && Widget->IsInViewport())
		Widget->RemoveFromParent();

	// Back to the dungeon's mode — the same one AS1MyPlayer sets: cursor aiming, nothing hidden.
	PC->bShowMouseCursor = true;
	PC->SetInputMode(FInputModeGameAndUI()
		.SetLockMouseToViewportBehavior(EMouseLockMode::LockAlways)
		.SetHideCursorDuringCapture(false));
}

void US1BaseCampSubsystem::SendLeave()
{
	if (Place != ES1Place::Dungeon)
		return;

	// The server holds this until the death commit is in, so S_LEAVE_GAME means "your bag is made".
	Protocol::C_LEAVE_GAME Pkt;
	SEND_PACKET(Pkt);

	UE_LOG(LogTemp, Log, TEXT("[BASECAMP] leave requested after death"));
}

void US1BaseCampSubsystem::EnsureLocalPawn(const Protocol::ObjectInfo& Player)
{
	APlayerController* PC = GetLocalPlayerController();
	UWorld* World = GetWorld();
	if (PC == nullptr || World == nullptr || PC->GetPawn() != nullptr)
		return;

	// Our pawn was destroyed when the server despawned us at the end of the last run.
	// US1GameInstance::HandleSpawn only positions the pawn it finds, so put one there first.
	const AGameModeBase* GameMode = World->GetAuthGameMode();
	UClass* PawnClass = GameMode != nullptr ? GameMode->DefaultPawnClass.Get() : nullptr;
	if (PawnClass == nullptr)
	{
		UE_LOG(LogTemp, Error, TEXT("[BASECAMP] no default pawn class - cannot respawn our character"));
		return;
	}

	const Protocol::PosInfo& Pos = Player.pos_info();
	const FTransform Transform(FRotator(0.0, Pos.yaw(), 0.0), FVector(Pos.x(), Pos.y(), Pos.z()));

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

	APawn* Pawn = World->SpawnActor<APawn>(PawnClass, Transform, Params);
	if (Pawn == nullptr)
	{
		UE_LOG(LogTemp, Error, TEXT("[BASECAMP] respawn failed - class=%s"), *GetNameSafe(PawnClass));
		return;
	}

	PC->Possess(Pawn);
	UE_LOG(LogTemp, Log, TEXT("[BASECAMP] respawned our pawn - %s at (%.0f, %.0f)"), *GetNameSafe(Pawn), Pos.x(), Pos.y());
}

APlayerController* US1BaseCampSubsystem::GetLocalPlayerController() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance != nullptr ? GameInstance->GetFirstLocalPlayerController() : nullptr;
}
