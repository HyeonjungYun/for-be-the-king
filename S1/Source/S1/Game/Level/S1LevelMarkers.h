// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "S1LevelMarkers.generated.h"

class UBoxComponent;
class UHierarchicalInstancedStaticMeshComponent;
class USplineComponent;
class UStaticMesh;
class UTextRenderComponent;

/*
 * Level markers — design/gdd/level-map.md Core Rule 12 ("the level is the source").
 *
 * The user places (or the graybox builder spawns) these in the floor level; the exporter reads them back into
 * Server/Data. Positions are actor locations in cm; the layout model works in metres (S1LevelLayout).
 * Point markers are editor-only — nothing marks an entry point in the world (§ Visual "진입 지점").
 */

UENUM(BlueprintType)
enum class ES1EntryKind : uint8
{
	Outer,		// 1–6, inside the boundary band
	House		// 7–9, near a house door
};

UENUM(BlueprintType)
enum class ES1RoomKind : uint8
{
	Other,
	First,		// the room you see right after a door
	Inner,		// holds the inner chest (F1)
	Corridor
};

UENUM(BlueprintType)
enum class ES1RouteKind : uint8
{
	Lantern,	// house door → tower along lantern trails
	Detour		// house door → tower, out of the lantern walkers' screen (F4)
};

USTRUCT(BlueprintType)
struct FS1LevelDoorSpec
{
	GENERATED_BODY()

	/** N / S sit on the north / south wall, E / W on the east / west wall. */
	UPROPERTY(EditAnywhere, Category = "Level")
	FString Side = TEXT("N");

	/** The other coordinate of the door centre, in metres (y for N/S, x for E/W). */
	UPROPERTY(EditAnywhere, Category = "Level")
	double AtMetres = 0.0;

	UPROPERTY(EditAnywhere, Category = "Level")
	bool bRubble = false;
};

/** Base: a root, an editor label and the "S1 level marker" tag. */
UCLASS(Abstract, NotPlaceable)
class S1_API AS1LevelMarker : public AActor
{
	GENERATED_BODY()

public:
	AS1LevelMarker();

	/** Label floating above the marker in the editor viewport. */
	void SetMarkerLabel(const FString& Text, FColor Color);

	/** Location on the floor in metres (layout units). */
	FVector2D FloorMetres() const;

protected:
	UPROPERTY(VisibleAnywhere, Category = "Level")
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(VisibleAnywhere, Category = "Level")
	TObjectPtr<UTextRenderComponent> Label;
};

UCLASS()
class S1_API AS1EntryPointMarker : public AS1LevelMarker
{
	GENERATED_BODY()

public:
	AS1EntryPointMarker();

	UPROPERTY(EditAnywhere, Category = "Level")
	int32 EntryId = 0;

	UPROPERTY(EditAnywhere, Category = "Level")
	ES1EntryKind Kind = ES1EntryKind::Outer;

	/** For near-house points: the house id (H1 …). */
	UPROPERTY(EditAnywhere, Category = "Level")
	FString HouseId;
};

UCLASS()
class S1_API AS1ChestSpotMarker : public AS1LevelMarker
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Level")
	int32 ChestId = 0;

	/** Empty = an open-field chest. */
	UPROPERTY(EditAnywhere, Category = "Level")
	FString HouseId;

	UPROPERTY(EditAnywhere, Category = "Level")
	FString Room;
};

UCLASS()
class S1_API AS1TowerMarker : public AS1LevelMarker
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Level")
	FString TowerId;
};

UCLASS()
class S1_API AS1MonsterSpotMarker : public AS1LevelMarker
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Level")
	FString SpotId;

	UPROPERTY(EditAnywhere, Category = "Level")
	FString HouseId;

	UPROPERTY(EditAnywhere, Category = "Level")
	bool bBoss = false;
};

/** A house outline (F1 needs the real outline) with its doors and interior grid. */
UCLASS()
class S1_API AS1HouseVolume : public AS1LevelMarker
{
	GENERATED_BODY()

public:
	AS1HouseVolume();

	UPROPERTY(EditAnywhere, Category = "Level")
	FString HouseId;

	UPROPERTY(EditAnywhere, Category = "Level")
	TArray<FS1LevelDoorSpec> Doors;

	/** 29 x 29 interior grid, relative to the layout file (design/levels/floor-1.md § 4). */
	UPROPERTY(EditAnywhere, Category = "Level")
	FString GridFile;

	UPROPERTY(EditAnywhere, Category = "Level")
	bool bHasInnerChest = false;

	UPROPERTY(EditAnywhere, Category = "Level")
	FVector2D InnerChestMetres = FVector2D::ZeroVector;

	/** Sets the box to the rectangle x0..x1, y0..y1 (metres). */
	void SetRectMetres(double X0, double X1, double Y0, double Y1);

	/** The rectangle back in metres: X0, X1, Y0, Y1. */
	void GetRectMetres(double& X0, double& X1, double& Y0, double& Y1) const;

protected:
	UPROPERTY(VisibleAnywhere, Category = "Level")
	TObjectPtr<UBoxComponent> Box;
};

/** One room of a house — "first room", "inner room", or open air (마당 노지, floor-1.md § 4). */
UCLASS()
class S1_API AS1RoomVolume : public AS1LevelMarker
{
	GENERATED_BODY()

public:
	AS1RoomVolume();

	UPROPERTY(EditAnywhere, Category = "Level")
	FString HouseId;

	UPROPERTY(EditAnywhere, Category = "Level")
	FString Room;

	UPROPERTY(EditAnywhere, Category = "Level")
	ES1RoomKind Kind = ES1RoomKind::Other;

	/** Open air: field floor tone and field ambience inside the house (H1 courtyard). */
	UPROPERTY(EditAnywhere, Category = "Level")
	bool bOpenAir = false;

	void SetRectMetres(double X0, double X1, double Y0, double Y1);

protected:
	UPROPERTY(VisibleAnywhere, Category = "Level")
	TObjectPtr<UBoxComponent> Box;
};

/** A lantern route or a detour — validation only, never in the game (Core Rule 12, F4). */
UCLASS()
class S1_API AS1RouteSpline : public AS1LevelMarker
{
	GENERATED_BODY()

public:
	AS1RouteSpline();

	UPROPERTY(EditAnywhere, Category = "Level")
	ES1RouteKind Kind = ES1RouteKind::Detour;

	UPROPERTY(EditAnywhere, Category = "Level")
	FString HouseId;

	UPROPERTY(EditAnywhere, Category = "Level")
	FString TowerId;

	void SetPointsMetres(const TArray<FVector2D>& Points);
	TArray<FVector2D> GetPointsMetres() const;

protected:
	UPROPERTY(VisibleAnywhere, Category = "Level")
	TObjectPtr<USplineComponent> Spline;
};

/**
 * A dirt trail. Lays its own surface and, if bLantern, a lantern every LanternSpacing metres (Core Rule 8, F2) —
 * placing lanterns by hand is how the spacing goes wrong.
 */
UCLASS()
class S1_API AS1TrailSpline : public AActor
{
	GENERATED_BODY()

public:
	AS1TrailSpline();

	virtual void OnConstruction(const FTransform& Transform) override;

	UPROPERTY(EditAnywhere, Category = "Level")
	FString TrailId;

	UPROPERTY(EditAnywhere, Category = "Level")
	bool bLantern = false;

	/** Metres between lanterns — level-map.md Tuning Knobs lantern_spacing (4–5.5). */
	UPROPERTY(EditAnywhere, Category = "Level", meta = (ClampMin = "2.0", ClampMax = "5.5"))
	double LanternSpacing = 5.0;

	/** Metres. */
	UPROPERTY(EditAnywhere, Category = "Level")
	double Width = 3.0;

	UPROPERTY(EditAnywhere, Category = "Level|Meshes")
	TObjectPtr<UStaticMesh> SurfaceMesh;

	UPROPERTY(EditAnywhere, Category = "Level|Meshes")
	TObjectPtr<UStaticMesh> LanternMesh;

	void SetPointsMetres(const TArray<FVector2D>& Points);
	TArray<FVector2D> GetPointsMetres() const;

	/** Rebuilds the surface pieces and lanterns from the spline. */
	void RebuildDressing();

	int32 NumLanterns() const;

protected:
	UPROPERTY(VisibleAnywhere, Category = "Level")
	TObjectPtr<USplineComponent> Spline;

	UPROPERTY(VisibleAnywhere, Category = "Level")
	TObjectPtr<UHierarchicalInstancedStaticMeshComponent> Surface;

	UPROPERTY(VisibleAnywhere, Category = "Level")
	TObjectPtr<UHierarchicalInstancedStaticMeshComponent> Lanterns;
};
