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
#include "DrawDebugHelpers.h"
#include "Components/TextRenderComponent.h"

AS1Player::AS1Player()
{
	// Set size for collision capsule
	GetCapsuleComponent()->InitCapsuleSize(42.f, 96.0f);

	// Don't rotate when the controller rotates. Let that just affect the camera.
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;

	// Facing follows the cursor, not the movement direction. This is what makes strafing
	// work — you back away with WASD while still aiming at whoever is chasing you.
	// See design/gdd/movement-camera.md Core Rule 3.
	GetCharacterMovement()->bOrientRotationToMovement = false;
	GetCharacterMovement()->RotationRate = FRotator(0.0f, 500.0f, 0.0f);

	// 340 cm/s = Ryze's base move speed in League. Floor size (280m), traversal time (82s)
	// and the whole session budget are derived from this number — see game-concept.md § 공간.
	GetCharacterMovement()->MaxWalkSpeed = 340.f;
	GetCharacterMovement()->MinAnalogWalkSpeed = 20.f;
	GetCharacterMovement()->BrakingDecelerationWalking = 2000.f;

	// Ground only — no jump (MVP decision, 2026-08-11). Jumping reads poorly from a
	// top-down camera and would force the server's move validation to handle the Z axis.
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

	// Before the PlayerInfo sync below, so the dash displacement rides out on this frame's
	// C_MOVE instead of lagging a frame behind the visible position.
	TickDash(DeltaTime);

	TickCc();
	DrawCombatDebug();
	TickDamagePopups();

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

void AS1Player::TickCc()
{
	// Remote proxies are moved by hand in TickRemotePlayer (MOVE_None), so walk speed means
	// nothing to them and StopMovementImmediately would fight the interpolation. Their CC is
	// visible through the positions the server sends, which are already slowed.
	if (IsMyPlayer() == false)
		return;

	UCharacterMovementComponent* Movement = GetCharacterMovement();
	if (Movement == nullptr)
		return;

	// Formula C5 — the server aggregates slow sources and sends the max; we only apply it.
	Movement->MaxWalkSpeed = BaseWalkSpeed * (1.f - ActiveSlow);

	if (CanMove() == false)
		Movement->StopMovementImmediately();
}

void AS1Player::OnDamaged(int32 Damage, int32 RemainingHp, bool bIsCrit)
{
	CurrentHp = RemainingHp;

	UWorld* World = GetWorld();
	if (World == nullptr)
		return;

	// Reuse a finished slot; otherwise grow up to the cap; otherwise steal the oldest.
	int32 SlotIdx = INDEX_NONE;

	for (int32 i = 0; i < DamagePopups.Num(); i++)
	{
		if (DamagePopups[i].bActive == false)
		{
			SlotIdx = i;
			break;
		}
	}

	if (SlotIdx == INDEX_NONE)
	{
		if (DamagePopups.Num() < DAMAGE_POPUP_MAX)
		{
			SlotIdx = DamagePopups.AddDefaulted();
		}
		else
		{
			float Oldest = TNumericLimits<float>::Max();
			for (int32 i = 0; i < DamagePopups.Num(); i++)
			{
				if (DamagePopups[i].SpawnedAt < Oldest)
				{
					Oldest = DamagePopups[i].SpawnedAt;
					SlotIdx = i;
				}
			}
		}
	}

	FDamagePopup& Popup = DamagePopups[SlotIdx];

	if (Popup.Text == nullptr)
	{
		// Created on demand and registered by hand — components made after BeginPlay do not
		// exist to the engine until RegisterComponent runs.
		Popup.Text = NewObject<UTextRenderComponent>(this);
		Popup.Text->SetupAttachment(RootComponent);
		Popup.Text->SetHorizontalAlignment(EHTA_Center);
		Popup.Text->SetVerticalAlignment(EVRTA_TextCenter);
		Popup.Text->RegisterComponent();
	}

	Popup.Text->SetText(FText::AsNumber(Damage));
	Popup.Text->SetTextRenderColor(bIsCrit ? FColor::Yellow : FColor::White);
	Popup.Text->SetWorldSize(DAMAGE_POPUP_TEXT_SIZE * (bIsCrit ? 1.6f : 1.f));
	Popup.Text->SetVisibility(true);

	// Fan consecutive hits sideways so two numbers arriving together do not land on top of
	// each other. Centred on the character: -1.5, -0.5, +0.5, +1.5 spreads.
	Popup.ScreenOffsetX = ((DamagePopupCount++ % 4) - 1.5f) * DAMAGE_POPUP_SPREAD;
	Popup.SpawnedAt = World->GetTimeSeconds();
	Popup.bActive = true;
}

void AS1Player::TickDamagePopups()
{
	UWorld* World = GetWorld();
	if (World == nullptr || DamagePopups.Num() == 0)
		return;

	APlayerController* PC = World->GetFirstPlayerController();
	if (PC == nullptr)
		return;

	FVector CamLoc;
	FRotator CamRot;
	PC->GetPlayerViewPoint(CamLoc, CamRot);

	const FMatrix CamMatrix = FRotationMatrix(CamRot);
	const FVector CamForward = CamMatrix.GetUnitAxis(EAxis::X);
	const FVector ScreenRight = CamMatrix.GetUnitAxis(EAxis::Y);
	const FVector ScreenUp = CamMatrix.GetUnitAxis(EAxis::Z);

	// TextRenderComponent draws in its local YZ plane facing local +X (engine source:
	// TangentZ is (1,0,0)). Point +X back at the camera and keep +Z on screen-up, and the
	// number reads flat and upright no matter what the camera does.
	const FRotator FaceCamera = FRotationMatrix::MakeFromXZ(-CamForward, ScreenUp).Rotator();

	const float Now = World->GetTimeSeconds();
	const FVector ActorLocation = GetActorLocation();

	for (FDamagePopup& Popup : DamagePopups)
	{
		if (Popup.bActive == false || Popup.Text == nullptr)
			continue;

		const float Age = Now - Popup.SpawnedAt;

		if (Age >= DAMAGE_POPUP_SECONDS)
		{
			Popup.bActive = false;
			Popup.Text->SetVisibility(false);
			continue;
		}

		const float Alpha = Age / DAMAGE_POPUP_SECONDS;

		// Anchored to the actor so the number travels with a target that keeps running,
		// and offset along screen axes so the rise and the spread stay independent.
		const FVector Location = ActorLocation
			+ ScreenRight * Popup.ScreenOffsetX
			+ ScreenUp * (DAMAGE_POPUP_BASE_HEIGHT + DAMAGE_POPUP_RISE * Alpha);

		Popup.Text->SetWorldLocation(Location);
		Popup.Text->SetWorldRotation(FaceCamera);
	}
}

void AS1Player::DrawCombatDebug()
{
	if (bShowCombatDebug == false)
		return;

	UWorld* World = GetWorld();
	if (World == nullptr)
		return;

	// Laid flat on the ground plane beside the character. From a fixed top-down camera that
	// reads as a bar above them, and it never overlaps the mesh the way a floating quad would.
	const FVector Base = GetActorLocation() + FVector(DEBUG_BAR_OFFSET_X, 0.f, 0.f);
	const FVector Half = FVector(0.f, HEALTH_BAR_WIDTH * 0.5f, 0.f);

	// bPersistentLines=false with LifeTime=-1 draws for exactly one frame, so the bar tracks
	// the actor instead of leaving a trail behind it.
	DrawDebugLine(World, Base - Half, Base + Half, FColor(40, 40, 40), false, -1.f, 0, 6.f);

	const float Ratio = (MaxHp > 0)
		? FMath::Clamp(CurrentHp / static_cast<float>(MaxHp), 0.f, 1.f)
		: 0.f;

	if (Ratio > 0.f)
	{
		DrawDebugLine(World, Base - Half, (Base - Half) + Half * 2.f * Ratio,
			FColor::Green, false, -1.f, 0, 6.f);
	}

	// One short tick per active CC, left to right. Colours are arbitrary debug picks — the
	// art bible's palette rules do not apply to something that never ships.
	const float Now = World->GetTimeSeconds();
	int32 Slot = 0;

	auto Mark = [&](bool bActive, FColor Color)
	{
		if (bActive == false)
			return;

		const FVector Start = (Base - Half) + FVector(-16.f, Slot++ * 14.f, 0.f);
		DrawDebugLine(World, Start, Start + FVector(0.f, 10.f, 0.f), Color, false, -1.f, 0, 8.f);
	};

	Mark(Now < StunUntil, FColor::Red);
	Mark(Now < RootUntil, FColor::Orange);
	Mark(Now < KnockbackUntil, FColor::Magenta);
	Mark(Now < LaunchUntil, FColor::Purple);
	Mark(ActiveSlow > 0.f, FColor::Cyan);
}

void AS1Player::ApplyCcEvent(const Protocol::CcEventInfo& Info, bool bApplied)
{
	// Expiry sets the deadline to now; the server is the only thing that extends it.
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	const float Until = bApplied ? (Now + Info.duration_ms() / 1000.f) : Now;

	switch (Info.cc_type())
	{
	case Protocol::CC_TYPE_STUN:		StunUntil = Until;		break;
	case Protocol::CC_TYPE_ROOT:		RootUntil = Until;		break;
	case Protocol::CC_TYPE_KNOCKBACK:	KnockbackUntil = Until;	break;
	case Protocol::CC_TYPE_LAUNCH:		LaunchUntil = Until;	break;

	case Protocol::CC_TYPE_SLOW:
		// Individual slow sources expire independently on the server, which recomputes the
		// aggregate. An expiry event here means every source is gone.
		if (bApplied == false)
			ActiveSlow = 0.f;
		else if (Info.magnitude() > ActiveSlow)
			ActiveSlow = Info.magnitude();
		break;

	default:
		// Silence and heal reduction have no movement effect — they are gameplay-only and
		// will be read by the skill system (P1.5) and healing (P2).
		break;
	}
}

void AS1Player::ApplyCcState(const Protocol::CcStateInfo& State)
{
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;

	// A snapshot is the whole truth, so start from cleared deadlines. Anything the server
	// did not list has already expired — merging instead of overwriting would let a single
	// missed expiry event pin the character in a CC forever.
	StunUntil = 0.f;
	RootUntil = 0.f;
	KnockbackUntil = 0.f;
	LaunchUntil = 0.f;

	for (const Protocol::CcSlot& Slot : State.slots())
	{
		const float Until = Now + Slot.remaining_ms() / 1000.f;

		switch (Slot.cc_type())
		{
		case Protocol::CC_TYPE_STUN:		StunUntil = Until;		break;
		case Protocol::CC_TYPE_ROOT:		RootUntil = Until;		break;
		case Protocol::CC_TYPE_KNOCKBACK:	KnockbackUntil = Until;	break;
		case Protocol::CC_TYPE_LAUNCH:		LaunchUntil = Until;	break;

		default:
			// Slow arrives as the aggregate below; silence and heal reduction have no
			// movement effect and are read by the skill and healing systems later.
			break;
		}
	}

	// Formula C5 — the server already took the max across sources. Taking it wholesale is
	// exactly what fixes partial expiry, which the event path cannot express.
	SetActiveSlow(State.active_slow());
}

void AS1Player::SetActiveSlow(float InActiveSlow)
{
	ActiveSlow = FMath::Clamp(InActiveSlow, 0.f, 1.f);
}

bool AS1Player::CanMove() const
{
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;

	return Now >= StunUntil && Now >= RootUntil && Now >= KnockbackUntil && Now >= LaunchUntil;
}

bool AS1Player::CanTurn() const
{
	// Root is the exception — it pins you but lets you keep aiming. That distinction is the
	// whole reason root and stun are separate CC types (combat-system.md Rule 5 table).
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;

	return Now >= StunUntil && Now >= KnockbackUntil && Now >= LaunchUntil;
}

void AS1Player::RequestDash(const FVector& Direction, float DistCm, float SpeedCms)
{
	if (DistCm <= 0.f || SpeedCms <= 0.f)
		return;

	FVector Flat = Direction;
	Flat.Z = 0.0;

	if (Flat.Normalize() == false)
		return;

	DashDirection = Flat;
	DashRemainingCm = DistCm;
	DashSpeedCms = SpeedCms;
}

void AS1Player::TickDash(float DeltaTime)
{
	if (DashRemainingCm <= 0.f)
		return;

	const float Step = FMath::Min(DashSpeedCms * DeltaTime, DashRemainingCm);

	// Swept so the capsule is stopped by geometry rather than tunnelling through it. At
	// 1200 cm/s a 33ms frame covers 40cm, which is comfortably under the capsule radius —
	// a non-swept move would put us inside a wall.
	FHitResult Hit;
	AddActorWorldOffset(DashDirection * Step, true, &Hit);

	if (Hit.bBlockingHit)
	{
		// Ends the moment it hits something (movement-camera.md § Dashing exit conditions).
		// The server is not told: it validated a speed window, not a distance, so stopping
		// short is always within what it already allows.
		DashRemainingCm = 0.f;
		return;
	}

	DashRemainingCm -= Step;
}

bool AS1Player::CanAttack() const
{
	// Deliberately identical to CanTurn: if you can still aim, you can still swing.
	// Kept as its own function because the two answer different questions and the server
	// splits them the same way — combat-system.md Rule 5 is free to move one without
	// dragging the other along.
	return CanTurn();
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
		ensureMsgf(PlayerInfo->object_id() == Info.object_id(),
			TEXT("SetPlayerInfo: object_id mismatch (have %llu, got %llu)"),
			PlayerInfo->object_id(), Info.object_id());
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
	// Was assert(PlayerInfo->set_object_id() == ...) — a setter called with no argument.
	// It never showed up because <cassert>'s assert compiles to nothing under NDEBUG,
	// so the broken expression was never parsed outside a Debug build.
	// ensureMsgf runs in Development too, and logs instead of halting.
	if (PlayerInfo->object_id() != 0)
	{
		ensureMsgf(PlayerInfo->object_id() == Info.object_id(),
			TEXT("SetDestInfo: object_id mismatch (have %llu, got %llu)"),
			PlayerInfo->object_id(), Info.object_id());
	}

	ResetInterpolation(Info.state());

	// Dest에 최종 상태 복사
	DestInfo->CopyFrom(Info);

	// 상태만 바로 적용
	SetMoveState(Info.state());
}
