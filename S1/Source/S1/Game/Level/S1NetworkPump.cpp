// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/Level/S1NetworkPump.h"

#include "S1GameInstance.h"
#include "Game/Login/S1LoginSubsystem.h"

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

	US1GameInstance* GameInstance = Cast<US1GameInstance>(GetGameInstance());
	if (GameInstance == nullptr)
		return;

	// A dev token (-token=, or DevLoginToken in editor binaries) connects right away as before. Otherwise the
	// login screen gets a token from the auth server and connects itself (ADR-0011)
	US1LoginSubsystem* Login = GameInstance->GetSubsystem<US1LoginSubsystem>();
	if (Login == nullptr || Login->BeginLogin(GetWorld()))
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
