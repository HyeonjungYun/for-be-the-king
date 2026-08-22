// Fill out your copyright notice in the Description page of Project Settings.


#include "Game/S1MyPlayer.h"
#include "Engine/LocalPlayer.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/PlayerController.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "S1.h"
#include "Kismet/KismetMathLibrary.h"
#include "DrawDebugHelpers.h"

AS1MyPlayer::AS1MyPlayer()
{
	// Fixed top-down camera. See design/gdd/movement-camera.md Core Rule 8.
	// TargetArmLength 2600 + FOV 60 gives a ~30m wide view, matching the density
	// budget in game-concept.md (0.22 players visible on average in the open field).
	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = 2600.0f;

	// The camera never rotates. Zooming or rotating would let players buy information,
	// which breaks Pillar 4 ("information is the most expensive resource").
	CameraBoom->SetUsingAbsoluteRotation(true);
	CameraBoom->SetRelativeRotation(FRotator(-60.0f, 0.0f, 0.0f));
	CameraBoom->bUsePawnControlRotation = false;
	CameraBoom->bInheritPitch = false;
	CameraBoom->bInheritYaw = false;
	CameraBoom->bInheritRoll = false;

	// Defaults to true. Leaving it on makes the boom pull in near walls and rooftops,
	// which yanks the visible area around. Level geometry is single-storey by design
	// (game-concept.md, the kingdom's buildings broke apart as they fell), so nothing
	// should ever sit between the camera and the player.
	CameraBoom->bDoCollisionTest = false;

	// Formula 4 — absorbs server snapback and stops the camera feeling robotic.
	// 170cm is 0.5s of travel at 340cm/s; the old 300cm was tuned for 600cm/s.
	CameraBoom->bEnableCameraLag = true;
	CameraBoom->CameraLagSpeed = 10.0f;
	CameraBoom->CameraLagMaxDistance = 170.0f;

	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;
	FollowCamera->FieldOfView = 60.0f;

	// Note: The skeletal mesh and anim blueprint references on the Mesh component (inherited from Character)
	// are set in the derived blueprint asset named ThirdPersonCharacter (to avoid direct content references in C++)
}

void AS1MyPlayer::BeginPlay()
{
	Super::BeginPlay();

	// Add the mapping contexts so the Enhanced Input actions bound below can trigger
	if (APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
			ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
		{
			for (UInputMappingContext* CurrentContext : DefaultMappingContexts)
			{
				Subsystem->AddMappingContext(CurrentContext, 0);
			}
		}

		// Aiming is done with the cursor, so it has to be visible and free to move around
		// the viewport. The default game input mode captures and hides it, which would
		// leave UpdateCursorFacing() reading a position the player cannot see.
		PC->bShowMouseCursor = true;
		PC->SetInputMode(FInputModeGameAndUI()
			.SetLockMouseToViewportBehavior(EMouseLockMode::LockAlways)
			.SetHideCursorDuringCapture(false));
	}

	// TODO: cursor and input mode belong on a player controller, not the pawn. Move this
	// when AS1PlayerController exists — the project has no C++ controller class yet.
}

void AS1MyPlayer::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	// Set up action bindings
	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent)) {

		// Jump is unbound — the game is ground-only (movement-camera.md Core Rule 5).
		// The DoJumpStart/DoJumpEnd wrappers below are left in place because Blueprints
		// may still reference them; removing them would break BP compilation.
		// EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Started, this, &ACharacter::Jump);
		// EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Completed, this, &ACharacter::StopJumping);

		// Moving
		EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this, &AS1MyPlayer::Move);
		EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Completed, this, &AS1MyPlayer::Move);

		// Look is unbound. It feeds AddControllerYawInput, and nothing reads control
		// rotation any more — the camera holds an absolute rotation and DoMove uses world
		// axes. Leaving it bound would silently drift the control rotation and confuse
		// anyone who later tries to read it.
		// Aim is handled by UpdateCursorFacing() instead.
		// EnhancedInputComponent->BindAction(MouseLookAction, ETriggerEvent::Triggered, this, &AS1MyPlayer::Look);
		// EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &AS1MyPlayer::Look);

		// Attacking. Binding a null action logs an error every launch, so guard it —
		// the IA asset may not exist yet and an unbound attack should not be noisy.
		if (AttackAction)
			EnhancedInputComponent->BindAction(AttackAction, ETriggerEvent::Started, this, &AS1MyPlayer::Attack);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("'%s' Failed to find an Enhanced Input component! This template is built to use the Enhanced Input system. If you intend to use the legacy system, then you will need to update this C++ file."), *GetNameSafe(this));
	}
}

void AS1MyPlayer::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	UpdateCursorFacing(DeltaTime);
	DrawWindupDebug();

	// Send ����
	bool ForceSendPacket = false;

	if (LastDesiredInput != DesiredInput)
	{
		ForceSendPacket = true;
		LastDesiredInput = DesiredInput;
	}

	// State ����
	if (DesiredInput == FVector2D::Zero())
		SetMoveState(Protocol::MOVE_STATE_IDLE);
	else
		SetMoveState(Protocol::MOVE_STATE_RUN);

	MovePacketSendTimer -= DeltaTime;

	if (MovePacketSendTimer <= 0 || ForceSendPacket)
	{
		MovePacketSendTimer = MOVE_PACKET_SEND_DELAY;

		Protocol::C_MOVE MovePkt;

		// ���� ��ġ ����
		{
			Protocol::PosInfo* Info = MovePkt.mutable_info();
			Info->CopyFrom(*PlayerInfo);
			Info->set_yaw(DesiredYaw);
			Info->set_state(GetMoveState());
		}

		SEND_PACKET(MovePkt);
	}
}

void AS1MyPlayer::Move(const FInputActionValue& Value)
{
	// Hard CC drops the input entirely rather than letting it through and relying on the
	// server to snap us back — a rejection round trip would show a visible lurch.
	if (CanMove() == false)
	{
		DoMove(0.f, 0.f);
		return;
	}

	// input is a Vector2D
	FVector2D MovementVector = Value.Get<FVector2D>();

	// route the input
	DoMove(MovementVector.X, MovementVector.Y);
}

void AS1MyPlayer::Look(const FInputActionValue& Value)
{
	// input is a Vector2D
	FVector2D LookAxisVector = Value.Get<FVector2D>();

	// route the input
	DoLook(LookAxisVector.X, LookAxisVector.Y);
}

void AS1MyPlayer::UpdateCursorFacing(float DeltaTime)
{
	// Stun, knockback and launch freeze facing. Root deliberately does not — being pinned
	// while still able to aim is the whole difference between the two (Rule 5 table).
	// Facing is never validated by the server, so this check is the only thing enforcing it.
	if (CanTurn() == false)
		return;

	APlayerController* PC = Cast<APlayerController>(GetController());
	if (PC == nullptr)
		return;

	FVector RayOrigin, RayDirection;
	if (PC->DeprojectMousePositionToWorld(RayOrigin, RayDirection) == false)
		return;

	// Intersect the cursor ray with the plane the character stands on, solved directly
	// rather than with GetHitResultUnderCursor. A trace returns nothing when the cursor
	// sits over a hole or past the edge of the level, and facing would freeze exactly
	// when the player is most likely to be looking somewhere dangerous.
	if (FMath::Abs(RayDirection.Z) < UE_KINDA_SMALL_NUMBER)
		return;

	const FVector Location = GetActorLocation();
	const double Distance = (Location.Z - RayOrigin.Z) / RayDirection.Z;
	if (Distance <= 0.0)
		return;

	FVector ToCursor = (RayOrigin + RayDirection * Distance) - Location;
	ToCursor.Z = 0.0;

	// Directly on top of the character there is no meaningful direction, and sub-pixel
	// mouse jitter would spin the character. Hold the previous yaw.
	if (ToCursor.SizeSquared() < FMath::Square(CursorDeadRadius))
		return;

	const float TargetYaw = static_cast<float>(ToCursor.Rotation().Yaw);

	float NewYaw = TargetYaw;
	if (FaceInterpSpeed > 0.f)
	{
		const FRotator Current(0.0, GetActorRotation().Yaw, 0.0);
		const FRotator Target(0.0, TargetYaw, 0.0);
		NewYaw = static_cast<float>(FMath::RInterpTo(Current, Target, DeltaTime, FaceInterpSpeed).Yaw);
	}

	SetActorRotation(FRotator(0.0, NewYaw, 0.0));

	// The server receives this, so what the player aims at and what other clients see
	// stay the same thing.
	DesiredYaw = NewYaw;
}

void AS1MyPlayer::DoMove(float Right, float Forward)
{
	if (GetController() != nullptr)
	{
		// World axes, not control rotation. The camera is bolted to a fixed yaw
		// (movement-camera.md Core Rule 8), so screen-up is always world +X and W always
		// means the same direction no matter where the character is facing.
		// Reading control rotation here is what would tie movement to aim and kill strafing.
		const FVector ForwardDirection = FVector::ForwardVector;
		const FVector RightDirection = FVector::RightVector;

		AddMovementInput(ForwardDirection, Forward);
		AddMovementInput(RightDirection, Right);

		// Cache
		{
			FVector2D MovementVector = FVector2D(Right, Forward);
			DesiredInput = MovementVector;

			DesiredMoveDirection = ForwardDirection * MovementVector.Y + RightDirection * MovementVector.X;
			DesiredMoveDirection.Normalize();

			// DesiredYaw is deliberately not touched here. Facing comes from the cursor,
			// never from the movement direction — see UpdateCursorFacing().
		}
	}
}

void AS1MyPlayer::DoLook(float Yaw, float Pitch)
{
	if (GetController() != nullptr)
	{
		// add yaw and pitch input to controller
		AddControllerYawInput(Yaw);
		AddControllerPitchInput(Pitch);
	}
}

void AS1MyPlayer::DoJumpStart()
{
	// signal the character to jump
	Jump();
}

void AS1MyPlayer::DoJumpEnd()
{
	// signal the character to stop jumping
	StopJumping();
}

uint64 AS1MyPlayer::AcquireTargetUnderCursor()
{
	APlayerController* PC = Cast<APlayerController>(GetController());
	if (PC == nullptr)
		return 0;

	// TODO: Rule 1 allows a 30px leniency radius (target_acquisition_leniency). An exact
	// cursor hit is unforgiving on small top-down silhouettes — revisit with a sphere
	// trace once the camera height is locked.
	FHitResult Hit;
	if (PC->GetHitResultUnderCursor(ECC_Pawn, false, Hit) == false)
		return 0;

	AS1Player* Target = Cast<AS1Player>(Hit.GetActor());
	if (Target == nullptr || Target->IsMyPlayer())
		return 0;

	return Target->GetPlayerInfo()->object_id();
}

void AS1MyPlayer::Attack()
{
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;

	// The server ignores requests that arrive mid-windup, so sending them is pure noise.
	if (Now < LocalReadyAt)
		return;

	const uint64 TargetId = AcquireTargetUnderCursor();
	if (TargetId == 0)
		return;

	// Range, cooldown, windup and damage are all decided server-side (Rule 10). The only
	// thing this packet carries is which object we want to hit — not even our own id,
	// which the server derives from the session so it cannot be forged.
	Protocol::C_ATTACK AttackPkt;
	AttackPkt.set_target_id(TargetId);

	SEND_PACKET(AttackPkt);

	// Predict the windup visual only. If the server rejects the attack — out of range, or
	// we were stunned in the meantime — the arc still completes and no damage number
	// follows. That mismatch is acceptable; predicting the *result* would not be.
	LocalWindupEndsAt = Now + PREDICTED_WINDUP_SECONDS;
	LocalReadyAt = LocalWindupEndsAt;
}

void AS1MyPlayer::DrawWindupDebug()
{
	UWorld* World = GetWorld();
	if (World == nullptr)
		return;

	const float Now = World->GetTimeSeconds();
	if (Now >= LocalWindupEndsAt)
		return;

	const float Elapsed = PREDICTED_WINDUP_SECONDS - (LocalWindupEndsAt - Now);
	const float Alpha = FMath::Clamp(Elapsed / PREDICTED_WINDUP_SECONDS, 0.f, 1.f);

	// An arc at the feet rather than a bar overhead — from a fixed top-down camera the
	// ground plane is the one place nothing else competes for, and it never covers the
	// character we are trying to aim with.
	const FVector Center = GetActorLocation() - FVector(0.f, 0.f, 90.f);
	const int32 Filled = FMath::RoundToInt(WINDUP_ARC_SEGMENTS * Alpha);

	for (int32 i = 0; i < WINDUP_ARC_SEGMENTS; i++)
	{
		const float A0 = (2.f * UE_PI) * i / WINDUP_ARC_SEGMENTS;
		const float A1 = (2.f * UE_PI) * (i + 1) / WINDUP_ARC_SEGMENTS;

		const FVector P0 = Center + FVector(FMath::Cos(A0), FMath::Sin(A0), 0.f) * WINDUP_ARC_RADIUS;
		const FVector P1 = Center + FVector(FMath::Cos(A1), FMath::Sin(A1), 0.f) * WINDUP_ARC_RADIUS;

		DrawDebugLine(World, P0, P1, (i < Filled) ? FColor::Yellow : FColor(60, 60, 60),
			false, -1.f, 0, 4.f);
	}
}
