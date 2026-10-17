// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "Game/Skill/S1SwordWave.h"
#include "S1ProjectileSubsystem.generated.h"

class UMaterialInterface;
class UMaterialInstanceDynamic;

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
	float Start = 0.f;
	float Until = 0.f;
	FColor Colour = FColor::White;
	FString Param;
	bool bFlash = false;		// shock · spin · slash (2026-10-11) - grows once; the server sends no end packet for these
	bool bExpires = false;		// ping - no end packet either, gone at Until (2026-10-11)
	float AngleDeg = 0.f;		// cone zone / slash - full angle, 0 = circle (네불라 아르카나 · 일보일검)
	FVector Dir = FVector::ForwardVector;
	uint64 FollowId = 0;		// ping - rides on this actor while this client knows it (맹수의 탐색)
};

/** 종이매 (1135) flying to the nearest enemy and vanishing over its head - display only (2026-10-11). */
struct FS1HawkFlight
{
	FVector From = FVector::ZeroVector;
	FVector To = FVector::ZeroVector;
	uint64 TargetId = 0;
	float Start = 0.f;
	float Duration = 0.9f;
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

	// skill_vfx.csv look (2026-10-15) - a mesh that flies with the projectile instead of the debug bolt
	TWeakObjectPtr<AActor> Visual;
	TWeakObjectPtr<UMaterialInstanceDynamic> VisualMid;
	float VisualLiftCm = 0.f;	// Pos is chest height; the wave stands on the ground
	float Born = 0.f;
	float PreviewUntil = 0.f;	// DebugVfx - no server end packet comes; it fades at this time
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

	/** 종이매 - a hawk flies from the caster to the nearest enemy (S_SKILL_HIT 1135 aimed far away, owner only). */
	void AddHawkFlight(const FVector& From, const FVector& To, uint64 TargetId);

	/**
	 * Show a skill's projectile look without a cast - from Chest along Dir at SpeedCms for Seconds, then it fades (no server,
	 * nothing hits). The DebugVfx console command; tuning skill_vfx.csv by eye. False = no look for this skill.
	 */
	bool PreviewLook(uint32 EffectId, const FVector& Chest, const FVector& Dir, float SpeedCms, float Seconds);

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

	/**
	 * 지각변동 (S_ZONE param "wall", 2026-10-11) - a box that blocks our own movement (the server's WallMap has the same
	 * box, so C_MOVE through it is rejected anyway) and the fog's sight lines. Gone when the zone ends.
	 */
	void SpawnWallActor(const Protocol::ZoneInfo& Info);
	void DestroyWallActor(uint64 ZoneId);
	void DestroyAllWallActors();

	/** Cone outline (two edges + arc) on the ground - zones, slash flashes. */
	static void DrawCone(UWorld* World, const FVector& Apex, const FVector& Dir, float RadiusCm, float AngleDeg, const FColor& Colour, float Thickness);

	/** Hawks: circling a followed enemy (S_VISION source kind "hawk") and flights to a far one. */
	void TickHawks(UWorld* World, float Now);
	TArray<FS1HawkFlight> HawkFlights;

	// ── Projectile looks (skill_vfx.csv, 2026-10-15) ─────────────────────────
	/** 검의 찬양 - an upright crescent (procedural mesh + M_S1SwordWave) riding on the projectile. False = draw the stock bolt. */
	bool SpawnWaveVisual(FS1ClientProjectile& P, const FS1ProjectileLook& Look, float Now);
	void PlaceWaveVisual(const FS1ClientProjectile& P, float Now) const;
	/** The wave flares and fades where the server ended the projectile, then its actor goes. */
	void StartWaveFade(const FS1ClientProjectile& P, float Now);
	void TickFadingWaves(float Now);
	void DestroyAllWaves();

	struct FFadingWave
	{
		TWeakObjectPtr<AActor> Actor;
		TWeakObjectPtr<UMaterialInstanceDynamic> Mid;
		float Start = 0.f;
		float Until = 0.f;
	};
	TArray<FFadingWave> FadingWaves;
	TMap<uint32, FS1ProjectileLook> Looks;		// effect id → look

	UPROPERTY()
	TObjectPtr<UMaterialInterface> WaveMaterial;

	static constexpr float WaveGrowSeconds = 0.08f;		// the blade rises out of the swing
	static constexpr float WaveFadeSeconds = 0.18f;		// M_S1SwordWave "Fade" 1 → 0

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
	TMap<uint64, TWeakObjectPtr<AActor>> WallActors;
	bool bInitialized = false;

	static constexpr float ImpactSeconds = 0.35f;
	static constexpr float TrailCm = 70.f;
	static constexpr float WallHeightCm = 250.f;		// taller than a character, lower than the house walls
};
