// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "S1NetworkPump.generated.h"

/**
 * Connects to the game server on BeginPlay and drains received packets every tick — the two nodes DevMap's level
 * blueprint held (BeginPlay → ConnectToGameServer, Tick → HandleRecvPackets, read 2026-10-07).
 *
 * Floor1 is rebuilt from nothing by the graybox commandlet, so it cannot carry level blueprint logic; the commandlet
 * places one of these instead (ADR-0006 Migration 4). A map needs exactly one — two would connect twice.
 */
UCLASS()
class S1_API AS1NetworkPump : public AActor
{
	GENERATED_BODY()

public:
	AS1NetworkPump();

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
};
