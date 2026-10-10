// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "S1VisionSubsystem.generated.h"

namespace Protocol
{
	class S_VISION;
}

/** One extra vision source the server granted this player (shadow clone · paper hawk · beast search). */
struct FS1VisionSource
{
	uint64 SourceId = 0;
	uint64 FollowId = 0;		// 0 = fixed point; otherwise the actor it rides on (spawned on this client - the source sees its own centre)
	FVector2D Pos = FVector2D::ZeroVector;
	float RadiusCm = 0.f;
	double Until = 0.0;			// FPlatformTime seconds, 0 = while the followed actor exists
	bool bNeedsLos = true;
	bool bSeesStealth = false;
};

/**
 * Fog of war, client half (design/gdd/fog-of-war.md, 2026-10-11).
 *
 * 🔴 The server decides what this client may know — creatures outside our sight are simply never sent (S_SPAWN /
 * S_DESPAWN per viewer). This subsystem only *draws* the fog: S_VISION tells it our radius (blind and 천리안 applied)
 * and any extra sources; the walls are the server's own 2D boxes (Server/Data/walls.csv, cover excluded - it does not
 * block sight), so the dark edge lands where the server stops sending. US1FogOfWarWidget paints it.
 *
 * Nothing is drawn until the first S_VISION arrives — with FBTK_FOW=0 on the server the client shows no fog.
 */
UCLASS()
class S1_API US1VisionSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	static US1VisionSubsystem* Get();

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	void HandleVision(const Protocol::S_VISION& Pkt);

	/** Leaving the dungeon — no fog in base camp, camera back to normal. */
	void Clear();

	bool IsActive() const { return RadiusCm > 0.f; }
	float GetRadiusCm() const { return RadiusCm; }
	float GetCameraScale() const { return CameraScale; }
	const TArray<FS1VisionSource>& GetSources() const { return Sources; }

	/** Loads the floor's sight-blocking walls once (walls.csv rows of that floor, cover skipped). False = no file / no walls. */
	bool EnsureWalls(uint32 FloorId);

	/**
	 * Distance from Origin along NumRays evenly spaced directions (ray 0 = +X, counter-clockwise) to the first
	 * sight-blocking wall, capped at MaxCm. The fog widget turns this into the visibility polygon.
	 */
	void CastRays(const FVector2D& Origin, float MaxCm, int32 NumRays, TArray<float>& OutDistances) const;

	/** Ray vs. walls - first hit distance along the unit direction Dir, or MaxCm. */
	float RayDistance(const FVector2D& Origin, const FVector2D& Dir, float MaxCm) const;

	/** Fog darkness 0..1 outside sight (vision_rules.csv fog_alpha; 0.72 if the key is missing). */
	float GetFogAlpha() const { return FogAlpha; }

	int32 NumWalls() const { return Walls.Num(); }

private:
	struct FWall
	{
		FVector2D Centre = FVector2D::ZeroVector;
		FVector2D Axis = FVector2D(1.f, 0.f);		// local X (length) direction
		float HalfLength = 0.f;
		float HalfThickness = 0.f;
		float BoundRadius = 0.f;
	};

	void LoadRules();
	static float RayDistanceIn(TArrayView<const FWall> Set, const FVector2D& Origin, const FVector2D& Dir, float MaxCm);

	TArray<FWall> Walls;
	int32 LoadedFloor = -1;

	float RadiusCm = 0.f;
	float CameraScale = 1.f;
	float FogAlpha = 0.72f;
	TArray<FS1VisionSource> Sources;
};
