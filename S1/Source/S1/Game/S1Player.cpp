// Fill out your copyright notice in the Description page of Project Settings.


#include "Game/S1Player.h"
#include "Game/Anim/S1AnimCatalog.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
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
#include "S1GameInstance.h"
#include "DrawDebugHelpers.h"
#include "Components/TextRenderComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Game/Monster/S1MonsterCatalog.h"
#include "Game/Item/S1ItemSubsystem.h"
#include "Game/S1SkillTable.h"
#include "Game/Level/S1LevelBuilder.h"

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
	TickAirborne();
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

	TickAnimation(DeltaTime);
}

void AS1Player::TickRemotePlayer(float DeltaTime)
{
	// A server push (knockback / pull) is animated by TickDash. The S_MOVE with the end point arrives in the same
	// flush and would otherwise snap the proxy there; let the push play out and resume interpolation from wherever
	// it left the actor, so the resulting lerp is a few cm, not a jump back to a stale start.
	if (IsDashing())
	{
		InterpStartLocation = GetActorLocation();
		InterpStartYaw = GetActorRotation().Yaw;
		InterpElapsed = 0.f;
		return;
	}

	const FVector CurLocation = GetActorLocation();
	const FVector DestLocation = ToVector(*DestInfo) + FVector(0.f, 0.f, GroundLiftCm);

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

	// Formula C5 — the server aggregates slow sources and sends the max; we only apply it. The equipment bonus and
	// the move-speed buff go in once, added together before the slow — the same order as the server's
	// Player::ComputeMoveSpeed (its ceiling for our moves), where the buff is added to percentMsBonus
	const US1ItemSubsystem* Items = US1ItemSubsystem::Get();
	const float Bonus = Items != nullptr ? Items->GetMoveSpeedBonus() : 0.f;

	// 모래주머니 (1174, 2026-10-11) - a bound skill's passive move_speed is in the same sum (server Player::RebuildCombatStats)
	float Passive = 0.f;
	if (const US1GameInstance* GameInstance = Cast<US1GameInstance>(GetGameInstance()))
	{
		for (const Protocol::SkillInfo& Info : GameInstance->SkillSlots)
		{
			if (Info.bind_state() != Protocol::BIND_BOUND)
				continue;
			if (const FS1SkillDef* Def = FS1SkillTable::Find(Info.skill_id()))
				Passive += Def->PassiveMoveSpeed;
		}
	}

	// Clamped like the server's Player::ComputeMoveSpeed (0.5x .. 1.5x base) - 전력 질주 +60 % on top of boots would otherwise
	// run past the server's ceiling and every move would bounce
	const float Speed = BaseWalkSpeed * (1.f + Bonus + MoveSpeedBuff + Passive) * (1.f - ActiveSlow);
	Movement->MaxWalkSpeed = FMath::Clamp(Speed, BaseWalkSpeed * 0.5f, BaseWalkSpeed * 1.5f);
	// DebugMoveSpeed (dev console) can only lower it - the server's ceiling is never crossed (walk/run animation checks, 2026-10-15)
	if (DebugMaxWalkSpeed > 0.f)
	{
		const bool bCapOn = DebugSpeedToggleSec <= 0.f
			|| (FMath::FloorToInt((GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f) / DebugSpeedToggleSec) % 2) == 0;
		if (bCapOn)
			Movement->MaxWalkSpeed = FMath::Min(Movement->MaxWalkSpeed, DebugMaxWalkSpeed);
	}

	if (CanMove() == false)
		Movement->StopMovementImmediately();
}

void AS1Player::OnDamaged(int32 Damage, int32 RemainingHp, bool bIsCrit, int32 NewMaxHp, bool bMagic)
{
	if (NewMaxHp > 0)
	{
		UE_LOG(LogTemp, Log, TEXT("[HP] max changed id=%llu max=%d->%d hp=%d"),
			PlayerInfo != nullptr ? PlayerInfo->object_id() : 0, MaxHp, NewMaxHp, RemainingHp);
		MaxHp = NewMaxHp;
	}

	CurrentHp = RemainingHp;
	OnAnimDamaged(Damage, RemainingHp);

	// Death ends every status; the server does not send expiries for them (Room::OnCreatureKilled)
	if (RemainingHp <= 0)
		ClearStatuses();

	// Damage 0 is a resync, not a hit — a monster that walked home heals to full and the server says so with
	// S_DAMAGE { damage 0, remaining_hp = max } because there is no heal packet (monster-ai.md T11)
	if (Damage <= 0)
		return;

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
	// Crit yellow, magic violet, physical white - a magic proc riding a physical swing (혼원일체 · 점화) reads as its own number
	Popup.Text->SetTextRenderColor(bIsCrit ? FColor::Yellow : (bMagic ? FColor(200, 150, 255) : FColor::White));
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

	// 숫자는 풀로 다시 쓴다(배열이 줄지 않는다) - 한 번 맞은 뒤로는 떠 있는 숫자가 없어도 매 프레임 카메라 계산을 했다 (2026-10-17)
	if (DamagePopups.ContainsByPredicate([](const FDamagePopup& Popup) { return Popup.bActive; }) == false)
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
		// Monsters in the art bible's monster teal (#199E70) so a bar alone says what it belongs to
		DrawDebugLine(World, Base - Half, (Base - Half) + Half * 2.f * Ratio,
			bIsMonster ? FColor(25, 158, 112) : FColor::Green, false, -1.f, 0, 6.f);
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
	Mark(Now < BlindUntil, FColor::Black);
	Mark(Now < TauntUntil, FColor(255, 120, 0));

	DrawStatusDebug();

	// 🔴 Everyone sees this, enemies included. It says "busy" and nothing else — no skill,
	// no remaining time, no range. skill-system.md B2a: the observer's payload is
	// { caster, is_stationary } and that is deliberately almost nothing.
	if (bCastMarkerVisible)
	{
		const FVector Feet = GetActorLocation() - FVector(0.f, 0.f, 90.f);

		// A ring rather than a gauge. A gauge would leak cast_ms — how long it takes is
		// exactly what the caster is paying for, and letting enemies read it hands back
		// the advantage the asymmetry was built to create.
		DrawDebugCircle(World, Feet, CAST_MARKER_RADIUS, 32,
			bCastMarkerStationary ? FColor(255, 210, 90) : FColor(150, 190, 255),
			false, -1.f, 0, 5.f, FVector(1, 0, 0), FVector(0, 1, 0), false);

		// Stationary casts get a second ring. That much IS public — the caster is visibly
		// rooted, so the information is on screen either way and hiding it would only make
		// the marker misleading.
		if (bCastMarkerStationary)
		{
			DrawDebugCircle(World, Feet, CAST_MARKER_RADIUS * 0.6f, 24, FColor(255, 210, 90),
				false, -1.f, 0, 3.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
		}
	}
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
	case Protocol::CC_TYPE_TAUNT:		TauntUntil = Until;		break;
	case Protocol::CC_TYPE_SILENCE:		SilenceUntil = Until;	break;

	case Protocol::CC_TYPE_LAUNCH:
		// The lift arc starts here and spans the CC (TickAirborne). An expiry lands it now
		LaunchUntil = Until;
		if (bApplied)
		{
			LaunchStart = Now;
			LaunchDuration = Info.duration_ms() / 1000.f;
		}
		else
		{
			LaunchDuration = 0.f;
		}
		break;

	case Protocol::CC_TYPE_SLOW:
		// Individual slow sources expire independently on the server, which recomputes the
		// aggregate. An expiry event here means every source is gone.
		if (bApplied == false)
			ActiveSlow = 0.f;
		else if (Info.magnitude() > ActiveSlow)
			ActiveSlow = Info.magnitude();
		break;

	case Protocol::CC_TYPE_BLIND:
		// magnitude = skill_rules blind_view_scale - the share of the view US1BlindOverlayWidget leaves uncovered
		BlindUntil = Until;
		if (bApplied && Info.magnitude() > 0.f)
			BlindViewScale = Info.magnitude();
		break;

	default:
		// Silence and heal reduction have no movement effect — they are gameplay-only and
		// will be read by the skill system (P1.5) and healing (P2).
		break;
	}
}

bool AS1Player::IsBlinded() const
{
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	return Now < BlindUntil;
}

FText AS1Player::GetCcLabel() const
{
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;

	// String Table keys (S1ItemStrings.csv cc.*) - Hangul, so it is painted by the UI font, never DrawDebugString
	TArray<FText> Parts;
	auto Add = [&Parts](bool bActive, const TCHAR* Key)
	{
		if (bActive)
			Parts.Add(FText::FromStringTable(TEXT("S1Items"), Key));
	};

	Add(Now < StunUntil, TEXT("cc.stun"));
	Add(Now < LaunchUntil, TEXT("cc.launch"));
	Add(Now < RootUntil, TEXT("cc.root"));
	Add(Now < TauntUntil, TEXT("cc.taunt"));
	Add(Now < BlindUntil, TEXT("cc.blind"));
	Add(Now < SilenceUntil, TEXT("cc.silence"));
	Add(ActiveSlow > 0.f, TEXT("cc.slow"));

	if (Parts.Num() == 0)
		return FText::GetEmpty();
	return FText::Join(FText::FromString(TEXT(" · ")), Parts);
}

void AS1Player::ShowFloatingNotice(const FText& Text, float Seconds)
{
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	FloatingNotice = Text;
	FloatingNoticeUntil = Now + Seconds;
}

const FText* AS1Player::GetFloatingNotice() const
{
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	return (Now < FloatingNoticeUntil && FloatingNotice.IsEmpty() == false) ? &FloatingNotice : nullptr;
}

void AS1Player::TickAirborne()
{
	USkeletalMeshComponent* MeshComp = GetMesh();
	if (MeshComp == nullptr)
		return;

	if (bMeshBaseCaptured == false)
	{
		MeshBaseOffset = MeshComp->GetRelativeLocation();
		bMeshBaseCaptured = true;
	}

	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	float Lift = 0.f;
	if (LaunchDuration > 0.f && Now < LaunchStart + LaunchDuration)
	{
		// Up and back down over the CC - peak at the middle (4h·t·(1-t))
		constexpr float PeakCm = 140.f;
		const float T = FMath::Clamp((Now - LaunchStart) / LaunchDuration, 0.f, 1.f);
		Lift = 4.f * PeakCm * T * (1.f - T);
	}

	const FVector Want = MeshBaseOffset + FVector(0.f, 0.f, Lift);
	if (MeshComp->GetRelativeLocation().Equals(Want, 0.5f) == false)
		MeshComp->SetRelativeLocation(Want);
}

void AS1Player::ApplyKnockback(const FVector& Direction, float DistCm, float SpeedCms)
{
	// Same mechanics as a dash: swept, stops at geometry — which is also where the server's 2D walls stopped it
	RequestDash(Direction, DistCm, SpeedCms);
}

void AS1Player::ApplyStatus(const Protocol::StatusInfo& Info, bool bApplied)
{
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	const FString Param = UTF8_TO_TCHAR(Info.param().c_str());

	int32 Index = INDEX_NONE;
	for (int32 i = 0; i < Statuses.Num(); i++)
	{
		if (Statuses[i].Kind == Info.kind() && Statuses[i].Param == Param && Statuses[i].InstigatorId == Info.instigator_id())
		{
			Index = i;
			break;
		}
	}

	if (bApplied)
	{
		if (Index == INDEX_NONE)
			Index = Statuses.AddDefaulted();

		FS1StatusEntry& Entry = Statuses[Index];
		Entry.Kind = Info.kind();
		Entry.Param = Param;
		Entry.InstigatorId = Info.instigator_id();
		Entry.Stacks = Info.stacks();
		Entry.Magnitude = Info.magnitude();
		Entry.Until = Now + Info.remaining_ms() / 1000.f;
	}
	else if (Index != INDEX_NONE)
	{
		// A dot expiry carries the stacks still running; 0 (or any other kind) means the line is gone
		if (Info.kind() == Protocol::STATUS_DOT && Info.stacks() > 0)
			Statuses[Index].Stacks = Info.stacks();
		else
			Statuses.RemoveAt(Index);
	}

	// A hold the server ended (max duration, CC, the second press) - the owner's key state follows (stage 3)
	if (bApplied == false && Info.kind() == Protocol::STATUS_HOLD)
		OnHoldEnded();

	// The one status the movement code reads. Several buffs on the same stat are separate skills, so they add
	MoveSpeedBuff = 0.f;
	for (const FS1StatusEntry& Entry : Statuses)
	{
		if (Entry.Kind == Protocol::STATUS_BUFF && Entry.Param == TEXT("move_speed"))
			MoveSpeedBuff += Entry.Magnitude;
	}

	UE_LOG(LogTemp, Log, TEXT("[STATUS%s] id=%llu kind=%d param=%s stacks=%u mag=%.2f ms=%u by=%llu"),
		bApplied ? TEXT("+") : TEXT("-"), PlayerInfo ? PlayerInfo->object_id() : 0, static_cast<int32>(Info.kind()),
		*Param, Info.stacks(), Info.magnitude(), Info.remaining_ms(), Info.instigator_id());
}

void AS1Player::ClearStatuses()
{
	Statuses.Reset();
	MoveSpeedBuff = 0.f;
}

void AS1Player::ShowRevealedSkills(const FString& Line, float Seconds)
{
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	RevealedSkills = Line;
	RevealedUntil = Now + FMath::Max(Seconds, 1.f);
}

void AS1Player::DrawStatusDebug()
{
	UWorld* World = GetWorld();
	if (World == nullptr)
		return;

	// 총명 - 드러난 스킬 구성 (한 줄, 머리 위)
	if (RevealedSkills.IsEmpty() == false && World->GetTimeSeconds() < RevealedUntil)
	{
		const FVector Where = GetActorLocation() + FVector(DEBUG_BAR_OFFSET_X - 60.f, 0.f, 0.f);
		DrawDebugString(World, Where, RevealedSkills, nullptr, FColor(120, 220, 255), 0.f, true, 1.1f);
	}

	if (Statuses.Num() == 0)
		return;

	// ASCII only — the debug font has no Hangul. "bleed x3 | atk +15% | mark:spear". Expired lines drop
	// when the server says so; the deadline here only trims lines whose expiry packet was lost
	const float Now = World->GetTimeSeconds();
	FString Line;

	for (int32 i = Statuses.Num() - 1; i >= 0; i--)
	{
		const FS1StatusEntry& Entry = Statuses[i];
		if (Now > Entry.Until + 1.f)
		{
			Statuses.RemoveAt(i);
			continue;
		}

		FString Piece;
		switch (Entry.Kind)
		{
		case Protocol::STATUS_DOT:
			Piece = FString::Printf(TEXT("%s x%u"), *Entry.Param, Entry.Stacks);
			break;
		case Protocol::STATUS_BUFF:
			// 모자 로스터 - 의미가 다른 버프 세 가지: 벽 통과 · 면역 · 피해 배율 (param 이 "stat:selector")
			if (Entry.Param.StartsWith(TEXT("wall_pierce")))
				Piece = TEXT("WALLS");
			else if (Entry.Param.StartsWith(TEXT("immune:")))
				Piece = Entry.Param;
			else if (Entry.Param.StartsWith(TEXT("damage_mod:")))
				Piece = FString::Printf(TEXT("dmg+%d%% %s"), FMath::RoundToInt(Entry.Magnitude * 100.f), *Entry.Param.Mid(11));
			// 2차 로스터 (2026-10-11) - 크기가 % 가 아닌 버프: 보호막(체력) · 사거리(cm) · 평타 모드(켜짐/꺼짐)
			else if (Entry.Param == TEXT("shield"))
				Piece = FString::Printf(TEXT("SHIELD %d"), FMath::RoundToInt(Entry.Magnitude));
			else if (Entry.Param == TEXT("scent"))
				Piece = TEXT("SCENT");
			else if (Entry.Param == TEXT("attack_range"))
				Piece = FString::Printf(TEXT("range %+dcm"), FMath::RoundToInt(Entry.Magnitude));
			else if (Entry.Param == TEXT("basic_only"))
				Piece = TEXT("BASIC ONLY");
			else if (Entry.Param == TEXT("basic_proc"))
				Piece = TEXT("ON-HIT+");
			else if (Entry.Param == TEXT("basic_dash"))
				Piece = TEXT("DASH SLASH");
			else if (Entry.Param == TEXT("basic_spin"))
				Piece = TEXT("SPIN SLASH");
			else if (Entry.Param == TEXT("basic_melee"))
				Piece = FString::Printf(TEXT("MELEE %dcm"), FMath::RoundToInt(Entry.Magnitude));
			else if (Entry.Param == TEXT("low_hp_bonus"))
				Piece = TEXT("LAST STAND");
			else if (Entry.Param == TEXT("thorns"))
				Piece = FString::Printf(TEXT("thorns %d%%"), FMath::RoundToInt(Entry.Magnitude * 100.f));
			// 3차 로스터 (2026-10-11)
			else if (Entry.Param == TEXT("front_guard"))
				Piece = FString::Printf(TEXT("GUARD front -%d%%"), FMath::RoundToInt(Entry.Magnitude * 100.f));
			else if (Entry.Param == TEXT("unmovable"))
				Piece = TEXT("UNMOVABLE");
			else if (Entry.Param == TEXT("projectile_dodge"))
				Piece = TEXT("DODGE");
			else if (Entry.Param == TEXT("damage_taken"))
				Piece = FString::Printf(TEXT("taken +%d%%"), FMath::RoundToInt(Entry.Magnitude * 100.f));
			else if (Entry.Param == TEXT("move_attack"))
				Piece = TEXT("MOVING SHOT");
			else if (Entry.Param == TEXT("basic_double"))
				Piece = TEXT("TWIN ORBS");
			else
				Piece = FString::Printf(TEXT("%s %+d%%"), *Entry.Param, FMath::RoundToInt(Entry.Magnitude * 100.f));
			break;
		case Protocol::STATUS_MARK:
			Piece = FString::Printf(TEXT("mark:%s"), *Entry.Param);
			break;
		case Protocol::STATUS_HOLD:
			Piece = FString::Printf(TEXT("hold:%s"), *Entry.Param);
			break;
		case Protocol::STATUS_COUNTER:
			Piece = TEXT("COUNTER");
			break;
		case Protocol::STATUS_PARRY:
			// 자업자득(갑옷 #107)은 같은 종류에 param "reflect" 로 온다 - 투사체를 되돌려 보내는 창
			// 풍차(창 갑옷 1146, 2026-10-11)는 param "windmill" - 사방의 투사체만
			Piece = (Entry.Param == TEXT("reflect")) ? TEXT("REFLECT") : (Entry.Param == TEXT("windmill") ? TEXT("WINDMILL") : TEXT("PARRY"));
			break;
		case Protocol::STATUS_STANCE:
			Piece = FString::Printf(TEXT("stance:%s"), *Entry.Param);
			break;
		case Protocol::STATUS_INVULN:
			Piece = TEXT("INVULN");
			break;
		case Protocol::STATUS_DECOY:
			// 그림자 보내기(1126)의 분신은 param "clone" - 걸어가며 시야를 나눠 준다
			Piece = (Entry.Param == TEXT("clone")) ? TEXT("clone out") : TEXT("decoy out");
			break;
		case Protocol::STATUS_STEALTH:
			// 은신 (그림자 감추기 · 니힐, FoW) - 다른 사람에겐 이 캐릭터가 아예 오지 않으므로 나만 본다
			Piece = TEXT("STEALTH");
			break;
		default:
			Piece = FString::Printf(TEXT("%d:%s"), static_cast<int32>(Entry.Kind), *Entry.Param);
			break;
		}

		if (Line.IsEmpty() == false)
			Line += TEXT(" | ");
		Line += Piece;
	}

	if (Line.IsEmpty())
		return;

	// Duration 0 = drawn this frame, dropped next tick — it tracks the actor instead of trailing it
	const FVector Where = GetActorLocation() + FVector(DEBUG_BAR_OFFSET_X - 30.f, 0.f, 0.f);
	DrawDebugString(World, Where, Line, nullptr, FColor(255, 200, 120), 0.f, true, 1.1f);
}

void AS1Player::ApplyCcState(const Protocol::CcStateInfo& State)
{
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;

	// A snapshot is the whole truth, so start from cleared deadlines. Anything the server
	// did not list has already expired — merging instead of overwriting would let a single
	// missed expiry event pin the character in a CC forever.
	const bool bWasAirborne = Now < LaunchUntil;

	StunUntil = 0.f;
	RootUntil = 0.f;
	KnockbackUntil = 0.f;
	LaunchUntil = 0.f;
	BlindUntil = 0.f;
	TauntUntil = 0.f;
	SilenceUntil = 0.f;

	for (const Protocol::CcSlot& Slot : State.slots())
	{
		const float Until = Now + Slot.remaining_ms() / 1000.f;

		switch (Slot.cc_type())
		{
		case Protocol::CC_TYPE_STUN:		StunUntil = Until;		break;
		case Protocol::CC_TYPE_ROOT:		RootUntil = Until;		break;
		case Protocol::CC_TYPE_KNOCKBACK:	KnockbackUntil = Until;	break;
		case Protocol::CC_TYPE_TAUNT:		TauntUntil = Until;		break;
		case Protocol::CC_TYPE_SILENCE:		SilenceUntil = Until;	break;
		case Protocol::CC_TYPE_LAUNCH:
			LaunchUntil = Until;
			if (bWasAirborne == false)
			{
				// Seen for the first time mid-air (FoW snapshot) - fly the rest of the arc from here
				LaunchStart = Now;
				LaunchDuration = Slot.remaining_ms() / 1000.f;
			}
			break;
		case Protocol::CC_TYPE_BLIND:
			BlindUntil = Until;
			if (Slot.magnitude() > 0.f)
				BlindViewScale = Slot.magnitude();
			break;

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

	return Now >= StunUntil && Now >= RootUntil && Now >= KnockbackUntil && Now >= LaunchUntil && Now >= TauntUntil && Now >= SkillLockUntil;
}

bool AS1Player::CanTurn() const
{
	// Root is the exception — it pins you but lets you keep aiming. That distinction is the
	// whole reason root and stun are separate CC types (combat-system.md Rule 5 table).
	// Taunt turns you toward the taunter server-side, so the cursor must not fight it.
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;

	return Now >= StunUntil && Now >= KnockbackUntil && Now >= LaunchUntil && Now >= TauntUntil;
}

void AS1Player::OnSkillCastStarted(bool bStationary)
{
	bCastMarkerVisible = true;
	bCastMarkerStationary = bStationary;
}

// ── Animation (2026-10-12) ──────────────────────────────────────────────────────────────────

void AS1Player::SetWeaponType(uint32 ItemTypeId)
{
	WeaponTypeId = ItemTypeId;
	AnimWeaponKey = bIsMonster ? FName(TEXT("monster")) : FS1AnimCatalog::WeaponKeyOf(ItemTypeId);

	// A new run reuses my pawn (escape → re-entry) - the corpse pose must not carry over
	if (bAnimDead && CurrentHp > 0)
	{
		bAnimDead = false;
		if (USkeletalMeshComponent* MeshComp = GetMesh())
			MeshComp->bPauseAnims = false;
		ActiveLayer = ES1AnimLayer::None;
		ActiveClipKey.Reset();
	}
	// Stance changes with the weapon - restart locomotion on the next tick
	if (ActiveLayer == ES1AnimLayer::Locomotion)
		ActiveClipKey.Reset();
}

namespace
{
	// Animation driver thresholds - design/quick-specs/animation-2026-10-12.md § 7 (named after the verifier asked, 2026-10-12)
	constexpr float ANIM_MOVE_THRESHOLD_CMS = 20.f;		// faster than this = "run"
	constexpr float ANIM_IDLE_DEBOUNCE_SEC = 0.25f;		// keep "run" this long after the last fast sample (remotes step at 30 Hz)
	constexpr float ANIM_RUN_RATE_MIN = 0.6f;			// run clip rate = speed / 340, clamped
	constexpr float ANIM_RUN_RATE_MAX = 1.4f;
	constexpr float ANIM_LOCO_BLEND_SEC = 0.2f;			// idle <-> run blend
	constexpr float ANIM_ATTACK_RATE_MIN = 0.5f;		// strike frame aligned to the server windup, clamped
	constexpr float ANIM_ATTACK_RATE_MAX = 2.5f;
	constexpr float ANIM_SKILL_RATE_MAX = 3.f;			// my own cast aligned to cast_ms, clamped
	constexpr float ANIM_RECOVERY_GRACE_SEC = 0.2f;		// after a skill's strike, moving ends its recovery - not before this (a dash's last frames)
	constexpr float ANIM_CHANNEL_DEFAULT_SEC = 1.5f;	// a looping motion with unknown cast time holds this long
	constexpr float ANIM_HIT_RATE = 1.2f;				// flinch plays a little fast
	constexpr float ANIM_HIT_REPEAT_GUARD_SEC = 0.2f;	// no new flinch unless the current one is almost over (multi-hit)
	// 8-direction locomotion (2026-10-15): walk_<dir> / run_<dir> clips with their natural speed (speed_cms)
	constexpr float ANIM_DIR_HYSTERESIS_DEG = 10.f;		// a direction is kept until the heading is 10° past its 22.5° edge
	constexpr float ANIM_RUN_ENTER_CMS = 260.f;			// walk → run above this (walk clips ~145-175 cm/s, run ~340-520, players move at 340)
	constexpr float ANIM_RUN_EXIT_CMS = 230.f;			// run → walk below this (slows, the first frames of a start)
	constexpr float ANIM_DIR_RATE_MIN = 0.5f;			// clip rate = actual speed / natural speed, clamped
	constexpr float ANIM_DIR_RATE_MAX = 1.8f;
	constexpr float ANIM_DIR_BLEND_SEC = 0.15f;			// direction / gait change blend (the step phase carries over)
}

bool AS1Player::PlayAnimClip(const FString& Key, float Rate, bool bLoop, ES1AnimLayer Layer, float BlendIn, float BlendOut, float StartPhase)
{
	USkeletalMeshComponent* MeshComp = GetMesh();
	UAnimInstance* Anim = MeshComp != nullptr ? MeshComp->GetAnimInstance() : nullptr;
	if (Anim == nullptr)
		return false;

	FS1AnimCatalog& Catalog = FS1AnimCatalog::Get();
	const FS1AnimClip* Clip = Catalog.Find(Key, AnimWeaponKey);
	UAnimSequenceBase* Sequence = Clip != nullptr ? Catalog.Load(*Clip, MeshComp->GetSkeletalMeshAsset() ? MeshComp->GetSkeletalMeshAsset()->GetSkeleton() : nullptr) : nullptr;
	if (Sequence == nullptr)
		return false;

	// Every clip plays in place - the server owns the position. Clips flagged root motion (GhostSamurai *_Root lunges) would move
	// the capsule through the montage: extract and discard it. Clips with root translation but no flag (Archer / Spear loops)
	// would drift the mesh ahead of the capsule and snap back: lock the root bone (in memory only, the asset is not saved)
	if (Anim->RootMotionMode != ERootMotionMode::IgnoreRootMotion)
		Anim->SetRootMotionMode(ERootMotionMode::IgnoreRootMotion);
	UAnimSequence* Seq = Cast<UAnimSequence>(Sequence);
	if (Seq != nullptr && !Seq->bEnableRootMotion && !Seq->bForceRootLock)
		Seq->bForceRootLock = true;

	const float PlayRate = (Rate > 0.f) ? Rate : Clip->Rate;
	// Loops get a very large count - the driver restarts them anyway when the key changes
	const int32 LoopCount = bLoop ? 100000 : 1;
	const float StartAt = FMath::Clamp(StartPhase, 0.f, 0.999f) * Sequence->GetPlayLength();
	ActiveMontage = Anim->PlaySlotAnimationAsDynamicMontage(Sequence, TEXT("DefaultSlot"), BlendIn, BlendOut, PlayRate, LoopCount, -1.f, StartAt);
	if (ActiveMontage == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ANIM] play failed key=%s weapon=%s asset=%s - no DefaultSlot in the anim blueprint?"), *Key, *AnimWeaponKey.ToString(), *Sequence->GetName());
		return false;
	}
	// One line per clip change (locomotion changes are rare, one-shots are a few per second at most) - the live-check evidence reads these
	UE_LOG(LogTemp, Log, TEXT("[ANIM] play %s key=%s weapon=%s asset=%s rate=%.2f loop=%d len=%.2fs"), *GetName(), *Key, *AnimWeaponKey.ToString(), *Sequence->GetName(), PlayRate, bLoop ? 1 : 0, Sequence->GetPlayLength());

	ActiveClipKey = Key;
	ActiveLayer = Layer;
	LocoRate = PlayRate;
	ActiveClipLength = Sequence->GetPlayLength();
	SkillStrikeAt = 0.f;		// OnSkillMotion sets it for a skill clip
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	OneShotEndsAt = bLoop ? 0.f : Now + (Sequence->GetPlayLength() - StartAt) / FMath::Max(PlayRate, 0.01f);
	return true;
}

bool AS1Player::PlayDirectionalLocomotion(float SpeedCms)
{
	// Keys are built once - this runs every frame for every moving body (2026-10-17: it allocated a new FString per lookup)
	static const FString RunF(TEXT("run_f"));
	static const FString WalkF(TEXT("walk_f"));
	static const TArray<FString> DirKeys = []()
	{
		TArray<FString> Keys;
		for (const TCHAR* Gait : { TEXT("walk_"), TEXT("run_") })
		{
			for (int32 Sector = 0; Sector < 8; Sector++)
				Keys.Add(FString(Gait) + S1LocoLogic::SectorSuffix(Sector));
		}
		return Keys;
	}();

	FS1AnimCatalog& Catalog = FS1AnimCatalog::Get();
	if (Catalog.FindExact(RunF, AnimWeaponKey) == nullptr || Catalog.FindExact(WalkF, AnimWeaponKey) == nullptr)
		return false;		// this weapon has no 8-direction set yet

	// Direction = where the body moves relative to where it faces (the cursor for me, the server yaw for others)
	const float MoveYaw = FMath::RadiansToDegrees(FMath::Atan2(LastMoveVelocity.Y, LastMoveVelocity.X));
	const float RelYaw = FMath::FindDeltaAngleDegrees(GetActorRotation().Yaw, MoveYaw);
	const int32 Sector = S1LocoLogic::DirectionSector(RelYaw, LocoSector, ANIM_DIR_HYSTERESIS_DEG);
	const bool bRun = S1LocoLogic::IsRunGait(SpeedCms, bLocoRun, ANIM_RUN_ENTER_CMS, ANIM_RUN_EXIT_CMS);
	const FString& Key = DirKeys[(bRun ? 8 : 0) + ((Sector % 8) + 8) % 8];		// same string as "run_"/"walk_" + SectorSuffix

	const FS1AnimClip* Clip = Catalog.FindExact(Key, AnimWeaponKey);
	if (Clip == nullptr)
		return false;
	// Stride matches the ground speed: the clip plays at actual speed / its natural speed
	const float Rate = Clip->SpeedCms > 0.f
		? FMath::Clamp(SpeedCms / Clip->SpeedCms, ANIM_DIR_RATE_MIN, ANIM_DIR_RATE_MAX)
		: FMath::Clamp(SpeedCms / BaseWalkSpeed, ANIM_RUN_RATE_MIN, ANIM_RUN_RATE_MAX);

	UAnimInstance* Anim = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr;
	const bool bPlaying = Anim != nullptr && ActiveMontage != nullptr && Anim->Montage_IsPlaying(ActiveMontage);
	if (ActiveLayer == ES1AnimLayer::Locomotion && ActiveClipKey == Key && bPlaying)
	{
		if (FMath::Abs(Rate - LocoRate) > 0.05f)
		{
			Anim->Montage_SetPlayRate(ActiveMontage, Rate);
			LocoRate = Rate;
		}
		return true;
	}

	// Direction or gait changed: start the new loop at the same point of the step cycle so the feet carry on
	// (the pack's walk clips share one length, the runs another - the cycles line up)
	const bool bFromDirectional = ActiveLayer == ES1AnimLayer::Locomotion && bPlaying
		&& (ActiveClipKey.StartsWith(TEXT("walk_")) || ActiveClipKey.StartsWith(TEXT("run_")));
	const float Phase = bFromDirectional ? S1LocoLogic::LoopPhase(Anim->Montage_GetPosition(ActiveMontage), ActiveClipLength) : 0.f;
	const float Blend = bFromDirectional ? ANIM_DIR_BLEND_SEC : ANIM_LOCO_BLEND_SEC;
	if (PlayAnimClip(Key, Rate, true, ES1AnimLayer::Locomotion, Blend, Blend, Phase) == false)
		return false;
	LocoSector = Sector;
	bLocoRun = bRun;
	return true;
}

void AS1Player::PlayLocomotionClip(bool bMoving, float SpeedCms)
{
	if (bMoving && PlayDirectionalLocomotion(SpeedCms))
		return;
	if (bMoving == false)
		LocoSector = -1;		// standing still - the next start picks its direction fresh

	// The server moves everyone at 340 cm/s (less under a slow) - the run clip is stretched to the real speed
	static const FString RunKey(TEXT("run"));
	static const FString IdleKey(TEXT("idle"));
	const FString& Key = bMoving ? RunKey : IdleKey;
	const float Rate = bMoving ? FMath::Clamp(SpeedCms / BaseWalkSpeed, ANIM_RUN_RATE_MIN, ANIM_RUN_RATE_MAX) : 1.f;
	if (ActiveLayer == ES1AnimLayer::Locomotion && ActiveClipKey == Key)
	{
		// Same clip - only the speed changes (a slow landed / ended)
		UAnimInstance* Anim = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr;
		if (Anim != nullptr && ActiveMontage != nullptr && Anim->Montage_IsPlaying(ActiveMontage))
		{
			if (FMath::Abs(Rate - LocoRate) > 0.05f)
			{
				Anim->Montage_SetPlayRate(ActiveMontage, Rate);
				LocoRate = Rate;
			}
			return;
		}
	}
	if (PlayAnimClip(Key, Rate, true, ES1AnimLayer::Locomotion, ANIM_LOCO_BLEND_SEC, ANIM_LOCO_BLEND_SEC) == false)
	{
		ActiveLayer = ES1AnimLayer::None;		// no clip for this weapon - the template ABP keeps walking
		ActiveClipKey.Reset();
	}
}

void AS1Player::TickAnimation(float DeltaTime)
{
	if (bAnimDead)
	{
		// Hold the last frame of the death clip until the corpse despawns
		const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
		if (OneShotEndsAt > 0.f && Now >= OneShotEndsAt - 0.05f && GetMesh() != nullptr && GetMesh()->bPauseAnims == false)
			GetMesh()->bPauseAnims = true;
		return;
	}
	if (FS1AnimCatalog::Get().IsLoaded() == false)
		return;

	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	if (ActiveLayer == ES1AnimLayer::Attack || ActiveLayer == ES1AnimLayer::Skill || ActiveLayer == ES1AnimLayer::Hit)
	{
		// A skill's recovery (after its strike) gives way once the character moves again - the GhostSamurai clips stand back up
		// for ~2 s. The grace keeps a dash skill's own arrival from ending it
		const bool bWalksOffRecovery = ActiveLayer == ES1AnimLayer::Skill && SkillStrikeAt > 0.f && Now > SkillStrikeAt + ANIM_RECOVERY_GRACE_SEC
			&& GetCharacterMovement() != nullptr && GetCharacterMovement()->Velocity.Size2D() > ANIM_MOVE_THRESHOLD_CMS;
		if (Now < OneShotEndsAt && bWalksOffRecovery == false)
			return;
	}

	// Remote players and monsters move in 30 Hz server steps, so their velocity flickers around zero between packets -
	// keep "run" for 0.25 s after the last fast sample instead of restarting idle/run every frame (live check 2026-10-12: a passing pack restarted 2,000+ times)
	const FVector Velocity = GetCharacterMovement() ? GetCharacterMovement()->Velocity : FVector::ZeroVector;
	const float Speed = Velocity.Size2D();
	if (Speed > ANIM_MOVE_THRESHOLD_CMS)
	{
		LastMovingTime = Now;
		LastMoveSpeed = Speed;
		LastMoveVelocity = FVector(Velocity.X, Velocity.Y, 0.f);
	}
	const bool bMoving = (Now - LastMovingTime) < ANIM_IDLE_DEBOUNCE_SEC;
	PlayLocomotionClip(bMoving, bMoving ? LastMoveSpeed : 0.f);
}

void AS1Player::OnAttackStarted(uint32 WindupMs)
{
	if (bAnimDead)
		return;
	const FS1AnimClip* Clip = FS1AnimCatalog::Get().Find(TEXT("attack"), AnimWeaponKey);
	float Rate = 1.f;
	if (Clip != nullptr && WindupMs > 0)
	{
		// The strike (hit_pct of the clip) lands when the server says the hit resolves
		USkeletalMeshComponent* MeshComp = GetMesh();
		UAnimSequenceBase* Sequence = FS1AnimCatalog::Get().Load(*Clip, MeshComp && MeshComp->GetSkeletalMeshAsset() ? MeshComp->GetSkeletalMeshAsset()->GetSkeleton() : nullptr);
		if (Sequence != nullptr)
		{
			const float HitAt = Sequence->GetPlayLength() * Clip->HitPct / 100.f;
			Rate = FMath::Clamp(HitAt / (WindupMs / 1000.f), ANIM_ATTACK_RATE_MIN, ANIM_ATTACK_RATE_MAX);
		}
	}
	// Alternate the two swings so a sustained attack does not stutter on one clip
	static int32 Swing = 0;
	const TCHAR* Key = ((Swing++ & 1) == 0) ? TEXT("attack") : TEXT("attack2");
	PlayAnimClip(Key, Rate, false, ES1AnimLayer::Attack, 0.1f, 0.15f);
}

void AS1Player::OnSkillMotion(uint32 MotionId, bool bStationary, uint32 CastMs)
{
	if (bAnimDead)
		return;
	FS1AnimCatalog& Catalog = FS1AnimCatalog::Get();
	const FS1AnimClip* Clip = Catalog.FindMotion(MotionId, AnimWeaponKey);
	if (Clip == nullptr)
		return;		// m_none / unknown - nothing to show
	// The strike lands when the effect does: after the cast, and for a dash skill (기습 · 일도양단) after the dash arrives -
	// the windup covers the travel. Only my own cast knows both; observers play the table's rate (sized for a full dash)
	const float StrikeSec = CastMs / 1000.f + LastSkillDashSec();
	USkeletalMeshComponent* MeshComp = GetMesh();
	UAnimSequenceBase* Sequence = Catalog.Load(*Clip, MeshComp && MeshComp->GetSkeletalMeshAsset() ? MeshComp->GetSkeletalMeshAsset()->GetSkeleton() : nullptr);
	const float WindupClipSec = Sequence != nullptr ? Sequence->GetPlayLength() * (Clip->HitPct - Clip->StartPct) / 100.f : 0.f;
	float Rate = 0.f;
	if (StrikeSec > 0.f && Clip->bLoop == false && Sequence != nullptr)
		Rate = FMath::Clamp(WindupClipSec / StrikeSec, ANIM_ATTACK_RATE_MIN, ANIM_SKILL_RATE_MAX);
	if (PlayAnimClip(Clip->Key, Rate, Clip->bLoop, ES1AnimLayer::Skill, 0.1f, 0.15f, Clip->StartPct / 100.f) == false)
		return;
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	if (Clip->bLoop)
		OneShotEndsAt = Now + (CastMs > 0 ? CastMs / 1000.f : ANIM_CHANNEL_DEFAULT_SEC);	// a channel holds for the cast, then locomotion resumes
	else
		SkillStrikeAt = Now + WindupClipSec / FMath::Max(LocoRate, 0.01f);		// LocoRate = the rate PlayAnimClip used
}

void AS1Player::OnAnimDamaged(int32 Damage, int32 RemainingHp)
{
	if (bAnimDead)
		return;
	if (RemainingHp <= 0)
	{
		bAnimDead = true;
		if (PlayAnimClip(TEXT("death"), 0.f, false, ES1AnimLayer::Dead, 0.1f, 0.f) == false)
			OneShotEndsAt = 0.f;
		return;
	}
	if (Damage <= 0 || ActiveLayer == ES1AnimLayer::Attack || ActiveLayer == ES1AnimLayer::Skill)
		return;		// a swing is not interrupted by a flinch; a damage-0 resync is not a hit
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	if (ActiveLayer == ES1AnimLayer::Hit && Now < OneShotEndsAt - ANIM_HIT_REPEAT_GUARD_SEC)
		return;		// one flinch at a time (multi-hit skills)
	PlayAnimClip(TEXT("hit"), ANIM_HIT_RATE, false, ES1AnimLayer::Hit, 0.05f, 0.15f);
}

void AS1Player::OnSkillCastEnded()
{
	bCastMarkerVisible = false;
}

void AS1Player::PlaySkillEffect(UWorld* World, uint32 EffectId, const FVector& Impact, const FVector& Aim, AS1Player* Target)
{
	if (World == nullptr)
		return;

	// Ground plane, same as every other readout — from a fixed top-down camera it is the one
	// surface nothing competes for.
	const FVector Ground(Impact.X, Impact.Y, (Target != nullptr)
		? Target->GetActorLocation().Z - 90.f
		: 0.f);

	// 🔴 Branching on effect_id, never skill_id — an observer sees a shape and a colour and has to
	// infer the rest; that inference is the fight (equipment-skill-binding.md B8). effect_id = skill_id
	// since the 2026-10-10 roster, but the lookup only reads the SHAPE: 76 skills get the same six
	// readouts, one per hit shape, until the art budget opens. 🔴 TEMP shapes (DrawDebug, not Niagara).
	//
	// Colour by power — physical warm, magic cool, hybrid in between — so "what hit me" has a second channel.
	const FS1SkillDef* Def = FS1SkillTable::Find(EffectId);

	// The boss telegraph keeps its own look: it is the warning, not the hit (monster-ai.md Core Rule 18 · F10)
	if (EffectId == 9001)
	{
		// skills.csv telegraph_ms - 2026-10-18 1450 → 1900 (감속 40 % 에서도 피할 수 있게, monster-ai.md F10)
		const float TelegraphSec = (Def != nullptr && Def->TelegraphMs > 0) ? Def->TelegraphMs / 1000.f : 1.9f;
		DrawDebugCircle(World, Ground, 400.f, 64, FColor(230, 60, 40),
			false, TelegraphSec, 0, 10.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
		DrawDebugCircle(World, Ground, 200.f, 40, FColor(150, 40, 30),
			false, TelegraphSec, 0, 4.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
		return;
	}

	// The boar chief's charge telegraph (monster-ai.md § L): the strip it will run down, held for telegraph_ms while it
	// paws the ground. Same warning colour as the boss — side-step out of the strip and it misses. 🔴 TEMP (DrawDebug)
	if (EffectId == 9002)
	{
		const float LengthCm = (Def != nullptr && Def->RangeCm > 0.f) ? Def->RangeCm : 700.f;
		const float WidthCm = (Def != nullptr && Def->WidthCm > 0.f) ? Def->WidthCm : 140.f;
		const float HoldSec = (Def != nullptr && Def->TelegraphMs > 0) ? Def->TelegraphMs / 1000.f : 0.75f;
		FVector ChargeDir = FVector(Aim.X - Impact.X, Aim.Y - Impact.Y, 0.f);
		if (ChargeDir.Normalize() == false)
			return;
		const FVector Side = FVector(-ChargeDir.Y, ChargeDir.X, 0.f) * (WidthCm * 0.5f);
		const FVector End = Ground + ChargeDir * LengthCm;
		const FColor Warn(230, 60, 40);
		DrawDebugLine(World, Ground + Side, End + Side, Warn, false, HoldSec, 0, 10.f);
		DrawDebugLine(World, Ground - Side, End - Side, Warn, false, HoldSec, 0, 10.f);
		DrawDebugLine(World, End + Side, End - Side, Warn, false, HoldSec, 0, 10.f);
		DrawDebugLine(World, Ground + Side, Ground - Side, FColor(150, 40, 30), false, HoldSec, 0, 4.f);
		DrawDebugLine(World, Ground, End, FColor(150, 40, 30), false, HoldSec, 0, 4.f);
		return;
	}

	if (Def == nullptr)
	{
		// Unknown effect_id — the client table is behind the server's. Draw something rather
		// than nothing: a hit the player cannot see is worse than one they cannot identify.
		DrawDebugCircle(World, Ground, 60.f, 16, FColor(180, 180, 180),
			false, EFFECT_LIFETIME_SEC, 0, 4.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
		return;
	}

	FColor Colour(230, 70, 70);
	switch (Def->Power)
	{
	case ES1SkillPower::Ap:		Colour = FColor(90, 140, 255); break;
	case ES1SkillPower::Hybrid:	Colour = FColor(200, 90, 230); break;
	case ES1SkillPower::None:	Colour = FColor(90, 220, 190); break;
	default: break;
	}

	// Direction toward the aim. A zero aim (self area) draws nothing directional
	FVector Dir = FVector(Aim.X, Aim.Y, 0.f) - FVector(Impact.X, Impact.Y, 0.f);
	Dir.Z = 0.0;
	const bool bHasDir = Dir.Normalize();

	switch (Def->Shape)
	{
	case ES1SkillShape::Single:
	{
		// A cross on the impact — the one hit, where it landed
		const float Size = 90.f;
		DrawDebugLine(World, Ground + FVector(-Size, 0.f, 0.f), Ground + FVector(Size, 0.f, 0.f), Colour, false, EFFECT_LIFETIME_SEC, 0, 8.f);
		DrawDebugLine(World, Ground + FVector(0.f, -Size, 0.f), Ground + FVector(0.f, Size, 0.f), Colour, false, EFFECT_LIFETIME_SEC, 0, 8.f);
		break;
	}

	case ES1SkillShape::Projectile:
	{
		// Only the launch - the flight and the impact are drawn by US1ProjectileSubsystem from S_PROJECTILE (stage 2).
		// A strip here would read as an instant hit, which is exactly what a projectile is not
		DrawDebugCircle(World, Ground, 45.f, 12, Colour, false, EFFECT_LIFETIME_SEC * 0.5f, 0, 4.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
		if (bHasDir)
			DrawDebugLine(World, Ground, Ground + Dir * 90.f, Colour, false, EFFECT_LIFETIME_SEC * 0.5f, 0, 4.f);
		break;
	}

	case ES1SkillShape::Line:
	{
		if (bHasDir == false)
			break;

		// The swept strip: the whole length is the reach, the half-width is the miss margin
		const FVector Side = FVector(-Dir.Y, Dir.X, 0.f) * (Def->WidthCm * 0.5f);
		const FVector End = Ground + Dir * Def->RangeCm;
		DrawDebugLine(World, Ground + Side, End + Side, Colour, false, EFFECT_LIFETIME_SEC, 0, 4.f);
		DrawDebugLine(World, Ground - Side, End - Side, Colour, false, EFFECT_LIFETIME_SEC, 0, 4.f);
		DrawDebugLine(World, Ground, End, Colour, false, EFFECT_LIFETIME_SEC, 0, 8.f);
		break;
	}

	case ES1SkillShape::Cone:
	{
		if (bHasDir == false)
			break;

		// Two edges and the arc between them — the angle is the lesson (stand outside it)
		const float Half = FMath::DegreesToRadians(Def->AngleDeg * 0.5f);
		const FVector Left = Dir.RotateAngleAxis(FMath::RadiansToDegrees(-Half), FVector::UpVector);
		const FVector Right = Dir.RotateAngleAxis(FMath::RadiansToDegrees(Half), FVector::UpVector);
		DrawDebugLine(World, Ground, Ground + Left * Def->RadiusCm, Colour, false, EFFECT_LIFETIME_SEC, 0, 6.f);
		DrawDebugLine(World, Ground, Ground + Right * Def->RadiusCm, Colour, false, EFFECT_LIFETIME_SEC, 0, 6.f);

		constexpr int32 Segments = 12;
		FVector Prev = Ground + Left * Def->RadiusCm;
		for (int32 i = 1; i <= Segments; i++)
		{
			const float Deg = FMath::RadiansToDegrees(-Half + (2.f * Half) * i / Segments);
			const FVector Next = Ground + Dir.RotateAngleAxis(Deg, FVector::UpVector) * Def->RadiusCm;
			DrawDebugLine(World, Prev, Next, Colour, false, EFFECT_LIFETIME_SEC, 0, 6.f);
			Prev = Next;
		}
		break;
	}

	case ES1SkillShape::CircleSelf:
	case ES1SkillShape::CirclePoint:
	{
		// A pure dash (radius 0) is an arrival marker — the mover's own position stream shows the path
		if (Def->RadiusCm <= 0.f)
		{
			DrawDebugCircle(World, Ground, 70.f, 24, Colour, false, EFFECT_LIFETIME_SEC, 0, 6.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
			break;
		}

		// The radius has to be visible or a bystander cannot learn how far it reaches,
		// which is the only way to learn to stand outside it
		FVector Centre = Ground;
		if (Def->Shape == ES1SkillShape::CirclePoint && bHasDir)
		{
			const float Reach = FMath::Min(static_cast<float>(FVector::Dist2D(Aim, Impact)), Def->RangeCm);
			Centre = Ground + Dir * Reach;
		}
		DrawDebugCircle(World, Centre, Def->RadiusCm, 40, Colour, false, EFFECT_LIFETIME_SEC, 0, 7.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
		break;
	}

	default:
		// Shape none — a self buff. A small pulse at the caster's feet says "something was used"
		DrawDebugCircle(World, Ground, 50.f, 16, Colour, false, EFFECT_LIFETIME_SEC, 0, 3.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
		break;
	}
}

void AS1Player::PlayTargetStrike(UWorld* World, uint32 EffectId, AS1Player* Target)
{
	if (World == nullptr || Target == nullptr)
		return;

	const FVector At = Target->GetActorLocation();
	const FVector Feet = At - FVector(0.f, 0.f, 90.f);
	const FS1SkillDef* Def = FS1SkillTable::Find(EffectId);
	constexpr float Life = 0.6f;

	UE_LOG(LogTemp, Log, TEXT("[SKILL FX] strike effect=%u on=%llu"), EffectId, Target->GetPlayerInfo() != nullptr ? Target->GetPlayerInfo()->object_id() : 0);

	// Ghost tag (혼원일체 원혼) - violet rings rising off the target
	if (Def != nullptr && Def->HasTag(TEXT("ghost")))
	{
		const FColor Violet(190, 120, 255);
		for (int32 i = 0; i < 3; i++)
			DrawDebugCircle(World, Feet + FVector(0.f, 0.f, 40.f + 45.f * i), 55.f - 10.f * i, 20, Violet, false, Life, 0, 4.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
		DrawDebugLine(World, Feet, Feet + FVector(0.f, 0.f, 170.f), Violet, false, Life, 0, 3.f);
		return;
	}

	// Lightning (낙뢰 · AP skills that strike a mark) - a jagged bolt from above onto the target
	if (Def != nullptr && Def->Power == ES1SkillPower::Ap)
	{
		const FColor Bolt(150, 190, 255);
		FVector Prev = At + FVector(0.f, 0.f, 700.f);
		const int32 Kinks = 5;
		for (int32 i = 1; i <= Kinks; i++)
		{
			const float T = static_cast<float>(i) / Kinks;
			FVector Next = FMath::Lerp(At + FVector(0.f, 0.f, 700.f), Feet, T);
			if (i < Kinks)
				Next += FVector(((i % 2) ? 1.f : -1.f) * 35.f, ((i % 3) ? -1.f : 1.f) * 25.f, 0.f);
			DrawDebugLine(World, Prev, Next, Bolt, false, Life, 0, 6.f);
			Prev = Next;
		}
		DrawDebugCircle(World, Feet, 90.f, 24, Bolt, false, Life, 0, 6.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
		return;
	}

	// Anything else - an impact star on the target (저 멀리 wall slam)
	const FColor Star(255, 200, 90);
	for (int32 i = 0; i < 4; i++)
	{
		const FVector Dir = FVector(1.f, 0.f, 0.f).RotateAngleAxis(45.f * i, FVector::UpVector);
		DrawDebugLine(World, At - Dir * 90.f, At + Dir * 90.f, Star, false, Life, 0, 6.f);
	}
	DrawDebugCircle(World, Feet, 110.f, 24, Star, false, Life, 0, 5.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
}

void AS1Player::RequestDash(const FVector& Direction, float DistCm, float SpeedCms, bool bIgnoresWalls, bool bThroughPawns)
{
	if (DistCm <= 0.f || SpeedCms <= 0.f)
		return;

	FVector Flat = Direction;
	Flat.Z = 0.0;

	if (Flat.Normalize() == false)
		return;

	// A push that lands mid-dash replaces it - give the capsule its Pawn response back first
	if (IsDashing())
		FinishDash();

	DashDirection = Flat;
	DashRemainingCm = DistCm;
	DashSpeedCms = SpeedCms;
	bDashIgnoresWalls = bIgnoresWalls;
	bDashThroughPawns = bThroughPawns;
	DashStartLocation = GetActorLocation();

	// 그림자 베기 - characters and monsters do not stop this dash; walls (WorldStatic) still do
	if (bDashThroughPawns)
	{
		if (UCapsuleComponent* Capsule = GetCapsuleComponent())
			Capsule->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
	}
}

void AS1Player::FinishDash()
{
	DashRemainingCm = 0.f;

	if (bDashThroughPawns)
	{
		bDashThroughPawns = false;
		if (UCapsuleComponent* Capsule = GetCapsuleComponent())
			Capsule->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
	}
}

void AS1Player::TickDash(float DeltaTime)
{
	if (DashRemainingCm <= 0.f)
		return;

	const float Step = FMath::Min(DashSpeedCms * DeltaTime, DashRemainingCm);

	if (bDashIgnoresWalls)
	{
		// 유령 신랑의 저주 (skill_effects.csv ignores_walls) - no sweep, so the capsule passes through walls. The server
		// skips its C_MOVE wall check for the same window (Room::ApplyMovement). Only the end point must be free.
		// The coast is the exception (island 2026-10-11): the server refuses a position outside it even then
		if (StopAtCoast(Step))
		{
			FinishDash();
			ResolveDashEndOutOfWalls(/*bForwardFirst*/ false);		// forward is over the coast
			return;
		}
		AddActorWorldOffset(DashDirection * Step, false, nullptr, ETeleportType::TeleportPhysics);
		DashRemainingCm -= Step;

		if (DashRemainingCm <= 0.f)
		{
			FinishDash();
			ResolveDashEndOutOfWalls();
		}
		return;
	}

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
		FinishDash();
		return;
	}

	DashRemainingCm -= Step;
	if (DashRemainingCm <= 0.f)
		FinishDash();
}

bool AS1Player::StopAtCoast(float StepCm)
{
	UCapsuleComponent* Capsule = GetCapsuleComponent();
	UWorld* World = GetWorld();
	if (Capsule == nullptr || World == nullptr)
		return false;

	// Every static hit along the step, not just the first - a house wall in front must not hide the coast behind it
	const FVector Start = GetActorLocation();
	const FCollisionShape Shape = FCollisionShape::MakeCapsule(Capsule->GetScaledCapsuleRadius(), Capsule->GetScaledCapsuleHalfHeight());
	FCollisionQueryParams Params(SCENE_QUERY_STAT(DashCoast), false, this);
	TArray<FHitResult> Hits;
	World->SweepMultiByObjectType(Hits, Start, Start + DashDirection * StepCm, FQuat::Identity, FCollisionObjectQueryParams(ECC_WorldStatic), Shape, Params);

	const FHitResult* First = nullptr;
	for (const FHitResult& Hit : Hits)
	{
		const UPrimitiveComponent* Component = Hit.GetComponent();
		if (Component != nullptr && Component->ComponentHasTag(S1LevelCoastTag) && Hit.bStartPenetrating == false
			&& (First == nullptr || Hit.Time < First->Time))
		{
			First = &Hit;
		}
	}
	if (First == nullptr)
		return false;

	SetActorLocation(First->Location, false, nullptr, ETeleportType::TeleportPhysics);
	UE_LOG(LogTemp, Log, TEXT("[DASH] wall-pass dash stopped at the coast after %.0f cm"), FVector::Dist2D(DashStartLocation, First->Location));
	return true;
}

void AS1Player::ResolveDashEndOutOfWalls(bool bForwardFirst)
{
	UCapsuleComponent* Capsule = GetCapsuleComponent();
	UWorld* World = GetWorld();
	if (Capsule == nullptr || World == nullptr)
		return;

	FCollisionQueryParams Params(SCENE_QUERY_STAT(DashEndOutOfWalls), false, this);
	const FCollisionShape Shape = FCollisionShape::MakeCapsule(Capsule->GetScaledCapsuleRadius(), Capsule->GetScaledCapsuleHalfHeight());
	const ECollisionChannel Channel = Capsule->GetCollisionObjectType();
	const FVector End = GetActorLocation();

	auto IsFree = [&](const FVector& At) { return World->OverlapBlockingTestByChannel(At, FQuat::Identity, Channel, Shape, Params) == false; };

	if (IsFree(End))
		return;

	// Forward first - the other side of the wall is where the dash was going. Then back toward the start (the spot in
	// front of the wall is always free). 25 cm steps: under the capsule radius, so no gap is skipped.
	constexpr float StepCm = 25.f;
	const float MaxForwardCm = bForwardFirst ? 400.f : 0.f;
	for (float Extra = StepCm; Extra <= MaxForwardCm; Extra += StepCm)
	{
		const FVector Probe = End + DashDirection * Extra;
		if (IsFree(Probe))
		{
			SetActorLocation(Probe, false, nullptr, ETeleportType::TeleportPhysics);
			UE_LOG(LogTemp, Warning, TEXT("[DASH] wall-pass end inside a wall - pushed forward %.0f cm"), Extra);
			return;
		}
	}

	const float BackCm = FVector::Dist2D(End, DashStartLocation);
	for (float Back = StepCm; Back <= BackCm; Back += StepCm)
	{
		const FVector Probe = End - DashDirection * Back;
		if (IsFree(Probe))
		{
			SetActorLocation(Probe, false, nullptr, ETeleportType::TeleportPhysics);
			UE_LOG(LogTemp, Warning, TEXT("[DASH] wall-pass end inside a wall - pulled back %.0f cm"), Back);
			return;
		}
	}

	SetActorLocation(DashStartLocation, false, nullptr, ETeleportType::TeleportPhysics);
	UE_LOG(LogTemp, Warning, TEXT("[DASH] wall-pass end inside a wall - no free spot, back to the start"));
}

bool AS1Player::CanAttack() const
{
	// Deliberately identical to CanTurn: if you can still aim, you can still swing.
	// Kept as its own function because the two answer different questions and the server
	// splits them the same way — combat-system.md Rule 5 is free to move one without
	// dragging the other along. The post-skill lock stops swings but not aiming (2026-10-15)
	return CanTurn() && (GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f) >= SkillLockUntil;
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

	FVector Location(Info.x(), Info.y(), Info.z() + GroundLiftCm);
	SetActorLocation(Location);

	DestInfo->CopyFrom(Info);
	ResetInterpolation(Info.state());
}

void AS1Player::SetAlly(bool bAlly, const FString& Name)
{
	bIsAlly = bAlly;
	AllyName = Name;

	if (bAlly && AllyDisc == nullptr && bIsMonster == false)
	{
		UStaticMesh* Cylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
		if (Cylinder != nullptr)
		{
			// The monster disc's recipe in party blue - a plain mesh component, so the dynamic tint works (an HISM's would not)
			AllyDisc = NewObject<UStaticMeshComponent>(this);
			AllyDisc->SetStaticMesh(Cylinder);
			AllyDisc->SetupAttachment(RootComponent);
			AllyDisc->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			AllyDisc->SetCastShadow(false);
			AllyDisc->SetRelativeLocation(FVector(0.f, 0.f, -GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() + 8.f));
			AllyDisc->SetRelativeScale3D(FVector(1.1f, 1.1f, 0.02f));
			if (UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")))
			{
				UMaterialInstanceDynamic* Tint = UMaterialInstanceDynamic::Create(Base, this);
				Tint->SetVectorParameterValue(TEXT("Color"), FLinearColor(FColor(0x39, 0x87, 0xE5)));
				AllyDisc->SetMaterial(0, Tint);
			}
			AllyDisc->RegisterComponent();
		}
	}

	if (AllyDisc != nullptr)
		AllyDisc->SetVisibility(bAlly);

	UE_LOG(LogTemp, Log, TEXT("[PARTY] ally %s id=%llu name=%s"), bAlly ? TEXT("on") : TEXT("off"),
		PlayerInfo != nullptr ? PlayerInfo->object_id() : 0ull, *Name);
}

void AS1Player::SetCreatureInfo(const Protocol::ObjectInfo& Info)
{
	// 0 = an older server that did not send it — keep the naked baseline
	if (Info.max_hp() > 0)
	{
		MaxHp = Info.max_hp();
		CurrentHp = Info.hp();
	}

	if (Info.creature_type() != Protocol::CREATE_TYPE_MONSTER || bIsMonster)
	{
		SetWeaponType(Info.weapon_type_id());		// stance from the spawn snapshot (anim, 2026-10-12)
		// ObjectInfo.betrayer is no longer used: the betrayer mark is the victims' own, kept by US1PartySubsystem (2026-10-13)
		return;
	}

	bIsMonster = true;
	MonsterTypeId = Info.monster_type_id();
	SetWeaponType(0);								// "monster" clips

	const FS1MonsterTypeDef* Def = FS1MonsterCatalog::Get().Find(MonsterTypeId);

	// Size from the server's own radius — the boss (110 cm) is the largest silhouette on the floor (art bible ⑩)
	if (Def != nullptr)
	{
		const float Scale = FS1MonsterCatalog::ScaleForRadius(Def->RadiusCm);
		SetActorScale3D(FVector(Scale));

		// Feet on the floor at any size (GroundLiftCm). SetPlayerInfo already placed the actor at the server Z
		GroundLiftCm = GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() * (Scale - 1.f);
		AddActorWorldOffset(FVector(0.f, 0.f, GroundLiftCm));
		InterpStartLocation = GetActorLocation();
	}

	// A teal disc under the feet. From the top-down camera this is what separates a monster from a player before
	// any mesh does — monsters share the player mesh until their art exists (monster-ai.md OQ-5)
	if (UStaticMesh* Cylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder")))
	{
		MonsterDisc = NewObject<UStaticMeshComponent>(this);
		MonsterDisc->SetStaticMesh(Cylinder);
		MonsterDisc->SetupAttachment(RootComponent);
		MonsterDisc->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		MonsterDisc->SetCastShadow(false);
		// 8 cm, not 2: a husk standing on the raised path strip lost its disc under it (2026-10-11 screenshot)
		MonsterDisc->SetRelativeLocation(FVector(0.f, 0.f, -GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() + 8.f));
		MonsterDisc->SetRelativeScale3D(FVector(1.0f, 1.0f, 0.02f));		// 100 cm cylinder → 1 m disc at player scale

		if (UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")))
		{
			UMaterialInstanceDynamic* Tint = UMaterialInstanceDynamic::Create(Base, this);
			Tint->SetVectorParameterValue(TEXT("Color"), FLinearColor(FColor(25, 158, 112)));
			MonsterDisc->SetMaterial(0, Tint);
		}
		MonsterDisc->RegisterComponent();
	}

	// Base-camp scarecrow - it should read as a training post, not a monster wearing a player (2026-10-17)
	if (Def != nullptr && Def->NameKey == TEXT("monster.scarecrow"))
		BuildScarecrowLook();

	// Painted by US1WorldLabelWidget in screen space with the UI font (the world text font has no Hangul)
	MonsterName = Def != nullptr ? FText::FromStringTable(TEXT("S1Items"), Def->NameKey) : FText::FromString(TEXT("?"));

	UE_LOG(LogTemp, Log, TEXT("[MON] spawn id=%llu type=%u hp=%d/%d"), Info.object_id(), MonsterTypeId, CurrentHp, MaxHp);
}

void AS1Player::BuildScarecrowLook()
{
	if (ScarecrowParts.Num() > 0)
		return;

	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UStaticMesh* Cylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (Cube == nullptr || Cylinder == nullptr || Sphere == nullptr)
		return;

	// The borrowed mesh goes - hit reactions are the damage numbers and the HP bar
	GetMesh()->SetVisibility(false, true);

	// Parts in the actor's unscaled space: feet at -half height, a 1.8 m figure
	const float Feet = -GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight();
	auto Part = [&](UStaticMesh* PartMesh, const FVector& At, const FVector& SizeCm, const FLinearColor& Color)
		{
			UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(this);
			C->SetStaticMesh(PartMesh);
			C->SetupAttachment(RootComponent);
			C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			C->SetRelativeLocation(FVector(At.X, At.Y, Feet + At.Z));
			C->SetRelativeScale3D(SizeCm / 100.f);
			if (Base != nullptr)
			{
				UMaterialInstanceDynamic* Tint = UMaterialInstanceDynamic::Create(Base, this);
				Tint->SetVectorParameterValue(TEXT("Color"), Color);
				C->SetMaterial(0, Tint);
			}
			C->RegisterComponent();
			ScarecrowParts.Add(C);
		};

	const FLinearColor Wood(0.34f, 0.22f, 0.12f);
	const FLinearColor Straw(0.78f, 0.64f, 0.30f);
	const FLinearColor Cloth(0.42f, 0.36f, 0.30f);
	Part(Cylinder, FVector(0.f, 0.f, 90.f), FVector(14.f, 14.f, 180.f), Wood);		// post
	Part(Cube, FVector(0.f, 0.f, 135.f), FVector(12.f, 130.f, 12.f), Wood);			// crossbar (arms out to the sides)
	Part(Cube, FVector(0.f, 0.f, 112.f), FVector(36.f, 50.f, 60.f), Cloth);			// sack body
	Part(Sphere, FVector(0.f, 0.f, 170.f), FVector(34.f, 34.f, 34.f), Straw);		// straw head
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
