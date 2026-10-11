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
#include "S1GameInstance.h"
#include "Game/S1SkillTable.h"
#include "Game/UI/S1ItemUIComponent.h"
#include "Game/UI/S1TestModeComponent.h"
#include "Game/UI/S1RadarWidget.h"
#include "Game/Escape/S1EscapeSubsystem.h"
#include "Game/Item/S1ItemSubsystem.h"
#include "Game/Radar/S1RadarLogic.h"
#include "Kismet/KismetMathLibrary.h"
#include "DrawDebugHelpers.h"

namespace
{
	/**
	 * Slot-to-key mapping is fixed, not configurable.
	 * design/gdd/equipment-skill-binding.md B1.
	 */
	const TCHAR* SlotKeyName(Protocol::EquipSlot Slot)
	{
		switch (Slot)
		{
		case Protocol::SLOT_WEAPON_PRIMARY:   return TEXT("Shift");
		case Protocol::SLOT_WEAPON_SECONDARY: return TEXT("Q");
		case Protocol::SLOT_HELMET:           return TEXT("E");
		case Protocol::SLOT_ARMOR:            return TEXT("R");
		case Protocol::SLOT_BOOTS:            return TEXT("Z");
		case Protocol::SLOT_TRINKET:          return TEXT("X");
		default:                              return TEXT("?");
		}
	}
}

AS1MyPlayer::AS1MyPlayer()
{
	// Fixed top-down camera. See design/gdd/movement-camera.md Core Rule 8.
	// TargetArmLength 2600 + FOV 60 gives a ~30m wide view, matching the density
	// budget in game-concept.md (0.22 players visible on average in the open field).
	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = DefaultArmLength;

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

	ItemUI = CreateDefaultSubobject<US1ItemUIComponent>(TEXT("ItemUI"));
	TestMode = CreateDefaultSubobject<US1TestModeComponent>(TEXT("TestMode"));

	// Note: The skeletal mesh and anim blueprint references on the Mesh component (inherited from Character)
	// are set in the derived blueprint asset named ThirdPersonCharacter (to avoid direct content references in C++)
}

void AS1MyPlayer::BeginPlay()
{
	Super::BeginPlay();

	SetupLocalController();
}

void AS1MyPlayer::NotifyControllerChanged()
{
	Super::NotifyControllerChanged();

	// The level's own pawn is possessed before BeginPlay, which does this itself.
	if (HasActorBegunPlay() == false)
		return;

	SetupLocalController();

	if (ItemUI != nullptr)
		ItemUI->InitForLocalController();
	if (TestMode != nullptr)
		TestMode->InitForLocalController();
}

void AS1MyPlayer::SetupLocalController()
{
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
		if (AimAttackAction)
			EnhancedInputComponent->BindAction(AimAttackAction, ETriggerEvent::Started, this, &AS1MyPlayer::OnAimAttackKey);

		// One handler for all six slots, with the slot number carried as a bound payload.
		// Six near-identical methods would be the alternative and they would only differ
		// by a constant.
		int32 BoundSkillCount = 0;
		for (const FSkillInputBinding& Binding : SkillActions)
		{
			if (Binding.Action == nullptr)
				continue;

			EnhancedInputComponent->BindAction(Binding.Action, ETriggerEvent::Started,
				this, &AS1MyPlayer::OnSkillKey, Binding.EquipSlot);
			BoundSkillCount++;
		}

		// Both mouse buttons need press AND release: LMB because holding it is what keeps a
		// cast alive, RMB because the cancel latch has to clear when the finger comes up.
		if (ConfirmAction)
		{
			EnhancedInputComponent->BindAction(ConfirmAction, ETriggerEvent::Started, this, &AS1MyPlayer::OnConfirmPressed);
			EnhancedInputComponent->BindAction(ConfirmAction, ETriggerEvent::Completed, this, &AS1MyPlayer::OnConfirmReleased);
		}

		if (CancelAction)
		{
			EnhancedInputComponent->BindAction(CancelAction, ETriggerEvent::Started, this, &AS1MyPlayer::OnCancelPressed);
			EnhancedInputComponent->BindAction(CancelAction, ETriggerEvent::Completed, this, &AS1MyPlayer::OnCancelReleased);
		}

		// Kept: a missing action here is silent otherwise, and that cost an entire debug
		// session once already.
		UE_LOG(LogTemp, Warning, TEXT("[SKILL INPUT] confirm=%s cancel=%s aimAttack=%s"),
			ConfirmAction ? *ConfirmAction->GetName() : TEXT("NULL"),
			CancelAction ? *CancelAction->GetName() : TEXT("NULL"),
			AimAttackAction ? *AimAttackAction->GetName() : TEXT("NULL"));

		UE_LOG(LogTemp, Warning, TEXT("[SKILL INPUT] bound %d of %d entries"),
			BoundSkillCount, SkillActions.Num());

		// Tab / Esc for the item panels.
		if (ItemUI != nullptr)
			ItemUI->SetupInput(EnhancedInputComponent);

		// F7 / F6 for the test mode panel.
		if (TestMode != nullptr)
			TestMode->SetupInput(EnhancedInputComponent);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("'%s' Failed to find an Enhanced Input component! This template is built to use the Enhanced Input system. If you intend to use the legacy system, then you will need to update this C++ file."), *GetNameSafe(this));
	}
}

const UInputAction* AS1MyPlayer::GetSkillAction(int32 EquipSlot) const
{
	for (const FSkillInputBinding& Binding : SkillActions)
	{
		if (Binding.EquipSlot == EquipSlot)
			return Binding.Action;
	}
	return nullptr;
}

void AS1MyPlayer::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	UpdateCursorFacing(DeltaTime);
	DrawWindupDebug();
	DrawSkillBarDebug();
	DrawAimIndicator();
	DrawCastGauge();
	TickSustainedAttack();

	// The client never learns that a cast completed — the server just judges and sends the
	// result. Clearing the slot on the predicted deadline stops a pointless C_SKILL_CANCEL
	// from going out when the button comes up after the cast already landed.
	if (CastingSlot != 0 && GetWorld() != nullptr && GetWorld()->GetTimeSeconds() >= LocalCastEndsAt)
	{
		// The cast ran to completion, so the effect landed and the cooldown starts (R3).
		// Every cancel path clears CastingSlot before reaching here, which is what keeps a
		// cancelled cast from charging one.
		StartSlotCooldown(CastingSlot, LocalCastCooldownMs);

		// The displacement lands with the rest of the effect, not at cast start. The server
		// opened its speed window at the same moment, so the two line up.
		if (PendingDashDistCm > 0.f)
			RequestDash(PendingDashDirection, PendingDashDistCm, PendingDashSpeedCms, bPendingDashIgnoresWalls, bPendingDashThroughPawns);

		// A recast's first press landed - the window for the second press starts now
		if (bPendingRecast)
			OpenRecastWindow(CastingSlot);

		CastingSlot = 0;
		LocalCastEndsAt = 0.f;
		LocalCastCooldownMs = 0;
		PendingDashDistCm = 0.f;
	}

	// Send 판정
	bool ForceSendPacket = false;

	if (LastDesiredInput != DesiredInput)
	{
		ForceSendPacket = true;
		LastDesiredInput = DesiredInput;
	}

	// State 정보
	if (DesiredInput == FVector2D::Zero())
		SetMoveState(Protocol::MOVE_STATE_IDLE);
	else
		SetMoveState(Protocol::MOVE_STATE_RUN);

	MovePacketSendTimer -= DeltaTime;

	if (MovePacketSendTimer <= 0 || ForceSendPacket)
	{
		MovePacketSendTimer = MOVE_PACKET_SEND_DELAY;

		Protocol::C_MOVE MovePkt;

		// 현재 위치 정보
		{
			Protocol::PosInfo* Info = MovePkt.mutable_info();
			Info->CopyFrom(*PlayerInfo);
			Info->set_yaw(DesiredYaw);
			Info->set_state(GetMoveState());
		}

		// When it was sent, on a monotonic clock — the server measures the gap between moves with this, so a
		// hitch on either side that delivers several moves at once is not mistaken for a speed hack (MoveClock.h)
		MovePkt.set_client_ms(static_cast<uint64>(FPlatformTime::Seconds() * 1000.0));

		SEND_PACKET(MovePkt);
	}
}

void AS1MyPlayer::Move(const FInputActionValue& Value)
{
	// A dash owns the character until it finishes — movement-camera.md § Dashing
	// ("걷기 입력 무시"). Letting walk input through would fight the swept displacement
	// and put the client somewhere the server's speed window does not cover.
	if (IsDashing())
	{
		DoMove(0.f, 0.f);
		return;
	}

	// Hard CC drops the input entirely rather than letting it through and relying on the
	// server to snap us back — a rejection round trip would show a visible lurch.
	if (CanMove() == false)
	{
		DoMove(0.f, 0.f);
		return;
	}

	// input is a Vector2D
	FVector2D MovementVector = Value.Get<FVector2D>();

	// R5 — a stationary cast dies on movement input. The server reaches the same verdict
	// from the position stream, but the client has to clear its own gauge here: otherwise
	// the bar keeps filling on a cast the server already dropped, and the screen is lying.
	//
	// Sent immediately rather than waiting for the 33ms batch (R4 step 5*) — the whole
	// point is that the client and server agree about the cancel without a round trip.
	if (CastingSlot != 0 && bLocalCastLocksMovement && MovementVector.IsNearlyZero() == false)
	{
		Protocol::C_SKILL_CANCEL CancelPkt;
		SEND_PACKET(CancelPkt);

		AbortCast();
	}

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

	FVector CursorLocation;
	if (GetCursorWorldLocation(CursorLocation) == false)
		return;

	FVector ToCursor = CursorLocation - GetActorLocation();
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

bool AS1MyPlayer::GetCursorWorldLocation(FVector& OutLocation) const
{
	APlayerController* PC = Cast<APlayerController>(GetController());
	if (PC == nullptr)
		return false;

	FVector RayOrigin, RayDirection;
	if (PC->DeprojectMousePositionToWorld(RayOrigin, RayDirection) == false)
		return false;

	// Intersect the cursor ray with the plane the character stands on, solved directly
	// rather than with GetHitResultUnderCursor. A trace returns nothing when the cursor
	// sits over a hole or past the edge of the level, and aiming would fail exactly
	// when the player is most likely to be looking somewhere dangerous.
	if (FMath::Abs(RayDirection.Z) < UE_KINDA_SMALL_NUMBER)
		return false;

	const double Distance = (GetActorLocation().Z - RayOrigin.Z) / RayDirection.Z;
	if (Distance <= 0.0)
		return false;

	OutLocation = RayOrigin + RayDirection * Distance;
	return true;
}

const Protocol::SkillInfo* AS1MyPlayer::FindUsableSlot(int32 EquipSlotValue) const
{
	auto* GameInstance = Cast<US1GameInstance>(GetGameInstance());
	if (GameInstance == nullptr)
		return nullptr;

	for (const Protocol::SkillInfo& Info : GameInstance->SkillSlots)
	{
		if (static_cast<int32>(Info.slot()) != EquipSlotValue)
			continue;

		// An empty or shadowed slot does nothing at all — no error sound, no warning, no
		// cooldown (skill-system.md R1). Treat it as if the key were not bound.
		return Info.bind_state() == Protocol::BIND_BOUND ? &Info : nullptr;
	}

	return nullptr;
}

float AS1MyPlayer::GetSlotCooldownRemaining(int32 EquipSlotValue) const
{
	const int32 Index = EquipSlotValue - 1;
	if (Index < 0 || Index >= SKILL_SLOT_MAX)
		return 0.f;

	const UWorld* World = GetWorld();
	if (World == nullptr)
		return 0.f;

	return FMath::Max(0.f, SlotCooldownEndsAt[Index] - World->GetTimeSeconds());
}

void AS1MyPlayer::StartSlotCooldown(int32 EquipSlotValue, uint32 CooldownMs)
{
	const int32 Index = EquipSlotValue - 1;
	if (Index < 0 || Index >= SKILL_SLOT_MAX || CooldownMs == 0)
		return;

	const UWorld* World = GetWorld();
	if (World == nullptr)
		return;

	SlotCooldownEndsAt[Index] = World->GetTimeSeconds() + (CooldownMs / 1000.f);
}

void AS1MyPlayer::ResetAllSlotCooldowns()
{
	for (float& EndsAt : SlotCooldownEndsAt)
		EndsAt = 0.f;
	UE_LOG(LogTemp, Log, TEXT("[TESTMODE] predicted cooldowns cleared"));
}

void AS1MyPlayer::StartCooldownForSkill(uint32 SkillId, uint32 CooldownMs)
{
	const US1GameInstance* GameInstance = Cast<US1GameInstance>(GetGameInstance());
	if (GameInstance == nullptr || SkillId == 0)
		return;

	for (const Protocol::SkillInfo& Info : GameInstance->SkillSlots)
	{
		if (Info.skill_id() != SkillId || Info.bind_state() != Protocol::BIND_BOUND)
			continue;

		StartSlotCooldown(static_cast<int32>(Info.slot()), CooldownMs);
		UE_LOG(LogTemp, Log, TEXT("[SKILL INPUT] passive %u fired - slot %d cooldown %ums"), SkillId, static_cast<int32>(Info.slot()), CooldownMs);
	}
}

void AS1MyPlayer::OnMyAttackAccepted(uint32 WindupMs, uint32 IntervalMs)
{
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;

	if (IntervalMs > 0)
		AttackIntervalSec = IntervalMs / 1000.f;
	LocalWindupSeconds = WindupMs / 1000.f;

	// Re-anchor the beat on when this swing left, now that the real interval is known
	const float SentAt = (AttackSentAt > 0.f) ? AttackSentAt : Now;
	LocalReadyAt = SentAt + AttackIntervalSec + ATTACK_SEND_SLACK;
	AttackSentAt = 0.f;
}

bool AS1MyPlayer::MeetsWeaponRequirement(const FS1SkillDef& Def, FText& OutWhy) const
{
	if (Def.RequiresWeapon.IsEmpty())
		return true;

	// The weapon slot's item type and its skill pool (item_types.csv) - the server checks the same thing (Room::MeetsWeaponRequirement)
	const US1ItemSubsystem* Items = US1ItemSubsystem::Get();
	const FS1ItemView* Weapon = Items != nullptr ? Items->GetEquipped(1) : nullptr;
	const FS1ItemTypeDef* Type = (Weapon != nullptr && Items != nullptr) ? Items->GetCatalog().Find(Weapon->ItemTypeId) : nullptr;
	if (Type == nullptr || Type->SkillPoolId.ToString() != Def.RequiresWeapon)
	{
		OutWhy = FText::FromStringTable(TEXT("S1Items"), TEXT("notice.skill_needs_weapon"));
		return false;
	}

	if (Def.RequiresWeaponTag.IsEmpty())
		return true;

	// "sword:ghost" - one of the weapon's two skills must carry the tag (백귀야행 needs a ghost skill on the sword)
	if (const US1GameInstance* GameInstance = Cast<US1GameInstance>(GetGameInstance()))
	{
		for (const Protocol::SkillInfo& Info : GameInstance->SkillSlots)
		{
			if (Info.slot() != Protocol::SLOT_WEAPON_PRIMARY && Info.slot() != Protocol::SLOT_WEAPON_SECONDARY)
				continue;
			const FS1SkillDef* WeaponSkill = FS1SkillTable::Find(Info.skill_id());
			if (WeaponSkill != nullptr && WeaponSkill->HasTag(Def.RequiresWeaponTag))
				return true;
		}
	}

	OutWhy = FText::FromStringTable(TEXT("S1Items"), FString::Printf(TEXT("notice.skill_needs_%s"), *Def.RequiresWeaponTag));
	return false;
}

bool AS1MyPlayer::HasTargetlessBasic() const
{
	for (const FS1StatusEntry& Entry : Statuses)
	{
		if (Entry.Kind == Protocol::STATUS_BUFF && (Entry.Param == TEXT("basic_dash") || Entry.Param == TEXT("basic_spin")))
			return true;
	}
	return false;
}

void AS1MyPlayer::SendTargetlessAttack()
{
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	if (Now < LocalReadyAt)
		return;

	FVector Cursor = GetActorLocation() + GetActorForwardVector() * 100.f;
	GetCursorWorldLocation(Cursor);

	Protocol::C_ATTACK AttackPkt;
	AttackPkt.set_target_id(0);
	AttackPkt.set_aim_x(static_cast<float>(Cursor.X));
	AttackPkt.set_aim_y(static_cast<float>(Cursor.Y));
	SEND_PACKET(AttackPkt);

	LocalWindupEndsAt = Now + LocalWindupSeconds;
	LocalReadyAt = Now + AttackIntervalSec + ATTACK_SEND_SLACK;
	AttackSentAt = Now;
}

void AS1MyPlayer::OnSkillKey(int32 EquipSlotValue)
{
	// Pressing the key that is already being aimed cancels it. One key, two directions —
	// there is nothing extra to learn. skill-system.md § States transitions.
	if (AimingSlot == EquipSlotValue)
	{
		CancelAiming();
		return;
	}

	const Protocol::SkillInfo* Slot = FindUsableSlot(EquipSlotValue);
	if (Slot == nullptr)
		return;

	const FS1SkillDef* Def = FS1SkillTable::Find(Slot->skill_id());
	if (Def == nullptr)
		return;

	// A passive holds the key and does nothing when pressed (user decision 2026-10-10). Its effect fires on a
	// basic-attack hit or a lethal blow, server-side; the server also refuses the packet
	if (Def->Kind == ES1SkillKind::Passive)
		return;

	// Second press of a recast inside its window - fires through the cooldown, at the locked target
	if (RecastSlot == EquipSlotValue && GetWorld() != nullptr && GetWorld()->GetTimeSeconds() < RecastUntil)
	{
		FireRecast();
		return;
	}

	// A hold in progress: the same key stops it (no aiming), any other key does nothing - the server refuses them too
	if (HoldingSlot != 0)
	{
		if (HoldingSlot == EquipSlotValue)
		{
			Protocol::C_SKILL StopPkt;
			StopPkt.set_slot(static_cast<Protocol::EquipSlot>(EquipSlotValue));
			SEND_PACKET(StopPkt);
			UE_LOG(LogTemp, Log, TEXT("[SKILL INPUT] hold stop sent slot=%d"), EquipSlotValue);
		}
		return;
	}

	// A skill on cooldown does not even open aiming. Letting the range circle appear and
	// then swallowing the click would look identical to a broken skill — which is exactly
	// the confusion this whole session ran into.
	if (GetSlotCooldownRemaining(EquipSlotValue) > 0.f)
		return;

	// The armor skill's weapon condition - say why instead of casting into a silent server refusal
	// (2026-10-11 "백귀야행이 캐스팅만 되고 기술이 안 나간다": the sword had no ghost skill)
	FText Why;
	if (MeetsWeaponRequirement(*Def, Why) == false)
	{
		ShowFloatingNotice(Why);
		UE_LOG(LogTemp, Log, TEXT("[SKILL INPUT] slot=%d skill=%u refused locally - weapon requirement %s%s%s"), EquipSlotValue, Def->SkillId,
			*Def->RequiresWeapon, Def->RequiresWeaponTag.IsEmpty() ? TEXT("") : TEXT(":"), *Def->RequiresWeaponTag);
		return;
	}

	// Nothing to point at — the key fires it outright. This is what keeps instant skills
	// instant now that everything else costs an extra click (R4, R12b).
	if (Def->NeedsAiming() == false)
	{
		AimingSlot = EquipSlotValue;
		FireAimedSkill();
		return;
	}

	// Switching straight from one aim to another. The previous aim is dropped without a
	// trace — it was never sent anywhere.
	AimingSlot = EquipSlotValue;
}

void AS1MyPlayer::CancelAiming()
{
	AimingSlot = 0;
}

void AS1MyPlayer::AbortCast()
{
	CastingSlot = 0;
	LocalCastEndsAt = 0.f;
	LocalCastCooldownMs = 0;
	PendingDashDistCm = 0.f;
	bPendingRecast = false;
	PendingRecastTargetId = 0;
}

void AS1MyPlayer::OpenRecastWindow(int32 EquipSlotValue)
{
	const UWorld* World = GetWorld();
	if (World == nullptr)
		return;

	RecastSlot = EquipSlotValue;
	RecastUntil = World->GetTimeSeconds() + FS1SkillTable::RecastWindowMs() / 1000.f;
	RecastTargetId = PendingRecastTargetId;

	bPendingRecast = false;
	PendingRecastTargetId = 0;

	UE_LOG(LogTemp, Log, TEXT("[SKILL INPUT] recast window open slot=%d target=%llu for %ums"), RecastSlot, RecastTargetId, FS1SkillTable::RecastWindowMs());
}

void AS1MyPlayer::FireRecast()
{
	const int32 SlotValue = RecastSlot;
	const uint64 TargetId = RecastTargetId;

	// One second press per window, whatever the server makes of it
	RecastSlot = 0;
	RecastUntil = 0.f;
	RecastTargetId = 0;

	const Protocol::SkillInfo* Slot = FindUsableSlot(SlotValue);
	if (Slot == nullptr)
		return;

	const FS1SkillDef* Def = FS1SkillTable::Find(Slot->skill_id());

	// Aim at the locked target's current position - the server does the same (Room::FireSkill recast)
	FVector Aim = GetActorLocation() + GetActorForwardVector() * 100.f;
	if (const US1GameInstance* GameInstance = Cast<US1GameInstance>(GetGameInstance()))
	{
		if (const TWeakObjectPtr<AS1Player>* Found = GameInstance->Players.Find(TargetId))
		{
			if (const AS1Player* Target = Found->Get())
				Aim = Target->GetActorLocation();
		}
	}

	Protocol::C_SKILL SkillPkt;
	SkillPkt.set_slot(static_cast<Protocol::EquipSlot>(SlotValue));
	SkillPkt.set_target_id(TargetId);
	SkillPkt.set_aim_x(static_cast<float>(Aim.X));
	SkillPkt.set_aim_y(static_cast<float>(Aim.Y));
	SEND_PACKET(SkillPkt);

	// The second press's dash runs here, toward the target, inside the speed window the server opens for it
	if (Def != nullptr && Def->RecastMoveDistCm > 0.f && Def->RecastMoveSpeedCms > 0.f)
	{
		FVector Dir = Aim - GetActorLocation();
		Dir.Z = 0.0;
		if (Def->RecastMoveKind == TEXT("back"))
			Dir = -Dir;

		if (Dir.Normalize())
			RequestDash(Dir, Def->RecastMoveDistCm, Def->RecastMoveSpeedCms);
	}

	UE_LOG(LogTemp, Log, TEXT("[SKILL INPUT] recast fired slot=%d target=%llu"), SlotValue, TargetId);
}

void AS1MyPlayer::OnConfirmPressed()
{
	if (AimingSlot == 0)
		return;

	// The basic attack fires exactly once from aiming. Holding LMB does not repeat it —
	// that is what RMB is for. combat-system.md Rule 1.
	if (AimingSlot == AIMING_BASIC_ATTACK)
	{
		AS1Player* Target = AcquireTargetActorUnderCursor();
		AimingSlot = 0;

		// 일보일검 · 난무 - the swing needs no target (2026-10-11)
		if (HasTargetlessBasic())
			SendTargetlessAttack();
		else if (Target != nullptr)
			SendAttack(Target->GetPlayerInfo()->object_id());

		return;
	}

	FireAimedSkill();
}

void AS1MyPlayer::OnConfirmReleased()
{
	if (CastingSlot == 0)
		return;

	// Letting go IS the cancel — no clearer statement of intent exists (R4). The server
	// drops the cast and charges no cooldown.
	Protocol::C_SKILL_CANCEL CancelPkt;
	SEND_PACKET(CancelPkt);

	AbortCast();
}

void AS1MyPlayer::OnCancelPressed()
{
	if (AimingSlot != 0)
	{
		CancelAiming();

		// This hold is spent. Releasing and pressing again is what starts an attack —
		// a button pressed to call something off must not immediately start something else.
		bCancelHoldConsumed = true;
		return;
	}

	bSustainedAttack = true;
}

void AS1MyPlayer::OnCancelReleased()
{
	bCancelHoldConsumed = false;

	// Stops on release, with no lingering swing. Breaking off a fight has to be as fast as
	// lifting a finger — in an extraction dungeon that decision is the whole game.
	bSustainedAttack = false;
}

void AS1MyPlayer::FireAimedSkill()
{
	const Protocol::SkillInfo* Slot = FindUsableSlot(AimingSlot);
	if (Slot == nullptr)
	{
		UE_LOG(LogTemp, Error, TEXT("[SKILL INPUT] FireAimedSkill: slot %d not usable"), AimingSlot);
		AimingSlot = 0;
		return;
	}

	const FS1SkillDef* Def = FS1SkillTable::Find(Slot->skill_id());

	// A target skill fires only on a click on an enemy in range - empty ground keeps the aim open
	// (2026-10-11 "기습(단검)은 타겟팅이라서 마우스로 적을 클릭했을 때만 발동되도록"). The server re-checks range and walls
	AS1Player* ClickedTarget = AcquireTargetActorUnderCursor();
	if (Def != nullptr && Def->AimType == ES1SkillAimType::Target)
	{
		if (ClickedTarget == nullptr)
		{
			ShowFloatingNotice(FText::FromStringTable(TEXT("S1Items"), TEXT("notice.skill_needs_target")), 1.0f);
			return;
		}
		if (FVector::Dist2D(ClickedTarget->GetActorLocation(), GetActorLocation()) > Def->RangeCm + 20.f)
		{
			ShowFloatingNotice(FText::FromStringTable(TEXT("S1Items"), TEXT("notice.skill_out_of_range")), 1.0f);
			return;
		}
	}

	Protocol::C_SKILL SkillPkt;
	SkillPkt.set_slot(static_cast<Protocol::EquipSlot>(AimingSlot));

	// Target and aim always travel together. Which one matters is decided server-side from
	// the skill's shape, so the client never needs to know what shape it just fired — and
	// forging either is pointless because the server revalidates both.
	SkillPkt.set_target_id(ClickedTarget != nullptr ? ClickedTarget->GetPlayerInfo()->object_id() : 0);

	FVector CursorLocation;
	if (GetCursorWorldLocation(CursorLocation))
	{
		SkillPkt.set_aim_x(static_cast<float>(CursorLocation.X));
		SkillPkt.set_aim_y(static_cast<float>(CursorLocation.Y));
	}
	else
	{
		// Fall back to where we are facing. Losing the whole input would be worse than
		// aiming somewhere slightly stale.
		const FVector Ahead = GetActorLocation() + GetActorForwardVector() * 100.f;
		SkillPkt.set_aim_x(static_cast<float>(Ahead.X));
		SkillPkt.set_aim_y(static_cast<float>(Ahead.Y));
	}

	SEND_PACKET(SkillPkt);

	// Freeze the dash direction now. The server stored the same aim when this packet
	// arrived, so re-reading the cursor on completion would send the two apart.
	// 🔴 blink is the one movement the server performs itself (through walls, landing corrected) - it arrives
	//    as an S_MOVE correction, so nothing is queued here. back = away from the aim; dash / leap / forward = toward it
	if (Def != nullptr && Def->HasMovement() && Def->MoveKind != TEXT("blink"))
	{
		FVector DashDir = FVector(SkillPkt.aim_x(), SkillPkt.aim_y(), 0.f) - GetActorLocation();
		float DashDistCm = Def->MoveDistCm;

		// to_target (기습 단검 · 일도양단, 2026-10-11) - straight at the clicked enemy, stopping just in front of it
		if (Def->MoveKind == TEXT("to_target") && ClickedTarget != nullptr)
		{
			constexpr float StopShortCm = 90.f;		// two capsule radii plus a little - land beside it, not inside it
			DashDir = ClickedTarget->GetActorLocation() - GetActorLocation();
			DashDistCm = FMath::Clamp(static_cast<float>(FVector(DashDir.X, DashDir.Y, 0.f).Size()) - StopShortCm, 0.f, Def->MoveDistCm);
		}

		DashDir.Z = 0.0;
		if (Def->MoveKind == TEXT("back"))
			DashDir = -DashDir;

		if (DashDistCm > 0.f && DashDir.Normalize())
		{
			PendingDashDirection = DashDir;
			PendingDashDistCm = DashDistCm;
			PendingDashSpeedCms = Def->MoveSpeedCms;
			bPendingDashIgnoresWalls = Def->bMoveIgnoresWalls;		// 유령 신랑의 저주 - through walls (server skips its wall check for the window)
			bPendingDashThroughPawns = (Def->MoveKind == TEXT("dash_through"));		// 그림자 베기 - characters do not stop it
		}
	}

	// A recast remembers who the first press locked - the second press goes after them
	if (Def != nullptr && Def->Kind == ES1SkillKind::Recast)
	{
		bPendingRecast = true;
		PendingRecastTargetId = SkillPkt.target_id();
	}

	// A cast lives as long as the button is held, so remember which slot is in flight.
	// Instant skills are done the moment they are sent.
	if (Def != nullptr && Def->CastMs > 0)
	{
		CastingSlot = AimingSlot;

		const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
		LocalCastDuration = Def->CastMs / 1000.f;
		LocalCastEndsAt = Now + LocalCastDuration;
		bLocalCastLocksMovement = (Def->bCanMoveWhileCasting == false);

		// 🔴 Held back until the cast completes. R3 puts the cooldown at the moment the
		// effect lands, so a cancelled cast leaves no trace at all — including here.
		LocalCastCooldownMs = Def->CooldownMs;
	}
	else
	{
		CastingSlot = 0;
		LocalCastEndsAt = 0.f;

		// Instant skills land as they are sent, so the cooldown starts now. A hold's cooldown waits for its end
		if (Def != nullptr && Def->Kind == ES1SkillKind::Hold)
		{
			HoldingSlot = AimingSlot;
			UE_LOG(LogTemp, Log, TEXT("[SKILL INPUT] hold start slot=%d"), HoldingSlot);
		}
		else if (Def != nullptr)
		{
			StartSlotCooldown(AimingSlot, Def->CooldownMs);
		}

		if (PendingDashDistCm > 0.f)
		{
			RequestDash(PendingDashDirection, PendingDashDistCm, PendingDashSpeedCms, bPendingDashIgnoresWalls, bPendingDashThroughPawns);
			PendingDashDistCm = 0.f;
		}

		if (bPendingRecast)
			OpenRecastWindow(AimingSlot);
	}

	AimingSlot = 0;
}

void AS1MyPlayer::SetCameraScale(float Scale)
{
	if (CameraBoom == nullptr)
		return;

	CameraBoom->TargetArmLength = DefaultArmLength * FMath::Clamp(Scale, 0.5f, 2.f);
	UE_LOG(LogTemp, Log, TEXT("[FOW] camera arm=%.0f (x%.2f)"), CameraBoom->TargetArmLength, Scale);
}

void AS1MyPlayer::DebugBlind(float Seconds)
{
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	BlindUntil = Now + Seconds;
	if (BlindViewScale >= 1.f)
		BlindViewScale = 0.15f;		// skill_rules blind_view_scale - the real value arrives with the CC event
	UE_LOG(LogTemp, Warning, TEXT("[DEBUG] blind for %.1fs scale=%.2f"), Seconds, BlindViewScale);
}

void AS1MyPlayer::OnHoldEnded()
{
	if (HoldingSlot == 0)
		return;

	// The cooldown starts when the hold ends (server R3) - the same moment the server set its own
	if (const Protocol::SkillInfo* Slot = FindUsableSlot(HoldingSlot))
	{
		if (const FS1SkillDef* Def = FS1SkillTable::Find(Slot->skill_id()))
			StartSlotCooldown(HoldingSlot, Def->CooldownMs);
	}

	UE_LOG(LogTemp, Log, TEXT("[SKILL INPUT] hold ended slot=%d"), HoldingSlot);
	HoldingSlot = 0;
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

AS1Player* AS1MyPlayer::AcquireTargetActorUnderCursor() const
{
	APlayerController* PC = Cast<APlayerController>(GetController());
	if (PC == nullptr)
		return nullptr;

	// TODO: Rule 1 allows a 30px leniency radius (target_acquisition_leniency). An exact
	// cursor hit is unforgiving on small top-down silhouettes — revisit with a sphere
	// trace once the camera height is locked.
	FHitResult Hit;
	if (PC->GetHitResultUnderCursor(ECC_Pawn, false, Hit) == false)
		return nullptr;

	AS1Player* Target = Cast<AS1Player>(Hit.GetActor());
	if (Target == nullptr || Target->IsMyPlayer())
		return nullptr;

	return Target;
}

uint64 AS1MyPlayer::AcquireTargetUnderCursor()
{
	AS1Player* Target = AcquireTargetActorUnderCursor();

	return Target ? Target->GetPlayerInfo()->object_id() : 0;
}

void AS1MyPlayer::OnAimAttackKey()
{
	// Same toggle as a skill key: press again to put it away.
	if (AimingSlot == AIMING_BASIC_ATTACK)
	{
		CancelAiming();
		return;
	}

	AimingSlot = AIMING_BASIC_ATTACK;
}

void AS1MyPlayer::SendAttack(uint64 TargetId)
{
	if (TargetId == 0)
		return;

	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;

	// The server ignores requests that arrive mid-windup, so sending them is pure noise.
	if (Now < LocalReadyAt)
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
	LocalWindupEndsAt = Now + LocalWindupSeconds;

	// Gated on the full interval, not just the windup: the sustained attack would otherwise
	// fire again the moment the arc closes and flood the server with requests it drops.
	// The interval is the server's (S_ATTACK interval_ms, attack speed applied - OnMyAttackAccepted, 2026-10-11)
	LocalReadyAt = Now + AttackIntervalSec + ATTACK_SEND_SLACK;
	AttackSentAt = Now;
}

AS1Player* AS1MyPlayer::FindEnemyNearCursorInRange() const
{
	auto* GameInstance = Cast<US1GameInstance>(GetGameInstance());
	if (GameInstance == nullptr)
		return nullptr;

	FVector CursorLocation;
	if (GetCursorWorldLocation(CursorLocation) == false)
		return nullptr;

	const FVector MyLocation = GetActorLocation();
	const float RangeCm = GetBasicAttackRangeCm();

	AS1Player* Best = nullptr;
	double BestCursorDistSq = TNumericLimits<double>::Max();

	for (const auto& Pair : GameInstance->Players)
	{
		AS1Player* Other = Pair.Value.Get();
		if (Other == nullptr || Other == this || Other->IsMyPlayer())
			continue;

		const FVector OtherLocation = Other->GetActorLocation();

		// Range gate first. Staying inside attack range is what stops this from dragging
		// the player into fights they never walked up to — it never chases.
		FVector ToTarget = OtherLocation - MyLocation;
		ToTarget.Z = 0.0;
		if (ToTarget.SizeSquared() > FMath::Square(RangeCm))
			continue;

		FVector ToCursor = OtherLocation - CursorLocation;
		ToCursor.Z = 0.0;

		const double CursorDistSq = ToCursor.SizeSquared();
		if (CursorDistSq < BestCursorDistSq)
		{
			BestCursorDistSq = CursorDistSq;
			Best = Other;
		}
	}

	return Best;
}

float AS1MyPlayer::GetBasicAttackRangeCm() const
{
	float RangeCm = ATTACK_RANGE_CM;

	// equip_slot 1 = weapon (item_types.csv)
	if (const US1ItemSubsystem* Items = US1ItemSubsystem::Get())
	{
		const FS1ItemView* Weapon = Items->GetEquipped(1);
		const FS1ItemTypeDef* Type = Weapon != nullptr ? Items->GetCatalog().Find(Weapon->ItemTypeId) : nullptr;
		if (Type != nullptr)
			RangeCm = FMath::Max(ATTACK_RANGE_CM, Type->AttackRangeCm);		// EquipStats: max(base range, weapon range)
	}

	// Same order as the server's Player::RebuildCombatStats - melee replaces, then the range buffs add, floor 100 cm
	float MeleeCm = 0.f;
	float BonusCm = 0.f;
	for (const FS1StatusEntry& Entry : Statuses)
	{
		if (Entry.Kind != Protocol::STATUS_BUFF)
			continue;
		if (Entry.Param == TEXT("basic_melee"))
			MeleeCm = FMath::Max(MeleeCm, Entry.Magnitude);
		else if (Entry.Param == TEXT("attack_range"))
			BonusCm += Entry.Magnitude;
	}
	if (MeleeCm > 0.f)
		RangeCm = MeleeCm;

	return FMath::Max(100.f, RangeCm + BonusCm);
}

void AS1MyPlayer::TickSustainedAttack()
{
	if (bSustainedAttack == false)
		return;

	// Aiming and casting both take precedence — RMB held through them means something else.
	if (AimingSlot != 0 || CastingSlot != 0)
		return;

	if (CanAttack() == false)
		return;

	// A swing the server never acknowledged was refused (sent a hair early, mid-CC, out of range) - free the gate so
	// the hold keeps swinging instead of waiting out a whole interval for nothing
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	if (AttackSentAt > 0.f && Now - AttackSentAt > ATTACK_ACK_TIMEOUT)
	{
		AttackSentAt = 0.f;
		LocalReadyAt = Now;
	}

	// 일보일검 · 난무 - the swing goes toward the cursor even with nobody in range (2026-10-11)
	if (HasTargetlessBasic())
	{
		SendTargetlessAttack();
		return;
	}

	AS1Player* Target = FindEnemyNearCursorInRange();
	if (Target == nullptr)
		return;

	SendAttack(Target->GetPlayerInfo()->object_id());
}

void AS1MyPlayer::DrawWindupDebug()
{
	UWorld* World = GetWorld();
	if (World == nullptr)
		return;

	const float Now = World->GetTimeSeconds();
	if (Now >= LocalWindupEndsAt || LocalWindupSeconds <= 0.f)
		return;

	const float Elapsed = LocalWindupSeconds - (LocalWindupEndsAt - Now);
	const float Alpha = FMath::Clamp(Elapsed / LocalWindupSeconds, 0.f, 1.f);

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

void AS1MyPlayer::DrawSkillBarDebug()
{
	if (GEngine == nullptr)
		return;

	auto* GameInstance = Cast<US1GameInstance>(GetGameInstance());
	if (GameInstance == nullptr)
		return;

	if (GameInstance->SkillSlots.Num() == 0)
	{
		GEngine->AddOnScreenDebugMessage(SKILL_BAR_DEBUG_KEY, 0.f, FColor::Silver,
			TEXT("[SKILL BAR] waiting for S_EQUIP_SYNC"));
		return;
	}

	// Empty slots are drawn too. Six positions that never move are what lets the hand
	// learn where a skill lives — design/gdd/skill-system.md UI Requirements.
	for (int32 i = 0; i < GameInstance->SkillSlots.Num(); i++)
	{
		const Protocol::SkillInfo& Slot = GameInstance->SkillSlots[i];

		FString Line;
		FColor Color;

		const float Cooldown = GetSlotCooldownRemaining(static_cast<int32>(Slot.slot()));

		switch (Slot.bind_state())
		{
		case Protocol::BIND_BOUND:
			// Showing the remaining time matters more than dimming the row: a key that
			// simply does nothing when pressed is indistinguishable from a broken one.
			if (Cooldown > 0.f)
			{
				Line = FString::Printf(TEXT("[%-5s] skill %u  — %.1fs"),
					SlotKeyName(Slot.slot()), Slot.skill_id(), Cooldown);
				Color = FColor(120, 120, 140);
			}
			else
			{
				Line = FString::Printf(TEXT("[%-5s] skill %u"),
					SlotKeyName(Slot.slot()), Slot.skill_id());
				Color = FColor::White;
			}
			break;

		case Protocol::BIND_SHADOWED:
			// Naming the slot that won matters: without it there is no way to tell why
			// this key went dead. equipment-skill-binding.md B5.
			Line = FString::Printf(TEXT("[%-5s] skill %u  - shadowed by %s"),
				SlotKeyName(Slot.slot()), Slot.skill_id(), SlotKeyName(Slot.shadowed_by()));
			Color = FColor::Orange;
			break;

		default:
			Line = FString::Printf(TEXT("[%-5s] empty"), SlotKeyName(Slot.slot()));
			Color = FColor(90, 90, 90);
			break;
		}

		GEngine->AddOnScreenDebugMessage(SKILL_BAR_DEBUG_KEY + i, 0.f, Color, Line);
	}
}

void AS1MyPlayer::DrawCastGauge()
{
	UWorld* World = GetWorld();
	if (World == nullptr || LocalCastEndsAt <= 0.f)
		return;

	const float Now = World->GetTimeSeconds();
	if (Now >= LocalCastEndsAt)
		return;

	const float Remaining = LocalCastEndsAt - Now;
	const float Alpha = (LocalCastDuration > 0.f)
		? FMath::Clamp(1.f - (Remaining / LocalCastDuration), 0.f, 1.f)
		: 1.f;

	const FVector Center = GetActorLocation() - FVector(0.f, 0.f, 90.f);
	const int32 Filled = FMath::RoundToInt(CAST_ARC_SEGMENTS * Alpha);

	for (int32 i = 0; i < CAST_ARC_SEGMENTS; i++)
	{
		const float A0 = (2.f * UE_PI) * i / CAST_ARC_SEGMENTS;
		const float A1 = (2.f * UE_PI) * (i + 1) / CAST_ARC_SEGMENTS;

		const FVector P0 = Center + FVector(FMath::Cos(A0), FMath::Sin(A0), 0.f) * CAST_ARC_RADIUS;
		const FVector P1 = Center + FVector(FMath::Cos(A1), FMath::Sin(A1), 0.f) * CAST_ARC_RADIUS;

		DrawDebugLine(World, P0, P1, (i < Filled) ? FColor(90, 220, 190) : FColor(40, 70, 65),
			false, -1.f, 0, 6.f);
	}

	// 🔴 The single most important thing on screen right now. Letting go cancels, and
	// nothing else says so — this exact gap is what made the cast skill look like it did
	// nothing at all. skill-system.md UI Requirements (홀드 표식).
	if (GEngine)
	{
		FString Hint = FString::Printf(TEXT("CASTING  %.0f%%   — hold LMB"), Alpha * 100.f);
		if (bLocalCastLocksMovement)
			Hint += TEXT("  ·  moving cancels");

		GEngine->AddOnScreenDebugMessage(CAST_HINT_DEBUG_KEY, 0.f, FColor(90, 220, 190), Hint);
	}
}

void AS1MyPlayer::DrawAimIndicator()
{
	if (AimingSlot == 0)
		return;

	UWorld* World = GetWorld();
	if (World == nullptr)
		return;

	// The basic attack aims like a Target skill; it just has no table entry.
	// combat-system.md Rule 1 puts it on the same flow on purpose — one thing to learn.
	ES1SkillAimType AimType = ES1SkillAimType::Target;
	float Radius = GetBasicAttackRangeCm();

	if (AimingSlot != AIMING_BASIC_ATTACK)
	{
		const Protocol::SkillInfo* Slot = FindUsableSlot(AimingSlot);
		if (Slot == nullptr)
		{
			// The slot went away mid-aim — unequipped, or shadowed by a swap.
			AimingSlot = 0;
			return;
		}

		const FS1SkillDef* Def = FS1SkillTable::Find(Slot->skill_id());
		if (Def == nullptr)
			return;

		AimType = Def->AimType;
		Radius = FMath::Max(Def->RangeCm, Def->RadiusCm);
	}

	// Drawn on the ground plane. From a fixed top-down camera nothing else competes for it,
	// and it never covers the character being aimed with — same reasoning as the windup arc.
	const FVector Center = GetActorLocation() - FVector(0.f, 0.f, 90.f);

	// 🔴 Owner-only. Nobody else may learn that this player is aiming — the server is not
	// even told (skill-system.md R4), so there is nothing here to replicate.
	if (Radius > 0.f)
	{
		DrawDebugCircle(World, Center, Radius, AIM_CIRCLE_SEGMENTS, FColor(70, 160, 255),
			false, -1.f, 0, 3.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
	}

	FVector CursorLocation;
	if (GetCursorWorldLocation(CursorLocation) == false)
		return;

	CursorLocation.Z = Center.Z;

	FVector ToCursor = CursorLocation - Center;
	ToCursor.Z = 0.0;

	const float DistanceToCursor = static_cast<float>(ToCursor.Size());
	const bool bInRange = (Radius <= 0.f) || (DistanceToCursor <= Radius);

	// Out of range reads as grey, so the answer to "will this reach?" is on screen before
	// the click rather than after it.
	const FColor MarkColor = bInRange ? FColor(70, 160, 255) : FColor(110, 110, 110);

	switch (AimType)
	{
	case ES1SkillAimType::Target:
	{
		// Highlight the actor when there is one, because whether a target is *in range* is
		// the question the player is actually asking — and the cursor's own distance does
		// not answer it. A cursor 10cm past the circle over an enemy standing well inside
		// it would otherwise read as "out of range".
		if (AS1Player* HoverTarget = AcquireTargetActorUnderCursor())
		{
			FVector ToTarget = HoverTarget->GetActorLocation() - GetActorLocation();
			ToTarget.Z = 0.0;

			const bool bTargetInRange = ToTarget.SizeSquared() <= FMath::Square(Radius);

			DrawDebugCircle(World, HoverTarget->GetActorLocation() - FVector(0.f, 0.f, 90.f),
				80.f, 24, bTargetInRange ? FColor(90, 230, 130) : FColor(200, 90, 90),
				false, -1.f, 0, 5.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
			break;
		}

		// No target under the cursor: a plain ring so the aim still reads as live.
		DrawDebugCircle(World, CursorLocation, 60.f, 24, MarkColor,
			false, -1.f, 0, 3.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
		break;
	}

	case ES1SkillAimType::Direction:
	{
		if (DistanceToCursor < UE_KINDA_SMALL_NUMBER)
			break;

		// Clamped to range: the line shows where the dash actually ends, not where the
		// cursor happens to be. Pointing further does not travel further.
		const FVector Direction = ToCursor / DistanceToCursor;
		const float Length = (Radius > 0.f) ? FMath::Min(DistanceToCursor, Radius) : DistanceToCursor;

		DrawDebugLine(World, Center, Center + Direction * Length, FColor(70, 160, 255),
			false, -1.f, 0, 5.f);
		break;
	}

	case ES1SkillAimType::Point:
	{
		// Clamped onto the range circle. point skills are the one case where an out-of-range
		// click still fires — at the furthest legal spot (skill-system.md § 조준 인디케이터).
		FVector Impact = CursorLocation;
		if (bInRange == false && DistanceToCursor > UE_KINDA_SMALL_NUMBER)
			Impact = Center + (ToCursor / DistanceToCursor) * Radius;

		DrawDebugCircle(World, Impact, 80.f, 24, FColor(70, 160, 255),
			false, -1.f, 0, 3.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
		break;
	}

	case ES1SkillAimType::SelfArea:
		// The foot circle drawn above is the entire indicator. Nothing goes at the cursor
		// because the cursor plays no part in where this lands — but the radius still has
		// to be visible, which is the whole reason this is not AimType None.
		break;

	default:
		break;
	}
}

void AS1MyPlayer::DebugDashNoNotice()
{
	// Same distance and speed as the real dash, so the only difference the server sees is
	// the missing notice.
	const FS1SkillDef* Dash = FS1SkillTable::Find(1003);		// 질주 (boots) - 2026-10-10 roster
	if (Dash == nullptr || Dash->HasMovement() == false)
		return;

	UE_LOG(LogTemp, Warning, TEXT("[DEBUG] dash without C_SKILL - expect [MOVE REJECT] on the server"));
	RequestDash(GetActorForwardVector(), Dash->MoveDistCm, Dash->MoveSpeedCms);
}

void AS1MyPlayer::TestCmd(const FString& Args)
{
	// The server checks everything (FBTK_TEST_MODE=1) - this only forwards
	Protocol::C_CHAT Pkt;
	Pkt.set_msg(TCHAR_TO_UTF8(*(TEXT("/t ") + Args)));
	SEND_PACKET(Pkt);
	UE_LOG(LogTemp, Log, TEXT("[TESTMODE] console /t %s"), *Args);

	if (Args.StartsWith(TEXT("cd")) || Args.StartsWith(TEXT("gear")))
		ResetAllSlotCooldowns();
}

void AS1MyPlayer::DebugRadar()
{
	const US1GameInstance* GameInstance = Cast<US1GameInstance>(GetGameInstance());
	const US1EscapeSubsystem* Escape = US1EscapeSubsystem::Get();
	const US1RadarWidget* Radar = Escape != nullptr ? Escape->GetRadar() : nullptr;
	if (GameInstance == nullptr || Radar == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[RADAR] no radar yet"));
		return;
	}

	const FVector2D Self(GetActorLocation());
	constexpr float RadiusCm = 2250.f;

	// What is in range in the world — counted without the radar's filter
	int32 Players = 0;
	int32 Monsters = 0;
	for (const TPair<uint64, TWeakObjectPtr<AS1Player>>& Pair : GameInstance->Players)
	{
		const AS1Player* Other = Pair.Value.Get();
		if (Other == nullptr || Other == this || S1RadarLogic::IsInRadarRange(Self, FVector2D(Other->GetActorLocation()), RadiusCm) == false)
			continue;
		(Other->IsMonster() ? Monsters : Players)++;
	}

	int32 Towers = 0;
	int32 Chests = 0;
	int32 Party = 0;
	for (const FS1RadarBlip& Blip : Radar->GetBlips())
	{
		switch (Blip.Kind)
		{
		case ES1RadarBlip::Tower: Towers++; break;
		case ES1RadarBlip::Chest: Chests++; break;
		case ES1RadarBlip::Party: Party++; break;
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[RADAR] in range - players=%d monsters=%d (not drawable) at=(%.0f, %.0f) showing=%d"),
		Players, Monsters, Self.X, Self.Y, Radar->IsShowing() ? 1 : 0);
	UE_LOG(LogTemp, Log, TEXT("[RADAR] blips - tower=%d chest=%d party=%d"), Towers, Chests, Party);
}

void AS1MyPlayer::DebugCameraFootprint()
{
	APlayerController* PC = Cast<APlayerController>(GetController());
	if (PC == nullptr || FollowCamera == nullptr)
		return;

	int32 Width = 0, Height = 0;
	PC->GetViewportSize(Width, Height);

	// The ground under our feet — the capsule's bottom, not its centre
	const FVector Feet = GetActorLocation() - FVector(0.f, 0.f, GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
	const FVector CameraAt = FollowCamera->GetComponentLocation();

	UE_LOG(LogTemp, Warning, TEXT("[CAMERA] viewport=%dx%d fov=%.1f arm=%.0f camera=(%.1f, %.1f, %.1f)m from feet (x fwd/back, y side, z up)"),
		Width, Height, FollowCamera->FieldOfView, CameraBoom != nullptr ? CameraBoom->TargetArmLength : 0.f,
		(CameraAt.X - Feet.X) / 100.f, (CameraAt.Y - Feet.Y) / 100.f, (CameraAt.Z - Feet.Z) / 100.f);

	// Where a screen point's ray meets a horizontal plane HeightCm above our feet, as metres from our feet
	auto Hit = [&](float ScreenX, float ScreenY, float HeightCm, FVector& OutMetres) -> bool
	{
		FVector Origin, Direction;
		if (PC->DeprojectScreenPositionToWorld(ScreenX, ScreenY, Origin, Direction) == false || FMath::Abs(Direction.Z) < UE_KINDA_SMALL_NUMBER)
			return false;

		const double T = (Feet.Z + HeightCm - Origin.Z) / Direction.Z;
		if (T <= 0.0)
			return false;

		OutMetres = (Origin + Direction * T - Feet) / 100.f;
		return true;
	};

	struct FScreenPoint { const TCHAR* Name; float X; float Y; };
	const FScreenPoint Points[] =
	{
		{ TEXT("top-left"),		0.f,			0.f },
		{ TEXT("top-centre"),	Width * 0.5f,	0.f },
		{ TEXT("top-right"),	float(Width),	0.f },
		{ TEXT("centre"),		Width * 0.5f,	Height * 0.5f },
		{ TEXT("bottom-left"),	0.f,			float(Height) },
		{ TEXT("bottom-centre"),Width * 0.5f,	float(Height) },
		{ TEXT("bottom-right"),	float(Width),	float(Height) },
	};

	for (const FScreenPoint& Point : Points)
	{
		FVector At;
		if (Hit(Point.X, Point.Y, 0.f, At))
		{
			UE_LOG(LogTemp, Warning, TEXT("[CAMERA] ground %-13s x=%6.1f m  y=%6.1f m"), Point.Name, At.X, At.Y);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[CAMERA] ground %-13s does not reach the ground"), Point.Name);
		}
	}

	// A thing of this height stays on screen only while its top is below the top edge's ray — this is how far
	// ahead (along the top-centre ray) its top can be and still show
	for (const float HeightM : { 1.8f, 3.f, 5.f, 10.f, 20.f })
	{
		FVector At;
		if (Hit(Width * 0.5f, 0.f, HeightM * 100.f, At))
		{
			UE_LOG(LogTemp, Warning, TEXT("[CAMERA] top of a %4.1f m object leaves the screen past x=%6.1f m ahead"), HeightM, At.X);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[CAMERA] a %4.1f m object never reaches the top edge"), HeightM);
		}
	}
}
