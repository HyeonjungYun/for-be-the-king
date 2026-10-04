// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "TimerManager.h"
#include "S1BaseCampSubsystem.generated.h"

namespace Protocol
{
	class S_LOGIN;
	class ObjectInfo;
}

class APlayerController;
class US1BaseCampWidget;

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

	// ── UI ───────────────────────────────────────────────────────────────

	/** [입장]. Only from base camp, and only once the base-camp inventory has arrived. */
	void RequestEnter();

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
	void HandleInventorySynced(bool bBaseCamp);
	void SendLeave();
	void EnsureLocalPawn(const Protocol::ObjectInfo& Player);
	APlayerController* GetLocalPlayerController() const;

	/** Seconds between our death and the automatic trip back to base camp (2026-10-04: 3 s). */
	UPROPERTY(Config)
	float DeathReturnSeconds = 3.f;

	UPROPERTY() TObjectPtr<US1BaseCampWidget>	Widget;

	ES1Place			Place = ES1Place::Connecting;
	uint64				MyCharacterId = 0;
	bool				bInventoryReady = false;
	bool				bLocalDead = false;
	FTimerHandle		DeathReturnTimer;
	FDelegateHandle		SyncedHandle;
};
