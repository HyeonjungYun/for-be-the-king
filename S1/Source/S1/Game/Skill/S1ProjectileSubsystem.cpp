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
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ProceduralMeshComponent.h"
#include "Game/S1Player.h"
#include "Game/S1SkillTable.h"
#include "Game/Vision/S1VisionSubsystem.h"
#include "HAL/PlatformTime.h"
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

	// Projectile looks - optional: without the table every projectile keeps the stock drawing
	const FString Path = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Data"), TEXT("skill_vfx.csv"));
	FString Csv;
	FString Error;
	if (FFileHelper::LoadFileToString(Csv, *Path) == false)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VFX] %s missing - projectiles use the stock drawing"), *Path);
	}
	else if (S1SwordWave::ParseLooks(Csv, Looks, Error) == false)
	{
		UE_LOG(LogTemp, Error, TEXT("[VFX] %s"), *Error);
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("[VFX] projectile looks loaded - %d"), Looks.Num());
	}
}

void US1ProjectileSubsystem::Deinitialize()
{
	bInitialized = false;
	DestroyAllWaves();
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
	DestroyAllWaves();
	Flying.Reset();
	Impacts.Reset();
	Zones.Reset();
	HawkFlights.Reset();
	DestroyAllWallActors();
}

void US1ProjectileSubsystem::AddHawkFlight(const FVector& From, const FVector& To, uint64 TargetId)
{
	const UWorld* World = GetWorld();
	FS1HawkFlight Flight;
	Flight.From = From;
	Flight.To = To;
	Flight.TargetId = TargetId;
	Flight.Start = World ? World->GetTimeSeconds() : 0.f;
	// About 25 m/s, never shorter than half a second - long enough to follow with the eye
	Flight.Duration = FMath::Clamp(static_cast<float>(FVector::Dist2D(From, To)) / 2500.f, 0.5f, 1.6f);
	HawkFlights.Add(Flight);
	UE_LOG(LogTemp, Log, TEXT("[HAWK] fly to (%.0f, %.0f) target=%llu in %.2fs"), To.X, To.Y, TargetId, Flight.Duration);
}

void US1ProjectileSubsystem::DrawCone(UWorld* World, const FVector& Apex, const FVector& Dir, float RadiusCm, float AngleDeg, const FColor& Colour, float Thickness)
{
	const float Half = AngleDeg * 0.5f;
	const FVector Left = Dir.RotateAngleAxis(-Half, FVector::UpVector);
	const FVector Right = Dir.RotateAngleAxis(Half, FVector::UpVector);
	DrawDebugLine(World, Apex, Apex + Left * RadiusCm, Colour, false, -1.f, 0, Thickness);
	DrawDebugLine(World, Apex, Apex + Right * RadiusCm, Colour, false, -1.f, 0, Thickness);

	const int32 Segments = FMath::Max(6, FMath::RoundToInt(AngleDeg / 8.f));
	FVector Prev = Apex + Left * RadiusCm;
	for (int32 i = 1; i <= Segments; i++)
	{
		const FVector Next = Apex + Dir.RotateAngleAxis(-Half + AngleDeg * i / Segments, FVector::UpVector) * RadiusCm;
		DrawDebugLine(World, Prev, Next, Colour, false, -1.f, 0, Thickness);
		Prev = Next;
	}
}

void US1ProjectileSubsystem::TickHawks(UWorld* World, float Now)
{
	const US1GameInstance* GameInstance = Cast<US1GameInstance>(GetGameInstance());
	if (GameInstance == nullptr)
		return;

	// A small paper bird: two wings in a V around a body dot, banking with its heading
	auto DrawHawk = [World](const FVector& At, const FVector& Heading)
	{
		const FColor Paper(245, 240, 225);
		const FVector Fwd = Heading.GetSafeNormal2D();
		const FVector Side(-Fwd.Y, Fwd.X, 0.f);
		DrawDebugLine(World, At, At - Fwd * 35.f + Side * 55.f, Paper, false, -1.f, 0, 5.f);
		DrawDebugLine(World, At, At - Fwd * 35.f - Side * 55.f, Paper, false, -1.f, 0, 5.f);
		DrawDebugSphere(World, At, 12.f, 6, Paper, false, -1.f, 0, 3.f);
	};

	// Circling (S_VISION source kind hawk - follows the enemy it found)
	if (const US1VisionSubsystem* Vision = US1VisionSubsystem::Get())
	{
		const double Wall = FPlatformTime::Seconds();
		for (const FS1VisionSource& Source : Vision->GetSources())
		{
			if (Source.Kind != TEXT("hawk") || Source.FollowId == 0 || (Source.Until > 0.0 && Wall >= Source.Until))
				continue;

			const TWeakObjectPtr<AS1Player>* Found = GameInstance->Players.Find(Source.FollowId);
			const AS1Player* Prey = Found != nullptr ? Found->Get() : nullptr;
			if (Prey == nullptr)
				continue;

			constexpr float OrbitCm = 160.f;
			const float Angle = Now * 2.4f;
			const FVector Centre = Prey->GetActorLocation() + FVector(0.f, 0.f, 230.f);
			const FVector At = Centre + FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0.f) * OrbitCm;
			const FVector Heading(-FMath::Sin(Angle), FMath::Cos(Angle), 0.f);
			DrawHawk(At, Heading);
		}
	}

	// Flights to a far enemy - vanish over its head with a small flash
	for (int32 i = HawkFlights.Num() - 1; i >= 0; i--)
	{
		const FS1HawkFlight& F = HawkFlights[i];
		const float T = (Now - F.Start) / F.Duration;
		if (T >= 1.25f)
		{
			HawkFlights.RemoveAtSwap(i);
			continue;
		}

		FVector To = F.To;
		if (const TWeakObjectPtr<AS1Player>* Found = GameInstance->Players.Find(F.TargetId))
		{
			if (const AS1Player* Prey = Found->Get())
				To = Prey->GetActorLocation();
		}

		const FVector From = F.From + FVector(0.f, 0.f, 200.f);
		const FVector Above = To + FVector(0.f, 0.f, 230.f);
		if (T < 1.f)
		{
			DrawHawk(FMath::Lerp(From, Above, T), Above - From);
		}
		else
		{
			DrawDebugCircle(World, Above, 60.f + 200.f * (T - 1.f), 20, FColor(245, 240, 225), false, -1.f, 0, 3.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
		}
	}
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
		// A short visual-only zone (shock · spin · slash, and the 3rd-roster element bursts - 플럭스 · 스쿠툼, 2026-10-11) is one growing ring
		Z.bFlash = (Param == TEXT("shock") || Param == TEXT("spin") || Param == TEXT("slash") || Info.duration_ms() <= 600);
		Z.bExpires = (Param == TEXT("ping"));
		Z.AngleDeg = Info.angle_deg();
		Z.Dir = FVector(1.f, 0.f, 0.f).RotateAngleAxis(Info.yaw(), FVector::UpVector);
		Z.FollowId = Info.follow_id();

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
		else if (Z.Param == TEXT("slash"))			Z.Colour = FColor(225, 225, 255);
		else if (Z.Param == TEXT("ping"))			Z.Colour = FColor(255, 70, 60);
		// 3rd roster (2026-10-11)
		else if (Z.Param == TEXT("frost"))			Z.Colour = FColor(150, 220, 255);
		else if (Z.Param == TEXT("gravity"))		Z.Colour = FColor(150, 80, 220);
		else if (Z.Param == TEXT("smoke"))			Z.Colour = FColor(140, 140, 140);
		else if (Z.Param == TEXT("caltrops"))		Z.Colour = FColor(205, 205, 205);
		else if (Z.Param == TEXT("portal"))			Z.Colour = FColor(180, 110, 255);
		else if (Z.Param == TEXT("spear"))			Z.Colour = FColor(255, 200, 120);
		else if (Z.Param == TEXT("telegraph"))		Z.Colour = FColor(255, 90, 60);
		else if (Z.Param == TEXT("lumen"))			Z.Colour = FColor(255, 245, 160);
		else if (Z.Param == TEXT("footprint"))		Z.Colour = FColor(175, 140, 95);
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
	if (const FS1ProjectileLook* Look = Looks.Find(P.EffectId))
		SpawnWaveVisual(P, *Look, GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f);

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

		// A wave finishes where the server stopped it, then flares out
		if (Flying[i].Visual.IsValid())
		{
			Flying[i].Pos = Impact.Pos;
			StartWaveFade(Flying[i], Now);
		}

		Flying.RemoveAtSwap(i);
		return;
	}
}

void US1ProjectileSubsystem::Tick(float DeltaTime)
{
	UWorld* World = GetWorld();
	if (World == nullptr)
		return;

	const US1GameInstance* GameInstance = Cast<US1GameInstance>(GetGameInstance());
	const float Now = World->GetTimeSeconds();

	TickHawks(World, Now);
	TickFadingWaves(Now);

	if (Flying.Num() == 0 && Impacts.Num() == 0 && Zones.Num() == 0)
		return;

	// Zones - the outline plus a slow inner pulse so a 5 s circle does not read as a static decal
	for (int32 i = Zones.Num() - 1; i >= 0; i--)
	{
		FS1ClientZone& Z = Zones[i];

		// Shockwave · spin slash · dash slash - one growing ring (or cone), gone at its own deadline (no end packet comes)
		if (Z.bFlash)
		{
			if (Now > Z.Until)
			{
				Zones.RemoveAtSwap(i);
				continue;
			}
			const float Span = FMath::Max(Z.Until - Z.Start, 0.05f);
			const float Grow = FMath::Clamp((Now - Z.Start) / Span, 0.f, 1.f);
			if (Z.AngleDeg > 0.f)
				DrawCone(World, Z.Pos, Z.Dir, Z.RadiusCm * (0.5f + 0.5f * Grow), Z.AngleDeg, Z.Colour, 6.f);
			else
				DrawDebugCircle(World, Z.Pos, Z.RadiusCm * (0.35f + 0.65f * Grow), 32, Z.Colour, false, -1.f, 0, 6.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
			continue;
		}

		// 맹수의 탐색 ping - a pulsing red mark on the enemy, riding on it while this client knows it (2026-10-11)
		if (Z.bExpires)
		{
			if (Now > Z.Until)
			{
				Zones.RemoveAtSwap(i);
				continue;
			}
			if (Z.FollowId != 0 && GameInstance != nullptr)
			{
				if (const TWeakObjectPtr<AS1Player>* Found = GameInstance->Players.Find(Z.FollowId))
				{
					if (const AS1Player* Actor = Found->Get())
						Z.Pos = Actor->GetActorLocation() - FVector(0.f, 0.f, 90.f);
				}
			}
			const float Beat = FMath::Fmod((Now - Z.Start) * 1.5f, 1.f);
			DrawDebugCircle(World, Z.Pos, Z.RadiusCm, 24, Z.Colour, false, -1.f, 0, 6.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
			DrawDebugCircle(World, Z.Pos, Z.RadiusCm * (1.f + 1.5f * Beat), 24, Z.Colour, false, -1.f, 0, 2.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
			DrawDebugLine(World, Z.Pos, Z.Pos + FVector(0.f, 0.f, 260.f), Z.Colour, false, -1.f, 0, 3.f);
			continue;
		}

		// 3rd-roster marks with no end packet of their own (2026-10-11) - gone exactly at their deadline
		const bool bTimedMark = (Z.Param == TEXT("telegraph") || Z.Param == TEXT("footprint") || Z.Param == TEXT("lumen"));
		if (bTimedMark && Now > Z.Until)
		{
			Zones.RemoveAtSwap(i);
			continue;
		}

		if (Now > Z.Until + 1.f)
		{
			Zones.RemoveAtSwap(i);		// the end packet was lost - trim a second after the server's deadline
			continue;
		}

		// 시길룸 - where it will burst, filling up until it does
		if (Z.Param == TEXT("telegraph"))
		{
			const float Span = FMath::Max(Z.Until - Z.Start, 0.05f);
			const float Fill = FMath::Clamp((Now - Z.Start) / Span, 0.f, 1.f);
			DrawDebugCircle(World, Z.Pos, Z.RadiusCm, 40, Z.Colour, false, -1.f, 0, 6.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
			DrawDebugCircle(World, Z.Pos, FMath::Max(Z.RadiusCm * Fill, 5.f), 32, Z.Colour, false, -1.f, 0, 3.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
			continue;
		}

		// 발자국 추적 - a small print, toed in the direction it was walking
		if (Z.Param == TEXT("footprint"))
		{
			DrawDebugCircle(World, Z.Pos, Z.RadiusCm, 10, Z.Colour, false, -1.f, 0, 3.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
			DrawDebugLine(World, Z.Pos, Z.Pos + Z.Dir * (Z.RadiusCm * 1.8f), Z.Colour, false, -1.f, 0, 3.f);
			continue;
		}

		// 루멘 - an orb hanging in the air over the spot it lights
		if (Z.Param == TEXT("lumen"))
		{
			const FVector Orb = Z.Pos + FVector(0.f, 0.f, 200.f);
			DrawDebugSphere(World, Orb, Z.RadiusCm, 12, Z.Colour, false, -1.f, 0, 2.f);
			DrawDebugLine(World, Z.Pos, Orb, Z.Colour, false, -1.f, 0, 1.f);
			continue;
		}

		// 포르타 문 · 비창 자리 - the spot the second press goes to: a ring and a post you can find across the screen
		if (Z.Param == TEXT("portal") || Z.Param == TEXT("spear"))
		{
			const float Beat = FMath::Fmod(Now * 1.2f, 1.f);
			DrawDebugCircle(World, Z.Pos, Z.RadiusCm, 24, Z.Colour, false, -1.f, 0, 5.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
			DrawDebugCircle(World, Z.Pos, Z.RadiusCm * (0.4f + 0.6f * Beat), 16, Z.Colour, false, -1.f, 0, 2.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
			DrawDebugLine(World, Z.Pos, Z.Pos + FVector(0.f, 0.f, 220.f), Z.Colour, false, -1.f, 0, 4.f);
			continue;
		}

		// 그라비타스 - rings falling inward
		if (Z.Param == TEXT("gravity"))
		{
			const float In = 1.f - FMath::Fmod(Now * 0.9f, 1.f);
			DrawDebugCircle(World, Z.Pos, Z.RadiusCm, 32, Z.Colour, false, -1.f, 0, 5.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
			DrawDebugCircle(World, Z.Pos, FMath::Max(Z.RadiusCm * In, 10.f), 24, Z.Colour, false, -1.f, 0, 2.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
			continue;
		}

		// 연막 - thick grey, no pulse: it is a cloud, not a timer
		if (Z.Param == TEXT("smoke"))
		{
			for (int32 k = 0; k < 4; k++)
				DrawDebugCircle(World, Z.Pos + FVector(0.f, 0.f, 30.f * k), Z.RadiusCm * (1.f - 0.15f * k), 28, Z.Colour, false, -1.f, 0, 8.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
			continue;
		}

		// 마름쇠 - a ring of little crosses
		if (Z.Param == TEXT("caltrops"))
		{
			DrawDebugCircle(World, Z.Pos, Z.RadiusCm, 20, Z.Colour, false, -1.f, 0, 2.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
			for (int32 k = 0; k < 6; k++)
			{
				const FVector At = Z.Pos + FVector(1.f, 0.f, 0.f).RotateAngleAxis(60.f * k + 15.f, FVector::UpVector) * (Z.RadiusCm * 0.55f);
				DrawDebugLine(World, At + FVector(-12.f, -12.f, 0.f), At + FVector(12.f, 12.f, 0.f), Z.Colour, false, -1.f, 0, 3.f);
				DrawDebugLine(World, At + FVector(-12.f, 12.f, 0.f), At + FVector(12.f, -12.f, 0.f), Z.Colour, false, -1.f, 0, 3.f);
			}
			continue;
		}

		const float Pulse = FMath::Fmod(Now * 0.8f, 1.f);
		if (Z.AngleDeg > 0.f)
		{
			// Cone zone (네불라 아르카나) - the outline plus an arc sweeping outward
			DrawCone(World, Z.Pos, Z.Dir, Z.RadiusCm, Z.AngleDeg, Z.Colour, 4.f);
			DrawCone(World, Z.Pos, Z.Dir, Z.RadiusCm * FMath::Max(Pulse, 0.05f), Z.AngleDeg, Z.Colour, 1.5f);
			continue;
		}

		DrawDebugCircle(World, Z.Pos, Z.RadiusCm, 32, Z.Colour, false, -1.f, 0, 4.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
		DrawDebugCircle(World, Z.Pos, Z.RadiusCm * Pulse, 24, Z.Colour, false, -1.f, 0, 1.5f, FVector(1, 0, 0), FVector(0, 1, 0), false);
	}

	// DebugVfx previews end on their own clock - no server packet comes for them
	for (int32 i = Flying.Num() - 1; i >= 0; i--)
	{
		if (Flying[i].PreviewUntil > 0.f && Now >= Flying[i].PreviewUntil)
		{
			StartWaveFade(Flying[i], Now);
			Flying.RemoveAtSwap(i);
		}
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

		if (P.Visual.IsValid())
		{
			PlaceWaveVisual(P, Now);
			continue;
		}

		// A wide projectile is a blade of force, not a bolt - a crescent across its path (검의 함성 1112 반원 검기, 2026-10-11)
		if (P.RadiusCm >= 90.f)
		{
			constexpr int32 Segments = 10;
			constexpr float SweepDeg = 150.f;
			const FVector Centre = P.Pos - P.Dir * (P.RadiusCm * 0.6f);
			FVector Prev = Centre + P.Dir.RotateAngleAxis(-SweepDeg * 0.5f, FVector::UpVector) * P.RadiusCm;
			for (int32 s = 1; s <= Segments; s++)
			{
				const FVector Next = Centre + P.Dir.RotateAngleAxis(-SweepDeg * 0.5f + SweepDeg * s / Segments, FVector::UpVector) * P.RadiusCm;
				DrawDebugLine(World, Prev, Next, P.Colour, false, -1.f, 0, 7.f);
				Prev = Next;
			}
			continue;
		}

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

// ── Projectile looks (skill_vfx.csv, 2026-10-15) ─────────────────────────────

bool US1ProjectileSubsystem::SpawnWaveVisual(FS1ClientProjectile& P, const FS1ProjectileLook& Look, float Now)
{
	UWorld* World = GetWorld();
	if (World == nullptr)
		return false;

	if (WaveMaterial == nullptr)
		WaveMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/FX/M_S1SwordWave.M_S1SwordWave"));
	if (WaveMaterial == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VFX] /Game/FX/M_S1SwordWave is missing (tools/fx/make_sword_wave_material.py) - effect %u keeps the stock drawing"), P.EffectId);
		return false;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
	if (Actor == nullptr)
		return false;

	UProceduralMeshComponent* Mesh = NewObject<UProceduralMeshComponent>(Actor, TEXT("SwordWave"));
	Actor->SetRootComponent(Mesh);
	Mesh->RegisterComponent();
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCastShadow(false);

	const FS1SwordWaveMesh Built = S1SwordWave::BuildCrescent(Look);
	Mesh->CreateMeshSection_LinearColor(0, Built.Vertices, Built.Triangles, TArray<FVector>(), TArray<FVector2D>(), Built.Colours, TArray<FProcMeshTangent>(), false);
	UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(WaveMaterial, Mesh);
	Mesh->SetMaterial(0, Mid != nullptr ? static_cast<UMaterialInterface*>(Mid) : WaveMaterial.Get());

	P.Visual = Actor;
	P.VisualMid = Mid;
	// Pos is feet + 50 (GroundZ). Sink 0 = the bottom tip skims the floor; sink 50 = the middle sits on it and the floor hides the rest
	P.VisualLiftCm = Look.HeightCm * (0.5f - Look.SinkPct / 100.f) + 5.f - 50.f;
	P.Born = Now;
	PlaceWaveVisual(P, Now);
	UE_LOG(LogTemp, Log, TEXT("[VFX] wave id=%llu effect=%u kind=%s h=%.0f"), P.Id, P.EffectId, *Look.Kind.ToString(), Look.HeightCm);
	return true;
}

void US1ProjectileSubsystem::PlaceWaveVisual(const FS1ClientProjectile& P, float Now) const
{
	AActor* Actor = P.Visual.Get();
	if (Actor == nullptr)
		return;
	// Edge first, standing up: local X = flight direction, Z = up. It rises out of the swing over the first frames
	const float Grow = FMath::Clamp((Now - P.Born) / WaveGrowSeconds, 0.f, 1.f);
	Actor->SetActorLocationAndRotation(P.Pos + FVector(0.f, 0.f, P.VisualLiftCm), FRotator(0.f, P.Dir.Rotation().Yaw, 0.f));
	Actor->SetActorScale3D(FVector(1.f, 1.f, 0.5f + 0.5f * Grow));
}

bool US1ProjectileSubsystem::PreviewLook(uint32 EffectId, const FVector& Chest, const FVector& Dir, float SpeedCms, float Seconds)
{
	const FS1ProjectileLook* Look = Looks.Find(EffectId);
	if (Look == nullptr)
		return false;
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	FS1ClientProjectile P;
	P.EffectId = EffectId;
	P.Pos = Chest;
	P.Dir = FVector(Dir.X, Dir.Y, 0.f).GetSafeNormal();
	if (P.Dir.IsNearlyZero())
		P.Dir = FVector::ForwardVector;
	P.SpeedCms = SpeedCms;
	P.RemainingCm = SpeedCms * Seconds;
	P.Colour = ColourFor(EffectId);
	P.PreviewUntil = Now + Seconds;
	if (SpawnWaveVisual(P, *Look, Now) == false)
		return false;
	Flying.Add(P);
	return true;
}

void US1ProjectileSubsystem::StartWaveFade(const FS1ClientProjectile& P, float Now)
{
	if (P.Visual.IsValid() == false)
		return;
	PlaceWaveVisual(P, Now);
	FFadingWave& Fade = FadingWaves.AddDefaulted_GetRef();
	Fade.Actor = P.Visual;
	Fade.Mid = P.VisualMid;
	Fade.Start = Now;
	Fade.Until = Now + WaveFadeSeconds;
}

void US1ProjectileSubsystem::TickFadingWaves(float Now)
{
	for (int32 i = FadingWaves.Num() - 1; i >= 0; i--)
	{
		FFadingWave& Wave = FadingWaves[i];
		AActor* Actor = Wave.Actor.Get();
		if (Actor == nullptr || Now >= Wave.Until)
		{
			if (Actor != nullptr)
				Actor->Destroy();
			FadingWaves.RemoveAtSwap(i);
			continue;
		}
		// Flare out: a little bigger, then gone
		const float Left = (Wave.Until - Now) / FMath::Max(Wave.Until - Wave.Start, 0.01f);
		Actor->SetActorScale3D(FVector(1.f + 0.4f * (1.f - Left), 1.f, 1.f + 0.25f * (1.f - Left)));
		if (UMaterialInstanceDynamic* Mid = Wave.Mid.Get())
			Mid->SetScalarParameterValue(TEXT("Fade"), Left);
	}
}

void US1ProjectileSubsystem::DestroyAllWaves()
{
	for (FS1ClientProjectile& P : Flying)
	{
		if (AActor* Actor = P.Visual.Get())
			Actor->Destroy();
		P.Visual.Reset();
	}
	for (FFadingWave& Wave : FadingWaves)
	{
		if (AActor* Actor = Wave.Actor.Get())
			Actor->Destroy();
	}
	FadingWaves.Reset();
}