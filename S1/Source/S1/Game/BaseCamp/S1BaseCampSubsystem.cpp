// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/BaseCamp/S1BaseCampSubsystem.h"
#include "Game/BaseCamp/S1CampScenery.h"
#include "Game/Item/S1ItemSubsystem.h"
#include "Game/Login/S1LoginSubsystem.h"
#include "Game/S1Player.h"
#include "Game/S1MyPlayer.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Game/UI/S1BaseCampWidget.h"
#include "Game/UI/S1DialogueWidget.h"
#include "Game/UI/S1ItemUIStyle.h"
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

	/** The conversation box - above the HUD, under S3 (an NPC window replaces it, never stacks on it). */
	constexpr int32 DialogueZOrder = 18;

	/** No answer to the camp room request in this long - ask again. */
	constexpr float CampRetrySeconds = 3.f;

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

	// The camp map - the server's basecamp.csv, next to entry_points.csv. Without it S3 stays the whole screen
	if (EntryDataDirectory.IsEmpty() == false && CampLayout.LoadFromDirectory(Directory, Error) == false)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CAMP] no camp map - %s (S3 stays the whole screen)"), *Error);
	}
	else if (CampLayout.IsLoaded())
	{
		UE_LOG(LogTemp, Log, TEXT("[CAMP] layout loaded - npcs %d props %d"), CampLayout.GetNpcs().Num(), CampLayout.GetProps().Num());
	}
}

void US1BaseCampSubsystem::Deinitialize()
{
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		GameInstance->GetTimerManager().ClearTimer(DeathReturnTimer);
		GameInstance->GetTimerManager().ClearTimer(CampRetryTimer);

		if (US1ItemSubsystem* Items = GameInstance->GetSubsystem<US1ItemSubsystem>())
			Items->OnInventorySynced.Remove(SyncedHandle);
	}

	Widget = nullptr;
	Dialogue = nullptr;
	Super::Deinitialize();
}

// ── Netcode hooks ────────────────────────────────────────────────────────

void US1BaseCampSubsystem::HandleLogin(const Protocol::S_LOGIN& Pkt)
{
	UE_LOG(LogTemp, Log, TEXT("[BASECAMP] login - characters=%d"), Pkt.characters_size());

	// The login screen steps aside - S3 takes the whole screen next (ADR-0011)
	if (US1LoginSubsystem* Login = GetGameInstance()->GetSubsystem<US1LoginSubsystem>())
		Login->HandleGameLoginSucceeded();

	if (Pkt.characters_size() > 0)
		MyCharacterId = Pkt.characters(0).object_info().object_id();

	SetPlace(ES1Place::BaseCamp);
	RequestCampMap();
}

void US1BaseCampSubsystem::HandleEnterAccepted(const Protocol::ObjectInfo& Player)
{
	MyCharacterId = Player.object_id();
	bLocalDead = false;

	// Leaving the camp map for a run - the scarecrow and anything else of the camp room go (the server drops our camp
	// body without a packet), the NPC windows and the conversation close, the camp hides
	if (bCampMap)
	{
		if (US1GameInstance* GameInstance = Cast<US1GameInstance>(GetGameInstance()))
			GameInstance->ClearDungeonView();
		bCampMap = false;
		CampSection = ES1CampSection::None;
		CloseDialogue();
		if (AS1CampScenery* Camp = Scenery.Get())
			Camp->SetShown(false);
	}

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

	// The server dropped our camp body when the entry began - ask for the camp room again (we land at the spawn)
	if (bCampMap)
		RequestCampMap();
}

void US1BaseCampSubsystem::HandleLeftGame()
{
	if (UGameInstance* GameInstance = GetGameInstance())
		GameInstance->GetTimerManager().ClearTimer(DeathReturnTimer);

	bLocalDead = false;
	SetPlace(ES1Place::BaseCamp);
	RequestCampMap();
}

// ── Base-camp map ────────────────────────────────────────────────────────

void US1BaseCampSubsystem::RequestCampMap()
{
	if (CampLayout.IsLoaded() == false || bCampGaveUp || IsInBaseCamp() == false)
		return;

	Protocol::C_ENTER_GAME Pkt;
	Pkt.set_playerindex(0);
	Pkt.set_floor_id(0);
	Pkt.set_mode(Protocol::RUN_MODE_BASECAMP);
	SEND_PACKET(Pkt);

	bCampPending = true;
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		GameInstance->GetTimerManager().SetTimer(CampRetryTimer, FTimerDelegate::CreateUObject(this, &US1BaseCampSubsystem::HandleCampTimeout),
			CampRetrySeconds, false);
	}

	UE_LOG(LogTemp, Log, TEXT("[CAMP] camp room requested (try %d)"), CampRetries + 1);
	ShowBaseCamp(IsInBaseCamp());
}

void US1BaseCampSubsystem::HandleCampTimeout()
{
	if (bCampPending == false || IsInBaseCamp() == false)
		return;

	// A death commit still running (the server refuses without the mode) or a lost packet - ask again a few times
	if (++CampRetries < CampRetryLimit)
	{
		RequestCampMap();
		return;
	}

	UE_LOG(LogTemp, Warning, TEXT("[CAMP] no camp room after %d tries - the whole-screen S3 instead"), CampRetries);
	bCampPending = false;
	bCampGaveUp = true;
	bCampMap = false;
	ShowBaseCamp(IsInBaseCamp());
}

void US1BaseCampSubsystem::HandleCampEntered(const Protocol::ObjectInfo& Player)
{
	MyCharacterId = Player.object_id();
	bLocalDead = false;
	bCampPending = false;
	CampRetries = 0;
	if (UGameInstance* GameInstance = GetGameInstance())
		GameInstance->GetTimerManager().ClearTimer(CampRetryTimer);

	// A fresh camp room each time - the last room's scarecrow (and anything left of a run) is not in this one
	if (US1GameInstance* GameInstance = Cast<US1GameInstance>(GetGameInstance()))
		GameInstance->ClearDungeonView();

	EnsureLocalPawn(Player);
	EnsureScenery();
	if (AS1CampScenery* Camp = Scenery.Get())
		Camp->SetShown(true);

	bCampMap = true;
	UE_LOG(LogTemp, Log, TEXT("[CAMP] entered the camp map at (%.0f, %.0f)"), Player.pos_info().x(), Player.pos_info().y());

	if (Place != ES1Place::BaseCamp)
		SetPlace(ES1Place::BaseCamp);
	else
		ShowBaseCamp(true);
}

void US1BaseCampSubsystem::HandleCampRejected()
{
	UE_LOG(LogTemp, Warning, TEXT("[CAMP] camp room refused"));
	// HandleCampTimeout retries or falls back - a refusal only makes it come sooner
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		GameInstance->GetTimerManager().SetTimer(CampRetryTimer, FTimerDelegate::CreateUObject(this, &US1BaseCampSubsystem::HandleCampTimeout),
			1.f, false);
	}
}

void US1BaseCampSubsystem::EnsureScenery()
{
	if (Scenery.IsValid() || CampLayout.IsLoaded() == false)
		return;

	UWorld* World = GetWorld();
	US1GameInstance* GameInstance = Cast<US1GameInstance>(GetGameInstance());
	if (World == nullptr || GameInstance == nullptr)
		return;

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AS1CampScenery* Camp = World->SpawnActor<AS1CampScenery>(AS1CampScenery::StaticClass(), FTransform::Identity, Params);
	if (Camp == nullptr)
		return;
	Camp->Build(CampLayout, GameInstance->OtherPlayerClass);
	Scenery = Camp;
}

const FS1CampPoint* US1BaseCampSubsystem::GetNpcInReach() const
{
	if (IsOnCampMap() == false)
		return nullptr;

	const US1GameInstance* GameInstance = Cast<US1GameInstance>(GetGameInstance());
	const AS1Player* Me = GameInstance != nullptr ? GameInstance->MyPlayer.Get() : nullptr;
	if (Me == nullptr)
		return nullptr;

	const FVector At = Me->GetActorLocation();
	return CampLayout.NearestNpc(At.X, At.Y, NpcTalkRangeCm);
}

void US1BaseCampSubsystem::InteractNearestNpc()
{
	if (IsOnCampMap() == false || CampSection != ES1CampSection::None || IsDialogueOpen() || Place != ES1Place::BaseCamp)
		return;

	const FS1CampPoint* Npc = GetNpcInReach();
	if (Npc == nullptr)
		return;

	APlayerController* PC = GetLocalPlayerController();
	if (PC == nullptr)
		return;

	if (Dialogue == nullptr)
	{
		Dialogue = CreateWidget<US1DialogueWidget>(PC, US1DialogueWidget::StaticClass());
		if (Dialogue == nullptr)
			return;
		Dialogue->OnChosen.BindUObject(this, &US1BaseCampSubsystem::ChooseDialogueOption);
	}

	if (Dialogue->IsInViewport() == false)
		Dialogue->AddToViewport(DialogueZOrder);
	Dialogue->Show(FS1CampLayout::DialogueFor(Npc->Id));

	UE_LOG(LogTemp, Log, TEXT("[CAMP] talk %s"), *Npc->Id);
	ShowBaseCamp(true);
}

bool US1BaseCampSubsystem::IsDialogueOpen() const
{
	return Dialogue != nullptr && Dialogue->IsInViewport();
}

void US1BaseCampSubsystem::CloseDialogue()
{
	if (Dialogue != nullptr && Dialogue->IsInViewport())
		Dialogue->RemoveFromParent();
}

void US1BaseCampSubsystem::ChooseDialogueOption(ES1CampAction Action)
{
	CloseDialogue();

	switch (Action)
	{
	case ES1CampAction::OpenStash:		OpenCampPanel(ES1CampSection::Stash); break;
	case ES1CampAction::OpenEnhance:	OpenCampPanel(ES1CampSection::Enhance); break;
	case ES1CampAction::OpenGate:		OpenCampPanel(ES1CampSection::Gate); break;
	case ES1CampAction::OpenSell:		OpenCampPanel(ES1CampSection::Sell); break;
	case ES1CampAction::OpenParty:
		OpenCampPanel(ES1CampSection::Gate);
		if (Widget != nullptr)
			Widget->OpenPartyFromCamp();
		break;
	default:
		ShowBaseCamp(IsInBaseCamp());
		break;
	}
}

void US1BaseCampSubsystem::OpenCampPanel(ES1CampSection Section)
{
	if (IsOnCampMap() == false)
		return;

	CloseDialogue();
	CampSection = Section;
	UE_LOG(LogTemp, Log, TEXT("[CAMP] panel %d"), static_cast<int32>(Section));
	ShowBaseCamp(true);
}

void US1BaseCampSubsystem::CloseCampPanel()
{
	if (CampSection == ES1CampSection::None)
		return;

	CampSection = ES1CampSection::None;
	UE_LOG(LogTemp, Log, TEXT("[CAMP] panel closed"));
	ShowBaseCamp(IsInBaseCamp());
}

TArray<FS1CampLabel> US1BaseCampSubsystem::GetCampLabels() const
{
	TArray<FS1CampLabel> Labels;
	const AS1CampScenery* Camp = Scenery.Get();
	if (IsOnCampMap() == false || Camp == nullptr)
		return Labels;

	const FS1CampPoint* Reach = (CampSection == ES1CampSection::None && IsDialogueOpen() == false) ? GetNpcInReach() : nullptr;
	for (const FS1CampNpcBody& Npc : Camp->GetNpcBodies())
	{
		const AS1Player* Body = Npc.Body.Get();
		if (Body == nullptr)
			continue;

		const FS1CampDialogue D = FS1CampLayout::DialogueFor(Npc.Id);
		FS1CampLabel& Name = Labels.AddDefaulted_GetRef();
		Name.World = Body->GetActorLocation();
		Name.Text = D.NameKey != nullptr ? S1ItemUIStyle::Text(D.NameKey) : FText::FromString(Npc.Id);

		if (Reach != nullptr && Reach->Id == Npc.Id)
		{
			FS1CampLabel& Prompt = Labels.AddDefaulted_GetRef();
			Prompt.World = Body->GetActorLocation();
			Prompt.Text = S1ItemUIStyle::Text(TEXT("camp.prompt.talk"));
			Prompt.bPrompt = true;
		}
	}
	return Labels;
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
	return Place == ES1Place::BaseCamp && bInventoryReady && GetEnterBlock() == S1PartyLogic::EEnterBlock::None;
}

void US1BaseCampSubsystem::SetRunMode(ES1RunMode Mode)
{
	if (static_cast<int32>(Mode) == RunModeIndex)
		return;

	RunModeIndex = static_cast<int32>(Mode);
	SaveConfig();
	UE_LOG(LogTemp, Log, TEXT("[BASECAMP] run mode %d"), RunModeIndex);
}

S1PartyLogic::EEnterBlock US1BaseCampSubsystem::GetEnterBlock() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	const US1PartySubsystem* Party = GameInstance != nullptr ? GameInstance->GetSubsystem<US1PartySubsystem>() : nullptr;
	return S1PartyLogic::EnterBlock(GetRunMode(), Party != nullptr ? Party->Size() : 0, Party != nullptr && Party->IsLeader(),
		Party != nullptr && Party->IsInRun(), Party != nullptr && Party->HasEnteredThisRun());
}

const TCHAR* US1BaseCampSubsystem::GetEnterLabelKey() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	const US1PartySubsystem* Party = GameInstance != nullptr ? GameInstance->GetSubsystem<US1PartySubsystem>() : nullptr;
	return S1PartyLogic::EnterLabelKey(GetRunMode(), Party != nullptr ? Party->Size() : 0, Party != nullptr && Party->IsLeader(),
		Party != nullptr && Party->IsInRun(), Place == ES1Place::Entering, Party != nullptr && Party->HasEnteredThisRun());
}

void US1BaseCampSubsystem::RequestEnter(uint32 EntryPointId)
{
	if (CanEnter() == false)
		return;

	// One character per account, one floor for now. entry_point_id 0 = the server picks one of the
	// floor's points (S4 [무작위]); an id it does not know on this floor is treated the same way.
	// The mode picks the room: PVP = the shared floor, PVE = a private copy of it (ADR-0014)
	Protocol::C_ENTER_GAME Pkt;
	Pkt.set_playerindex(0);
	Pkt.set_floor_id(FloorId);
	Pkt.set_entry_point_id(EntryPointId);
	Pkt.set_mode(static_cast<Protocol::RunMode>(static_cast<int32>(GetRunMode())));
	SEND_PACKET(Pkt);

	UE_LOG(LogTemp, Log, TEXT("[BASECAMP] enter requested - entry point %u%s mode=%d"), EntryPointId, EntryPointId == 0 ? TEXT(" (random)") : TEXT(""),
		static_cast<int32>(GetRunMode()));
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

	// What base camp shows (2026-10-17 camp map):
	//   on the camp map    nothing but the world - an NPC window (CampSection) or the conversation box when one is up
	//   asking for it      nothing, keys held (a moment - the server answers within a frame or two)
	//   no camp map        the whole-screen S3 as before (server without basecamp.csv · gave up asking)
	ES1CampSection Section = ES1CampSection::None;
	bool bHoldKeys = false;
	if (bShow)
	{
		if (IsOnCampMap())
			Section = CampSection;
		else if (bCampPending && bCampGaveUp == false)
			bHoldKeys = true;
		else
			Section = ES1CampSection::All;
	}

	if (Section != ES1CampSection::None)
	{
		CloseDialogue();
		if (Widget == nullptr)
			Widget = CreateWidget<US1BaseCampWidget>(PC, US1BaseCampWidget::StaticClass());

		if (Widget == nullptr)
			return;

		Widget->SetSection(Section);
		if (Widget->IsInViewport() == false)
			Widget->AddToViewport(BaseCampZOrder);
		Widget->SetKeyboardFocus();

		// UI only — WASD and the skill keys must not reach the pawn standing behind the window.
		ReleaseHeldInput(PC, /*bUiOnly*/ true);
		PC->bShowMouseCursor = true;
		PC->SetInputMode(FInputModeUIOnly().SetLockMouseToViewportBehavior(EMouseLockMode::LockAlways));
		return;
	}

	if (Widget != nullptr && Widget->IsInViewport())
		Widget->RemoveFromParent();

	if (bShow && (bHoldKeys || IsDialogueOpen()))
	{
		ReleaseHeldInput(PC, /*bUiOnly*/ true);
		PC->bShowMouseCursor = true;
		FInputModeUIOnly Mode;
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::LockAlways);
		if (IsDialogueOpen())
		{
			Mode.SetWidgetToFocus(Dialogue->TakeWidget());
		}
		PC->SetInputMode(Mode);
		return;
	}

	if (bShow == false)
		CloseDialogue();

	// Back to the dungeon's mode — the same one AS1MyPlayer sets: cursor aiming, nothing hidden.
	ReleaseHeldInput(PC, /*bUiOnly*/ false);
	PC->bShowMouseCursor = true;
	PC->SetInputMode(FInputModeGameAndUI()
		.SetLockMouseToViewportBehavior(EMouseLockMode::LockAlways)
		.SetHideCursorDuringCapture(false));
}

void US1BaseCampSubsystem::ReleaseHeldInput(APlayerController* PC, bool bUiOnly)
{
	if (PC == nullptr || bUiOnly == bInputUiOnly)
		return;
	bInputUiOnly = bUiOnly;

	// A key let go while a window owns the keyboard never reaches the player input - Enhanced Input then keeps W "held"
	// and the pawn walks on by itself once the window closes (2026-10-17 user report: NPC talk). Release everything at
	// every switch between UI-only and the game, and zero the pawn's own cached walk input with it
	PC->FlushPressedKeys();
	if (AS1MyPlayer* Me = Cast<AS1MyPlayer>(PC->GetPawn()))
	{
		Me->DoMove(0.f, 0.f);
		if (UCharacterMovementComponent* Movement = Me->GetCharacterMovement())
			Movement->StopMovementImmediately();
	}
	UE_LOG(LogTemp, Log, TEXT("[CAMP] input %s - held keys released"), bUiOnly ? TEXT("UI only") : TEXT("game"));
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
