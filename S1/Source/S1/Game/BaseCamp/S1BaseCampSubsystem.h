// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "TimerManager.h"
#include "Game/BaseCamp/S1EntryPoints.h"
#include "Game/BaseCamp/S1CampLayout.h"
#include "Game/Party/S1PartySubsystem.h"
#include "S1BaseCampSubsystem.generated.h"

namespace Protocol
{
	class S_LOGIN;
	class ObjectInfo;
}

class APlayerController;
class AS1CampScenery;
class US1BaseCampWidget;
class US1DialogueWidget;

/** A name (or the talk prompt) to paint over an NPC — US1WorldLabelWidget. */
struct FS1CampLabel
{
	FVector	World = FVector::ZeroVector;
	FText	Text;
	bool	bPrompt = false;
};

/** Where this client is, as far as the server has said. design/ux/item-ui.md § Navigation Position. */
enum class ES1Place : uint8
{
	Connecting = 0,		// Before S_LOGIN
	BaseCamp,			// S3 is the screen
	Entering,			// [입장] sent, waiting for S_ENTER_GAME
	Dungeon,
};

DECLARE_MULTICAST_DELEGATE_OneParam(FOnS1PlaceChanged, ES1Place);
DECLARE_MULTICAST_DELEGATE(FOnS1BaseCampEvent);

/**
 * The base camp ↔ dungeon flow on the client — UI stage 4.
 *
 *   S_LOGIN ──▶ BaseCamp (S3) ──[입장]──▶ Entering ──S_ENTER_GAME──▶ Dungeon
 *      ▲                                     │ rejected                 │
 *      └──────── S_LEAVE_GAME ◀──────────────┴──── escape · death ──────┘
 *
 * S3 lives here and not on the pawn: the server despawns our own character when it leaves the
 * dungeon, and the pawn takes the dungeon HUD with it. The player controller outlives both.
 *
 * Camp map (2026-10-17, design/quick-specs/basecamp-map-2026-10-17.md): in BaseCamp the client asks for its private
 * camp room (C_ENTER_GAME{mode=BASECAMP}) and walks around it — NPCs open the dialogue box, then one part of S3
 * (stash · enhance · gate). The place stays BaseCamp the whole time; without the camp map S3 is the whole screen as before.
 *
 * Packets come in through the Handle* hooks in ClientPacketHandler.cpp (H7 ~ H10).
 */
UCLASS(Config = Game)
class S1_API US1BaseCampSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:

	static US1BaseCampSubsystem* Get();

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// ── Netcode hooks ────────────────────────────────────────────────────

	/** H7. S_LOGIN succeeded. No automatic entry any more — the player picks a loadout first. */
	void HandleLogin(const Protocol::S_LOGIN& Pkt);

	/**
	 * H8. S_ENTER_GAME succeeded. Call before US1GameInstance::HandleSpawn — when our pawn went
	 * with the last run, a new one is spawned and possessed here so HandleSpawn finds it.
	 */
	void HandleEnterAccepted(const Protocol::ObjectInfo& Player);

	/** H8. S_ENTER_GAME refused. Back to S3 with a reason. */
	void HandleEnterRejected();

	/** H9. S_LEAVE_GAME — the run is over and the server has committed it. */
	void HandleLeftGame();

	/** H10. Someone died. If it was us, we go back to base camp after DeathReturnSeconds. */
	void HandleDied(uint64 VictimId);

	// ── Base-camp map (2026-10-17, design/quick-specs/basecamp-map-2026-10-17.md) ─────────

	/**
	 * S_ENTER_GAME{mode=BASECAMP} succeeded — our body stands in the private camp room. Call before
	 * US1GameInstance::HandleSpawn (like HandleEnterAccepted): the camp view is cleared and our pawn ensured first.
	 */
	void HandleCampEntered(const Protocol::ObjectInfo& Player);

	/** S_ENTER_GAME{mode=BASECAMP} refused — retried a few times, then the old whole-screen S3. */
	void HandleCampRejected();

	/** Walking around the camp: base camp, the server put our body in the camp room, the map loaded. */
	bool IsOnCampMap() const { return bCampMap && IsInBaseCamp(); }

	/** F near an NPC — the conversation box. Nothing when no one is in reach or a window is already up. */
	void InteractNearestNpc();

	/** The NPC within talking range of our pawn, or nullptr. */
	const FS1CampPoint* GetNpcInReach() const;

	/** The NPC windows: Stash · Enhance · Gate (None = back to walking). */
	void OpenCampPanel(ES1CampSection Section);
	void CloseCampPanel();
	ES1CampSection GetCampSection() const { return CampSection; }

	/** NPC names, and "[F] 대화" over the one in reach. Empty off the camp map. */
	TArray<FS1CampLabel> GetCampLabels() const;

	const FS1CampLayout& GetCampLayout() const { return CampLayout; }

	// ── UI ───────────────────────────────────────────────────────────────

	/**
	 * S4 picked a point (or [무작위] = 0). Only from base camp, and only once the base-camp inventory
	 * has arrived. design/ux/entry-point.md
	 */
	void RequestEnter(uint32 EntryPointId);

	/** [입장] may open S4 and S4 may send — base camp, inventory arrived, nothing in flight, and the mode fits the party. */
	bool CanEnter() const;

	// ── Run mode (PVE, ADR-0014) ─────────────────────────────────────────

	/** PVP · PVE 솔로 · PVE 협동 — chosen on S3, remembered between sessions. */
	void SetRunMode(ES1RunMode Mode);
	ES1RunMode GetRunMode() const { return static_cast<ES1RunMode>(FMath::Clamp(RunModeIndex, 0, 2)); }

	/** Why the chosen mode cannot enter now (the server refuses the same) — S1PartyLogic::EnterBlock with my party. */
	S1PartyLogic::EEnterBlock GetEnterBlock() const;

	/** What [입장] says: 입장 · 입장 중 · 함께 입장 (co-op leader) · 합류 (party run alive) · 파티장 대기 (member). */
	const TCHAR* GetEnterLabelKey() const;

	/** The mode of the run we are in or last entered (S_ENTER_GAME.mode). */
	ES1RunMode GetCurrentRunMode() const { return CurrentRunMode; }
	void SetCurrentRunMode(ES1RunMode Mode) { CurrentRunMode = Mode; }

	/** The floor S4 shows and C_ENTER_GAME asks for. One floor for now. */
	uint32 GetFloorId() const { return FloorId; }
	const FS1EntryPoints& GetEntryPoints() const { return EntryPoints; }

	/** Half the side of the square S4 draws, in cm — the TEMP floor plan until the real map has a picture. */
	float GetMapHalfExtentCm() const { return MapHalfExtentCm; }

	ES1Place GetPlace() const { return Place; }

	/** Base camp, including the wait after [입장] — the inventory sync there carries the vault. */
	bool IsInBaseCamp() const { return Place == ES1Place::BaseCamp || Place == ES1Place::Entering; }

	/** The base-camp inventory sync has arrived since we last came back. Until then S3 says "불러오는 중". */
	bool IsInventoryReady() const { return bInventoryReady; }

	/** Our character died this run — the dungeon HUD stays shut until we are back in base camp. */
	bool IsLocalDead() const { return bLocalDead; }

	FOnS1PlaceChanged	OnPlaceChanged;
	FOnS1BaseCampEvent	OnInventoryReady;
	FOnS1BaseCampEvent	OnEnterRejected;
	FOnS1BaseCampEvent	OnLocalDeath;

private:

	void SetPlace(ES1Place NewPlace);
	void ShowBaseCamp(bool bShow);

	/** Asks the server for the camp room (C_ENTER_GAME{mode=BASECAMP}). Bots and old clients never do. */
	void RequestCampMap();
	void HandleCampTimeout();
	void EnsureScenery();
	void ChooseDialogueOption(ES1CampAction Action);
	void CloseDialogue();
	bool IsDialogueOpen() const;

	/** Flush held keys and the pawn's walk input when the input mode flips between UI-only and the game. */
	void ReleaseHeldInput(APlayerController* PC, bool bUiOnly);
	bool bInputUiOnly = false;
	void HandleInventorySynced(bool bBaseCamp);
	void SendLeave();
	void EnsureLocalPawn(const Protocol::ObjectInfo& Player);
	APlayerController* GetLocalPlayerController() const;

	/** Seconds between our death and the automatic trip back to base camp (2026-10-04: 3 s). */
	UPROPERTY(Config)
	float DeathReturnSeconds = 3.f;

	/** Where entry_points.csv lives, relative to the project — the server's Server/Data, like ItemDataDirectory. */
	UPROPERTY(Config)
	FString EntryDataDirectory;

	UPROPERTY(Config)
	float MapHalfExtentCm = 3000.f;

	/** S3 mode row (ES1RunMode) — saved to the user's config when changed. */
	UPROPERTY(Config)
	int32 RunModeIndex = 0;

	ES1RunMode			CurrentRunMode = ES1RunMode::Pvp;

	UPROPERTY() TObjectPtr<US1BaseCampWidget>	Widget;
	UPROPERTY() TObjectPtr<US1DialogueWidget>	Dialogue;

	/** How close (cm, from the NPC's body edge) F starts a conversation. */
	UPROPERTY(Config)
	float NpcTalkRangeCm = 300.f;

	/** The camp room request is retried this many times (3 s apart) before falling back to the whole-screen S3. */
	UPROPERTY(Config)
	int32 CampRetryLimit = 5;

	FS1CampLayout					CampLayout;
	TWeakObjectPtr<AS1CampScenery>	Scenery;
	ES1CampSection					CampSection = ES1CampSection::None;
	bool							bCampMap = false;		// the server put our body in the camp room
	bool							bCampPending = false;	// C_ENTER_GAME{BASECAMP} in flight
	bool							bCampGaveUp = false;	// no camp room this session - S3 is the whole screen
	int32							CampRetries = 0;
	FTimerHandle					CampRetryTimer;

	FS1EntryPoints		EntryPoints;
	uint32				FloorId = 0;

	ES1Place			Place = ES1Place::Connecting;
	uint64				MyCharacterId = 0;
	bool				bInventoryReady = false;
	bool				bLocalDead = false;
	FTimerHandle		DeathReturnTimer;
	FDelegateHandle		SyncedHandle;
};
