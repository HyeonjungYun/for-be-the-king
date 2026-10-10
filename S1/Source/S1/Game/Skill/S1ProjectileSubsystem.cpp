// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/Skill/S1ProjectileSubsystem.h"

#include "DrawDebugHelpers.h"
#include "Engine/CollisionProfile.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Game/S1Player.h"
#include "Game/S1SkillTable.h"
#include "Game/Vision/S1VisionSubsystem.h"
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
	DestroyAllWallActors();
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
	Zones.Reset();
	DestroyAllWallActors();
}

void US1ProjectileSubsystem::HandleZones(const Protocol::S_ZONE& Pkt)
{
	const UWorld* World = GetWorld();
	const float Now = World ? World->GetTimeSeconds() : 0.f;

	for (const Protocol::ZoneInfo& Info : Pkt.spawned())
	{
		const FString Param = UTF8_TO_TCHAR(Info.param().c_str());

		// 지각변동 - a real box, not a ground circle
		if (Param == TEXT("wall"))
		{
			SpawnWallActor(Info);
			continue;
		}

		FS1ClientZone Z;
		Z.Id = Info.zone_id();
		Z.Pos = FVector(Info.x(), Info.y(), GroundZ(Info.caster_id()) - 50.f);
		Z.RadiusCm = Info.radius_cm();
		Z.Start = Now;
		Z.Until = Now + Info.duration_ms() / 1000.f;
		Z.Param = Param;
		Z.bFlash = (Param == TEXT("shock") || Param == TEXT("spin"));

		// Colour by what it does - poison green, fire orange, lightning blue, mist grey, arrows tan, water blue
		if (Z.Param == TEXT("poison"))				Z.Colour = FColor(110, 200, 60);
		else if (Z.Param == TEXT("blood"))			Z.Colour = FColor(150, 20, 20);
		else if (Z.Param.Contains(TEXT("fire")))	Z.Colour = FColor(255, 140, 40);
		else if (Z.Param == TEXT("lightning"))		Z.Colour = FColor(120, 160, 255);
		else if (Z.Param == TEXT("mist"))			Z.Colour = FColor(170, 170, 190);
		else if (Z.Param == TEXT("arrows"))			Z.Colour = FColor(220, 190, 120);
		else if (Z.Param == TEXT("puddle"))			Z.Colour = FColor(70, 150, 255);
		else if (Z.Param == TEXT("shock"))			Z.Colour = FColor(230, 170, 90);
		else if (Z.Param == TEXT("spin"))			Z.Colour = FColor(225, 225, 255);
		else										Z.Colour = ColourFor(Info.effect_id());

		Zones.Add(Z);
		UE_LOG(LogTemp, Log, TEXT("[ZONE] spawn id=%llu effect=%u r=%.0f %s for=%ums"), Z.Id, Info.effect_id(), Z.RadiusCm, *Z.Param, Info.duration_ms());
	}

	for (uint64 Id : Pkt.ended())
	{
		Zones.RemoveAll([Id](const FS1ClientZone& Z) { return Z.Id == Id; });
		DestroyWallActor(Id);
	}
}

void US1ProjectileSubsystem::SpawnWallActor(const Protocol::ZoneInfo& Info)
{
	UWorld* World = GetWorld();
	if (World == nullptr)
		return;

	const uint64 Id = Info.zone_id();
	const float HalfLength = Info.half_length() > 0.f ? Info.half_length() : Info.radius_cm();
	const float HalfThickness = FMath::Max(10.f, Info.half_thickness());
	const float FloorZ = GroundZ(Info.caster_id()) - 50.f;

	// The fog first - even if the mesh fails to load, sight must stop where the server's does
	if (US1VisionSubsystem* Vision = US1VisionSubsystem::Get())
		Vision->AddDynamicWall(Id, FVector2D(Info.x(), Info.y()), Info.yaw(), HalfLength, HalfThickness);

	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (Cube == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[WALL] /Engine/BasicShapes/Cube is missing - wall %llu has no mesh"), Id);
		return;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	const FVector Location(Info.x(), Info.y(), FloorZ + WallHeightCm * 0.5f);
	AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Location, FRotator(0.f, Info.yaw(), 0.f), Params);
	if (Actor == nullptr)
		return;

	// The engine cube is 100 cm on a side, pivot in the middle
	Actor->SetMobility(EComponentMobility::Movable);
	UStaticMeshComponent* Mesh = Actor->GetStaticMeshComponent();
	Mesh->SetStaticMesh(Cube);
	Mesh->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
	Actor->SetActorScale3D(FVector(HalfLength * 2.f / 100.f, HalfThickness * 2.f / 100.f, WallHeightCm / 100.f));

	// Light earth - BasicShapeMaterial exposes "Color". A dark brown read as a hole in the floor from the top-down camera
	if (UMaterialInstanceDynamic* Mid = Mesh->CreateAndSetMaterialInstanceDynamic(0))
		Mid->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.62f, 0.48f, 0.32f));

	WallActors.Add(Id, Actor);
	UE_LOG(LogTemp, Log, TEXT("[WALL] raise id=%llu at=(%.0f,%.0f) yaw=%.0f len=%.0f thick=%.0f"), Id, Info.x(), Info.y(), Info.yaw(), HalfLength * 2.f, HalfThickness * 2.f);
}

void US1ProjectileSubsystem::DestroyWallActor(uint64 ZoneId)
{
	if (US1VisionSubsystem* Vision = US1VisionSubsystem::Get())
		Vision->RemoveDynamicWall(ZoneId);

	TWeakObjectPtr<AActor> Actor;
	if (WallActors.RemoveAndCopyValue(ZoneId, Actor) == false)
		return;

	if (AActor* Alive = Actor.Get())
		Alive->Destroy();
	UE_LOG(LogTemp, Log, TEXT("[WALL] fall id=%llu"), ZoneId);
}

void US1ProjectileSubsystem::DestroyAllWallActors()
{
	TArray<uint64> Ids;
	WallActors.GetKeys(Ids);
	for (uint64 Id : Ids)
		DestroyWallActor(Id);
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
	if (World == nullptr || (Flying.Num() == 0 && Impacts.Num() == 0 && Zones.Num() == 0))
		return;

	const US1GameInstance* GameInstance = Cast<US1GameInstance>(GetGameInstance());
	const float Now = World->GetTimeSeconds();

	// Zones - the outline plus a slow inner pulse so a 5 s circle does not read as a static decal
	for (int32 i = Zones.Num() - 1; i >= 0; i--)
	{
		const FS1ClientZone& Z = Zones[i];

		// Shockwave · spin slash - one growing ring, gone at its own deadline (no end packet comes)
		if (Z.bFlash)
		{
			if (Now > Z.Until)
			{
				Zones.RemoveAtSwap(i);
				continue;
			}
			const float Span = FMath::Max(Z.Until - Z.Start, 0.05f);
			const float Grow = FMath::Clamp((Now - Z.Start) / Span, 0.f, 1.f);
			DrawDebugCircle(World, Z.Pos, Z.RadiusCm * (0.35f + 0.65f * Grow), 32, Z.Colour, false, -1.f, 0, 6.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
			continue;
		}

		if (Now > Z.Until + 1.f)
		{
			Zones.RemoveAtSwap(i);		// the end packet was lost - trim a second after the server's deadline
			continue;
		}

		DrawDebugCircle(World, Z.Pos, Z.RadiusCm, 32, Z.Colour, false, -1.f, 0, 4.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
		const float Pulse = FMath::Fmod(Now * 0.8f, 1.f);
		DrawDebugCircle(World, Z.Pos, Z.RadiusCm * Pulse, 24, Z.Colour, false, -1.f, 0, 1.5f, FVector(1, 0, 0), FVector(0, 1, 0), false);
	}

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
