// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "S1.h"
#include "S1GameInstance.generated.h"

class AS1Player;

/**
 * 
 */
UCLASS()
class S1_API US1GameInstance : public UGameInstance
{
	GENERATED_BODY()

public:
	virtual void Shutdown() override;

	UFUNCTION(BlueprintCallable)
	void ConnectToGameServer();

	/**
	 * Connects and sends C_LOGIN with this token — the login screen's token from the auth server (ADR-0011).
	 * Empty = ConnectToGameServer(): the command line, then the ini (dev). True when the socket connected and
	 * C_LOGIN went out; the answer comes as S_LOGIN.
	 */
	bool ConnectToGameServerWithToken(const FString& Token);

	UFUNCTION(BlueprintCallable)
	void DisconnectFromGameServer();

	UFUNCTION(BlueprintCallable)
	void HandleRecvPackets();

	void SendPacket(SendBufferRef SendBuffer);

private:
	FString ResolveLoginToken() const;

public:
	void HandleSpawn(const Protocol::ObjectInfo& PlayerInfo, bool IsMine);
	void HandleSpawn(const Protocol::S_ENTER_GAME& EnterGamePkt);
	void HandleSpawn(const Protocol::S_SPAWN& SpawnPkt);

	void HandleDespawn(uint64 ObjectId);
	void HandleDespawn(const Protocol::S_DESPAWN& DespawnPkt);

	/**
	 * S_LEAVE_GAME — the run is over. Destroys every other actor (players, monsters) and forgets every bag.
	 * The server sends nothing to a session outside the dungeon, so whatever left, died or expired while we sat
	 * in base camp would otherwise linger as a ghost after the next entry. The next S_SPAWN repopulates.
	 * Our own pawn stays — the server despawns it on death; after an escape HandleSpawn reuses it (2026-10-08).
	 */
	void ClearDungeonView();

	void HandleMove(const Protocol::S_MOVE& MovePkt);
	void HandleCc(const Protocol::S_CC& CcPkt);
	void HandleCcState(const Protocol::S_CC_STATE& StatePkt);
	void HandleDamage(const Protocol::S_DAMAGE& DamagePkt);
	void HandleEquipSync(const Protocol::S_EQUIP_SYNC& EquipPkt);
	void HandleSkillCast(const Protocol::S_SKILL_CAST& CastPkt);
	void HandleSkillCancel(const Protocol::S_SKILL_CANCEL& CancelPkt);
	void HandleSkillHit(const Protocol::S_SKILL_HIT& HitPkt);
	void HandleBagContents(const Protocol::S_BAG_CONTENTS BagPkt);
	
public:
	// GameServer
	class FSocket* Socket;
	FString IpAddress = TEXT("127.0.0.1");
	int16 Port = 7777;
	TSharedPtr<class PacketSession> GameServerSession;

public:
	UPROPERTY(EditAnywhere)
	TSubclassOf<AS1Player> OtherPlayerClass;

	TWeakObjectPtr<AS1Player> MyPlayer;
	TMap<uint64, TWeakObjectPtr<AS1Player>> Players;
	TArray<Protocol::SkillInfo> SkillSlots;
	TSet<uint64> Bags;
	TMap<uint64, TArray<uint64>> OpenBags;

	UFUNCTION(BlueprintCallable)
	void DebugEquipWeapon();

	UFUNCTION(BlueprintCallable)
	void DebugUnequipWeapon();

	UFUNCTION(BlueprintCallable)
	void DebugEquipIdempotent();

	UFUNCTION(BlueprintCallable)
	void DebugEquipBadSlot();

	UFUNCTION(Exec)
	void DebugEquip(int64 InstanceId, int32 Slot);

	UFUNCTION(Exec)
	void DebugUnequip(int64 InstanceId);

	UFUNCTION(Exec)
	void DebugBag();

	UFUNCTION(Exec)
	void DebugUnbag(int64 BagId);

	UFUNCTION(Exec)
	void DebugDie();

	UFUNCTION(Exec)
	void DebugOpenBag(int64 BagId);

	UFUNCTION(Exec)
	void DebugLoot(int64 BagId, int64 InstanceId);

	UFUNCTION(Exec)
	void DebugLootAll(int64 BagId);

	UFUNCTION(Exec)
	void DebugDrop(int64 InstanceId);

	UFUNCTION(Exec)
	void DebugLeave();

	UFUNCTION(Exec)
	void DebugEnter();

	UFUNCTION(Exec)
	void DebugVault(int64 InstanceId);

	UFUNCTION(Exec)
	void DebugUnvault(int64 InstanceId);

	UFUNCTION(Exec)
	void DebugEscape();
};
