// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/Skill/S1ProjectileSubsystem.h"

#include "DrawDebugHelpers.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Game/S1Player.h"
#include "Game/S1SkillTable.h"
#include "Protocol.pb.h"
#include "S1GameInstance.h"

namespace
{
	TWeakObjectPtr<US1ProjectileSubsystem> GProjectiles;

	// Same palette as AS1Player::PlaySkillEffect - physical warm, magic cool, hybrid between. Basic attacks are white
	FColor ColourFor(uint32 EffectId)
	{
		if (EffectId == 0)
			return FColor(240, 240, 240);

		const FS1SkillDef* Def = FS1SkillTable::Find(EffectId);
		if (Def == nullptr)
			return FColor(180, 180, 180);

		switch (Def->Power)
		{
		case ES1SkillPower::Ap:		return FColor(90, 140, 255);
		case ES1SkillPower::Hybrid:	return FColor(200, 90, 230);
		case ES1SkillPower::None:	return FColor(90, 220, 190);
		default:					return FColor(230, 70, 70);
		}
	}
}

US1ProjectileSubsystem* US1ProjectileSubsystem::Get()
{
	return GProjectiles.Get();
}

void US1ProjectileSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	GProjectiles = this;
	bInitialized = true;
}

void US1ProjectileSubsystem::Deinitialize()
{
	bInitialized = false;
	Flying.Reset();
	Impacts.Reset();
	if (GProjectiles.Get() == this)
		GProjectiles = nullptr;
	Super::Deinitialize();
}

void US1ProjectileSubsystem::HandlePacket(const Protocol::S_PROJECTILE& Pkt)
{
	for (const Protocol::ProjectileInfo& Info : Pkt.spawned())
		OnSpawned(Info);

	for (const Protocol::ProjectileEndInfo& Info : Pkt.ended())
		OnEnded(Info);
}

void US1ProjectileSubsystem::Clear()
{
	Flying.Reset();
	Impacts.Reset();
}

float US1ProjectileSubsystem::GroundZ(uint64 ActorId) const
{
	const US1GameInstance* GameInstance = Cast<US1GameInstance>(GetGameInstance());
	if (GameInstance == nullptr)
		return 0.f;

	if (const TWeakObjectPtr<AS1Player>* Found = GameInstance->Players.Find(ActorId))
	{
		if (const AS1Player* Actor = Found->Get())
			return Actor->GetActorLocation().Z - 40.f;		// chest height - the bolt flies, it does not roll
	}
	return 60.f;
}

void US1ProjectileSubsystem::OnSpawned(const Protocol::ProjectileInfo& Info)
{
	FS1ClientProjectile P;
	P.Id = Info.projectile_id();
	P.CasterId = Info.caster_id();
	P.EffectId = Info.effect_id();
	P.Pos = FVector(Info.x(), Info.y(), GroundZ(Info.caster_id()));
	P.Dir = FVector(Info.dir_x(), Info.dir_y(), 0.f);
	if (P.Dir.Normalize() == false)
		P.Dir = FVector::ForwardVector;
	P.SpeedCms = Info.speed_cms();
	P.RemainingCm = Info.range_cm();
	P.RadiusCm = Info.radius_cm() > 0.f ? Info.radius_cm() : 40.f;
	P.TargetId = Info.target_id();
	P.Colour = ColourFor(P.EffectId);

	Flying.Add(P);

	UE_LOG(LogTemp, Log, TEXT("[PROJ] spawn id=%llu caster=%llu effect=%u target=%llu speed=%.0f range=%.0f"),
		P.Id, P.CasterId, P.EffectId, P.TargetId, P.SpeedCms, P.RemainingCm);
}

void US1ProjectileSubsystem::OnEnded(const Protocol::ProjectileEndInfo& Info)
{
	const UWorld* World = GetWorld();
	const float Now = World ? World->GetTimeSeconds() : 0.f;

	for (int32 i = 0; i < Flying.Num(); i++)
	{
		if (Flying[i].Id != Info.projectile_id())
			continue;

		FImpact Impact;
		Impact.Pos = FVector(Info.x(), Info.y(), Flying[i].Pos.Z);
		Impact.bHit = (Info.reason() == Protocol::PROJECTILE_END_HIT);
		Impact.Colour = Impact.bHit ? Flying[i].Colour : (Info.reason() == Protocol::PROJECTILE_END_WALL ? FColor(120, 120, 120) : FColor(70, 70, 70));
		Impact.Until = Now + ImpactSeconds;
		Impacts.Add(Impact);

		// How far the local flight drifted from where the server ended it - a few cm is a frame of lag, metres is a bug
		const float DriftCm = FVector::Dist2D(Flying[i].Pos, Impact.Pos);
		UE_LOG(LogTemp, Log, TEXT("[PROJ] end id=%llu reason=%d target=%llu drift=%.0fcm"),
			Info.projectile_id(), static_cast<int32>(Info.reason()), Info.target_id(), DriftCm);

		Flying.RemoveAtSwap(i);
		return;
	}
}

void US1ProjectileSubsystem::Tick(float DeltaTime)
{
	UWorld* World = GetWorld();
	if (World == nullptr || (Flying.Num() == 0 && Impacts.Num() == 0))
		return;

	const US1GameInstance* GameInstance = Cast<US1GameInstance>(GetGameInstance());
	const float Now = World->GetTimeSeconds();

	for (FS1ClientProjectile& P : Flying)
	{
		// Homing: re-aim at the target every frame - the server does the same each flush
		if (P.TargetId != 0 && GameInstance != nullptr)
		{
			if (const TWeakObjectPtr<AS1Player>* Found = GameInstance->Players.Find(P.TargetId))
			{
				if (const AS1Player* Target = Found->Get())
				{
					FVector To = Target->GetActorLocation() - P.Pos;
					To.Z = 0.f;
					if (To.Normalize())
						P.Dir = To;
				}
			}
		}

		float Step = P.SpeedCms * DeltaTime;
		if (P.TargetId == 0)
		{
			// Straight: never draw it beyond the range the server gave - the end packet may lag a frame
			Step = FMath::Min(Step, P.RemainingCm);
			P.RemainingCm -= Step;
		}
		P.Pos += P.Dir * Step;

		DrawDebugSphere(World, P.Pos, P.RadiusCm * 0.5f, 8, P.Colour, false, -1.f, 0, 2.f);
		DrawDebugLine(World, P.Pos - P.Dir * TrailCm, P.Pos, P.Colour, false, -1.f, 0, 3.f);
	}

	for (int32 i = Impacts.Num() - 1; i >= 0; i--)
	{
		const FImpact& Impact = Impacts[i];
		if (Now >= Impact.Until)
		{
			Impacts.RemoveAtSwap(i);
			continue;
		}

		// A hit flares, a wall or a miss just fades
		const float Alpha = (Impact.Until - Now) / ImpactSeconds;
		const float Radius = Impact.bHit ? (60.f + 60.f * (1.f - Alpha)) : 30.f;
		DrawDebugCircle(World, Impact.Pos, Radius, 16, Impact.Colour, false, -1.f, 0, Impact.bHit ? 5.f : 2.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
	}
}
