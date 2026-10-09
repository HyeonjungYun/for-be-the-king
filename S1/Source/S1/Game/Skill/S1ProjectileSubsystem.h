// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "S1ProjectileSubsystem.generated.h"

namespace Protocol
{
	class S_PROJECTILE;
	class ProjectileInfo;
	class ProjectileEndInfo;
	class S_ZONE;
	class ZoneInfo;
}

/** A ground zone (stage 3) - drawn for its duration; the ticks arrive as S_DAMAGE / S_CC / S_STATUS. */
struct FS1ClientZone
{
	uint64 Id = 0;
	FVector Pos = FVector::ZeroVector;
	float RadiusCm = 0.f;
	float Until = 0.f;
	FColor Colour = FColor::White;
	FString Param;
};

/** One projectile in flight on this client. Flown locally from the launch packet; the server decides what it hits. */
struct FS1ClientProjectile
{
	uint64 Id = 0;
	uint64 CasterId = 0;
	uint32 EffectId = 0;		// 0 = basic attack (bow / staff)
	FVector Pos = FVector::ZeroVector;
	FVector Dir = FVector::ForwardVector;
	float SpeedCms = 0.f;
	float RemainingCm = 0.f;	// straight flight only
	float RadiusCm = 40.f;
	uint64 TargetId = 0;		// homing: follows this actor
	FColor Colour = FColor::White;
};

/**
 * Projectiles (skill roster stage 2, 2026-10-10). S_PROJECTILE.spawned starts one here; the client advances it every
 * frame (straight along dir, or toward the homing target's current position) and draws it. S_PROJECTILE.ended removes
 * it and leaves an impact pulse where the server said it stopped — on a target, a wall, or at max range.
 *
 * 🔴 Display only. Hits, damage and CC arrive through S_DAMAGE / S_CC / S_STATUS exactly as before; nothing here
 * predicts them. A projectile that visibly passes "through" a dodging player and then ends with EXPIRED is the
 * dodge working as designed (skill-system.md: projectiles can be sidestepped). 🔴 TEMP DrawDebug until the art budget.
 */
UCLASS()
class S1_API US1ProjectileSubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	static US1ProjectileSubsystem* Get();

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	void HandlePacket(const Protocol::S_PROJECTILE& Pkt);

	/** S_ZONE - ground zones live here too: same lifetime rules, same ground-plane drawing (stage 3). */
	void HandleZones(const Protocol::S_ZONE& Pkt);

	/** Everything in flight is dropped - leaving the dungeon, or a resync. */
	void Clear();

	int32 NumFlying() const { return Flying.Num(); }
	int32 NumZones() const { return Zones.Num(); }

	// ── FTickableGameObject ───────────────────────────────────────────────
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(US1ProjectileSubsystem, STATGROUP_Tickables); }
	virtual bool IsTickable() const override { return bInitialized; }
	virtual bool IsTickableInEditor() const override { return false; }

private:
	void OnSpawned(const Protocol::ProjectileInfo& Info);
	void OnEnded(const Protocol::ProjectileEndInfo& Info);

	/** Ground height for drawing - the caster's feet if we can see them, else the floor plane. */
	float GroundZ(uint64 ActorId) const;

	struct FImpact
	{
		FVector Pos;
		FColor Colour;
		float Until;
		bool bHit;
	};

	TArray<FS1ClientProjectile> Flying;
	TArray<FImpact> Impacts;
	TArray<FS1ClientZone> Zones;
	bool bInitialized = false;

	static constexpr float ImpactSeconds = 0.35f;
	static constexpr float TrailCm = 70.f;
};
