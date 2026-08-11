// Fill out your copyright notice in the Description page of Project Settings.


#include "Game/S1Player.h"
#include "Engine/LocalPlayer.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/Controller.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputActionValue.h"
#include "S1MyPlayer.h"
#include "S1.h"

AS1Player::AS1Player()
{
	// Set size for collision capsule
	GetCapsuleComponent()->InitCapsuleSize(42.f, 96.0f);

	// Don't rotate when the controller rotates. Let that just affect the camera.
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;

	// Configure character movement
	GetCharacterMovement()->bOrientRotationToMovement = true;
	GetCharacterMovement()->RotationRate = FRotator(0.0f, 500.0f, 0.0f);

	// Note: For faster iteration times these variables, and many more, can be tweaked in the Character Blueprint
	// instead of recompiling to adjust them
	GetCharacterMovement()->JumpZVelocity = 500.f;
	GetCharacterMovement()->AirControl = 0.35f;
	GetCharacterMovement()->MaxWalkSpeed = 500.f;
	GetCharacterMovement()->MinAnalogWalkSpeed = 20.f;
	GetCharacterMovement()->BrakingDecelerationWalking = 2000.f;
	GetCharacterMovement()->BrakingDecelerationFalling = 1500.0f;

	GetCharacterMovement()->bRunPhysicsWithNoController = true;

	PlayerInfo = new Protocol::PosInfo();
	DestInfo = new Protocol::PosInfo();
}

AS1Player::~AS1Player()
{
	delete PlayerInfo;
	delete DestInfo;
	PlayerInfo = nullptr;
	DestInfo = nullptr;
}

void AS1Player::SetMoveState(Protocol::MoveState State)
{
	if (PlayerInfo->state() == State)
		return;

	PlayerInfo->set_state(State);

	// TODO
}

void AS1Player::BeginPlay()
{
	Super::BeginPlay();
	
	{
		FVector Location = GetActorLocation();
		DestInfo->set_x(Location.X);
		DestInfo->set_y(Location.Y);
		DestInfo->set_z(Location.Z);
		DestInfo->set_yaw(GetControlRotation().Yaw);

		SetMoveState(Protocol::MOVE_STATE_IDLE);
	}

	if (IsMyPlayer() == false)
	{
		UCharacterMovementComponent* Movement = GetCharacterMovement();
		Movement->SetMovementMode(MOVE_None);
		Movement->bOrientRotationToMovement = false;
		Movement->StopMovementImmediately();

		InterpStartLocation = GetActorLocation();
		InterpStartYaw = GetActorRotation().Yaw;
		InterpElapsed = InterpDuration;

	}

}

void AS1Player::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	{
		FVector Location = GetActorLocation();
		PlayerInfo->set_x(Location.X);
		PlayerInfo->set_y(Location.Y);
		PlayerInfo->set_z(Location.Z);
		PlayerInfo->set_yaw(GetControlRotation().Yaw);
	}

	if (IsMyPlayer() == false)
		TickRemotePlayer(DeltaTime);
}

void AS1Player::TickRemotePlayer(float DeltaTime)
{
	const FVector CurLocation = GetActorLocation();
	const FVector DestLocation = ToVector(*DestInfo);

	if (FVector::Dist(CurLocation, DestLocation) > TELEPORT_THERESHOLD)
	{
		SetActorLocation(DestLocation, false);
		SetActorRotation(FRotator(0.f, DestInfo->yaw(), 0.f));
		GetCharacterMovement()->Velocity = FVector::ZeroVector;

		InterpStartLocation = DestLocation;
		InterpStartYaw = DestInfo->yaw();
		InterpElapsed = InterpDuration;
		
		return;
	}

	InterpElapsed = FMath::Min(InterpElapsed + DeltaTime, InterpDuration);
	const float Alpha = (InterpDuration > UE_KINDA_SMALL_NUMBER)
		? FMath::Clamp(InterpElapsed / InterpDuration, 0.1f, 1.f)
		: 1.f;

	const FVector NewLocation = FMath::Lerp(InterpStartLocation, DestLocation, Alpha);
	SetActorLocation(NewLocation, false);

	const float DeltaYaw = FMath::FindDeltaAngleDegrees(InterpStartYaw, DestInfo->yaw());
	SetActorRotation(FRotator(0.f, InterpStartYaw + DeltaYaw * Alpha, 0.f));

	const FVector FrameVelocity = (DeltaTime > UE_KINDA_SMALL_NUMBER)
		? (NewLocation - CurLocation) / DeltaTime
		: FVector::ZeroVector;

	GetCharacterMovement()->Velocity = FrameVelocity;

	if (FrameVelocity.SizeSquared() > 1.f)
		AddMovementInput(FrameVelocity.GetSafeNormal(), 1.f);
}

void AS1Player::ResetInterpolation(Protocol::MoveState State)
{
	InterpStartLocation = GetActorLocation();
	InterpStartYaw = GetActorRotation().Yaw;
	InterpElapsed = 0.f;

	InterpDuration = (State == Protocol::MOVE_STATE_IDLE)
		? IDLE_SNAP_DURATION
		: MOVE_PACKET_SEND_DELAY;
}

bool AS1Player::IsMyPlayer()
{
	return Cast<AS1MyPlayer>(this) != nullptr;
}

void AS1Player::SetPlayerInfo(const Protocol::PosInfo& Info)
{
	if (PlayerInfo->object_id() != 0)
	{
		assert(PlayerInfo->object_id() == Info.object_id());
	}

	// TODO
	PlayerInfo->CopyFrom(Info);

	FVector Location(Info.x(), Info.y(), Info.z());
	SetActorLocation(Location);

	DestInfo->CopyFrom(Info);
	ResetInterpolation(Info.state());
}

void AS1Player::SetDestInfo(const Protocol::PosInfo& Info)
{
	if (PlayerInfo->object_id() != 0)
	{
		assert(PlayerInfo->set_object_id() == Info.object_id());
	}

	ResetInterpolation(Info.state());

	// Dest에 최종 상태 복사
	DestInfo->CopyFrom(Info);

	// 상태만 바로 적용
	SetMoveState(Info.state());
}
