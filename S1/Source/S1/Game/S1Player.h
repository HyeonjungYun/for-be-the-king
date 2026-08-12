// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "InputActionValue.h"
#include "Protocol.pb.h"
#include "S1Player.generated.h"

class USpringArmComponent;
class UCameraComponent;
class UInputAction;
struct FInputActionValue;

UCLASS()
class S1_API AS1Player : public ACharacter
{
	GENERATED_BODY()

public:

	AS1Player();
	virtual ~AS1Player();

	Protocol::MoveState GetMoveState() { return PlayerInfo->state(); }
	void SetMoveState(Protocol::MoveState State);

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

	void TickRemotePlayer(float DeltaTime);
	void ResetInterpolation(Protocol::MoveState State);

public:
	bool IsMyPlayer();

public:
	void SetPlayerInfo(const Protocol::PosInfo& Info);
	void SetDestInfo(const Protocol::PosInfo& Info);
	Protocol::PosInfo* GetPlayerInfo() { return PlayerInfo; };

protected:
	class Protocol::PosInfo* PlayerInfo; // ���� ��ġ
	class Protocol::PosInfo* DestInfo; // ����ġ

	// 30Hz. movement-camera.md Core Rule 10, entities.yaml move_packet_send_rate.
	// The server's validation margin (1.15) and min_delta_t (16.6ms = half of this)
	// are both derived from a 33ms interval — they mean nothing at the old 5Hz.
	// InterpDuration for remote proxies also reads this value, so both move together.
	const float MOVE_PACKET_SEND_DELAY = 1.f / 30.f;
	const float IDLE_SNAP_DURATION = 0.1f;
	const float TELEPORT_THERESHOLD = 250.0f;

	FVector InterpStartLocation = FVector::ZeroVector;
	float InterpStartYaw = 0.f;
	float InterpElapsed = 0.f;
	float InterpDuration = 0.f;

	// Cache
	FVector2D DesiredInput;
	FVector DesiredMoveDirection;
	float DesiredYaw;

	// Dirty Flag Test
	FVector2D LastDesiredInput;
};

inline FVector ToVector(const Protocol::PosInfo& Info)
{
	return FVector(Info.x(), Info.y(), Info.z());
}