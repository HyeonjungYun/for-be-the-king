// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Game/S1Player.h"
#include "S1MyPlayer.generated.h"

class UInputMappingContext;

/**
 * 
 */
UCLASS()
class S1_API AS1MyPlayer : public AS1Player
{
	GENERATED_BODY()

public:

	/** Constructor */
	AS1MyPlayer();

protected:

	virtual void BeginPlay() override;
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;

	virtual void Tick(float DeltaTime) override;

protected:

	void Move(const FInputActionValue& Value);

	void Look(const FInputActionValue& Value);

	/**
	 * Turns the character toward the cursor every frame, independently of where it is
	 * moving. This separation is the whole point of the control scheme — you retreat with
	 * WASD while keeping your aim on whoever is chasing you.
	 * Formula 2 in design/gdd/movement-camera.md.
	 */
	void UpdateCursorFacing(float DeltaTime);

protected:

	/** 0 snaps instantly. Higher converges faster. movement-camera.md Tuning Knobs (safe range 0-40). */
	UPROPERTY(EditAnywhere, Category = "Movement|Facing")
	float FaceInterpSpeed = 20.f;

	/** A cursor nearer than this leaves facing alone — there is no direction at the origin. */
	UPROPERTY(EditAnywhere, Category = "Movement|Facing")
	float CursorDeadRadius = 25.f;

public:

	UFUNCTION(BlueprintCallable, Category = "Input")
	virtual void DoMove(float Right, float Forward);

	UFUNCTION(BlueprintCallable, Category = "Input")
	virtual void DoLook(float Yaw, float Pitch);

	UFUNCTION(BlueprintCallable, Category = "Input")
	virtual void DoJumpStart();

	UFUNCTION(BlueprintCallable, Category = "Input")
	virtual void DoJumpEnd();

public:

	FORCEINLINE class USpringArmComponent* GetCameraBoom() const { return CameraBoom; }

	FORCEINLINE class UCameraComponent* GetFollowCamera() const { return FollowCamera; }

protected:

	/** Input mapping contexts applied to the local player on possession */
	UPROPERTY(EditAnywhere, Category = "Input")
	TArray<UInputMappingContext*> DefaultMappingContexts;

	UPROPERTY(EditAnywhere, Category = "Input")
	UInputAction* JumpAction;

	UPROPERTY(EditAnywhere, Category = "Input")
	UInputAction* MoveAction;

	UPROPERTY(EditAnywhere, Category = "Input")
	UInputAction* LookAction;

	UPROPERTY(EditAnywhere, Category = "Input")
	UInputAction* MouseLookAction;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components", meta = (AllowPrivateAccess = "true"))
	USpringArmComponent* CameraBoom;

	/** Follow camera */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components", meta = (AllowPrivateAccess = "true"))
	UCameraComponent* FollowCamera;

protected:
	float MovePacketSendTimer = MOVE_PACKET_SEND_DELAY;
};
