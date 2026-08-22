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
	 * RMB acquires whatever is under the cursor and asks the server to attack it.
	 * The client never decides whether the hit lands — it only names a target.
	 * See design/gdd/combat-system.md Rule 1 (target lock) and Rule 10 (server authority).
	 */
	void Attack();

	/** object_id under the cursor, or 0 when there is no valid target. */
	uint64 AcquireTargetUnderCursor();

	/**
	 * Draws the locally predicted windup as a closing arc at the character's feet.
	 * Rule 10 allows exactly one client-side prediction — our own windup animation —
	 * because waiting a round trip for *any* feedback makes the attack feel broken.
	 * The result (damage, crit, whether it landed at all) still waits for the server.
	 */
	void DrawWindupDebug();

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

	/** Right mouse button. Leave unset and attacking is simply disabled — nothing else breaks. */
	UPROPERTY(EditAnywhere, Category = "Input")
	UInputAction* AttackAction;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components", meta = (AllowPrivateAccess = "true"))
	USpringArmComponent* CameraBoom;

	/** Follow camera */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components", meta = (AllowPrivateAccess = "true"))
	UCameraComponent* FollowCamera;

protected:
	float MovePacketSendTimer = MOVE_PACKET_SEND_DELAY;

	/** World-seconds deadline for the predicted windup. 0 means we are not attacking. */
	float LocalWindupEndsAt = 0.f;

	/**
	 * Gated on the windup only, not the full interval. The server does not consume cooldown
	 * when an attack is cancelled, so blocking for the recovery too would make the client
	 * stricter than the server — you would be locked out after a miss that the server
	 * already forgave.
	 */
	float LocalReadyAt = 0.f;

	/**
	 * Mirrors the server's Formula C3 (windup_ratio 0.3 x attack_interval 1000ms).
	 * 🔴 Duplicated constant. When the server value changes this must change with it —
	 * P2 should send attack speed to the client instead of restating the formula here.
	 */
	static constexpr float PREDICTED_WINDUP_SECONDS = 0.3f;
	static constexpr float WINDUP_ARC_RADIUS = 70.f;
	static constexpr int32 WINDUP_ARC_SEGMENTS = 32;
};
