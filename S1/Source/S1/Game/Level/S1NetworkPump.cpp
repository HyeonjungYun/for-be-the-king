// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/Level/S1NetworkPump.h"

#include "S1GameInstance.h"

AS1NetworkPump::AS1NetworkPump()
{
	PrimaryActorTick.bCanEverTick = true;
	// A level script actor ticks in the default group too — packets are handled before pawns move this frame
	PrimaryActorTick.TickGroup = TG_PrePhysics;
	SetHidden(true);
}

void AS1NetworkPump::BeginPlay()
{
	Super::BeginPlay();

	if (US1GameInstance* GameInstance = Cast<US1GameInstance>(GetGameInstance()))
	{
		GameInstance->ConnectToGameServer();
	}
}

void AS1NetworkPump::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (US1GameInstance* GameInstance = Cast<US1GameInstance>(GetGameInstance()))
	{
		GameInstance->HandleRecvPackets();
	}
}
