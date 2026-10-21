// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Game/Level/S1LevelRules.h"
#include "S1LevelBuilder.generated.h"

class UHierarchicalInstancedStaticMeshComponent;
class UStaticMesh;

/** Component tag on the coast walls — what a wall-passing dash still stops at (2026-10-11 island). */
inline const FName S1LevelCoastTag(TEXT("S1Coast"));

/** Structural module kinds — design/levels/floor-1.md § 8. One instanced component each. */
UENUM(BlueprintType)
enum class ES1LevelModule : uint8
{
	Ground,			// the base slab under the whole floor — the collision players walk on; the sea colour around an island
	Boundary,		// impassable coast wall along the floor's outline (the square without one) — server wall kind "boundary"
	Wall,			// W / WB — 4 m, server wall
	DoorPost,		// DF — two posts per 3 m opening, server wall
	Pillar,			// P at grid joints — server wall
	FreePillar,		// free-standing pillar (boss room) — server wall
	FloorTile,		// FT — 4 x 4 m, per room material
	Rubble,			// RB — flat, passable
	TowerBody,		// 3 x 3 m box collision — server wall (ADR-0006: one box per server wall, so not a cylinder)
	TowerPlaza,		// 12 m paved ring
	Cover,			// open-field cover piece — server wall
	IslandGround,	// island ground tiles inside the coast — look only, the Ground slab under them carries the collision
	Count UMETA(Hidden)
};

/**
 * Builds the floor graybox from the layout data and exports the level back to Server/Data
 * (design/levels/floor-1.md § 9 — "에디터 도구로 자동 생성", design/gdd/level-map.md Core Rules 12–16).
 *
 * Place one in an empty level and press "Build Graybox". Every module kind starts as an engine basic shape;
 * put the real mesh in ModuleMeshes (authored at module size, pivot at the bottom centre) and build again —
 * the same spots get the real module. Generated markers and trails are destroyed and re-spawned on every build.
 *
 * Headless: UnrealEditor-Cmd.exe S1.uproject -run=S1LevelGraybox -Map=/Game/Maps/Floor1 [-Export] [-NoSave]
 */
UCLASS()
class S1_API AS1LevelBuilder : public AActor
{
	GENERATED_BODY()

public:
	AS1LevelBuilder();

	/** Layout file, relative to the project directory (Tools/level/floor1.json — the approved floor 1). */
	UPROPERTY(EditAnywhere, Category = "Level")
	FString LayoutFile = TEXT("../Tools/level/floor1.json");

	/**
	 * Where the export goes, relative to the project directory. Not Server/Data itself until activation
	 * (docs/architecture/adr-0006-server-2d-walls.md § Migration 4) — the server would otherwise load floor-1 walls
	 * while the client still opens DevMap, and players would hit invisible walls.
	 */
	UPROPERTY(EditAnywhere, Category = "Level")
	FString ExportDirectory = TEXT("../Server/Data/export");

	UPROPERTY(EditAnywhere, Category = "Level")
	int32 FloorId = 0;

	/**
	 * Adds a movable sun, sky light and atmosphere so the graybox can be seen at all — the project has neither Lumen
	 * nor static lighting. TEMPORARY: the real lighting is decided by the lighting spike (level-map.md OQ-3).
	 */
	UPROPERTY(EditAnywhere, Category = "Level")
	bool bTemporaryLighting = true;

	/** Real module meshes. Missing kinds fall back to engine basic shapes. */
	UPROPERTY(EditAnywhere, Category = "Level|Modules")
	TMap<ES1LevelModule, TObjectPtr<UStaticMesh>> ModuleMeshes;

	/**
	 * The coast the last build raised (metres, clockwise from north) — empty = the 280 m square. CollectLayout reads it back,
	 * the export writes it to floor_outline.csv and the server keeps players inside it (2026-10-11 island).
	 */
	UPROPERTY(VisibleAnywhere, Category = "Level")
	TArray<FVector2D> CoastOutlineMetres;

	/** The layout's open_chest_count rule (0 = the default 12), kept for the round trip. */
	UPROPERTY(VisibleAnywhere, Category = "Level")
	int32 OpenChestCountRule = 0;

	/** Content hash of the last export (Core Rule 16). */
	UPROPERTY(VisibleAnywhere, Category = "Level")
	FString LastDataVersion;

	/** One-line result of the last build / validate / export. */
	UPROPERTY(VisibleAnywhere, Category = "Level")
	FString LastResult;

	UFUNCTION(CallInEditor, Category = "Level")
	void BuildGraybox();

	UFUNCTION(CallInEditor, Category = "Level")
	void ValidateLevel();

	UFUNCTION(CallInEditor, Category = "Level")
	void ExportLevelData();

	/** Builds the graybox from LayoutFile. */
	bool Build(FString& OutError);

	/** Reads the markers, trails and cover in this builder's world back into a layout. */
	bool CollectLayout(FS1LevelLayout& OutLayout, FString& OutError) const;

	/** Every layout, route and house-grid rule. */
	void Validate(const FS1LevelLayout& Layout, FS1LevelReport& OutReport) const;

	/** Validates, then writes the CSVs. Writes nothing if a rule fails (level-map.md Edge Cases). */
	bool Export(FS1LevelReport& OutReport, FString& OutDirectory, FString& OutError);

	/**
	 * walls.csv rows (without the header) — one oriented box per server-wall module instance, sized from the mesh's
	 * simple collision, sorted (ADR-0006 § ⑧ E1–E4, E6, E9). False with the reason if a mesh or instance breaks a rule.
	 */
	bool CollectServerWalls(TArray<FString>& OutRows, FString& OutError) const;

	/** Primitives that block characters above floor height but are not builder modules — "client-only walls" (E7). */
	void FindClientOnlyBlockers(TArray<FString>& OutNames) const;

	FString LayoutPath() const;

	int32 NumInstances(ES1LevelModule Module) const;

private:
	void ClearGenerated();
	bool HasModuleMesh(ES1LevelModule Module) const;
	void AddModule(ES1LevelModule Module, const FVector& BottomCentreCm, double YawDeg, const FVector& SizeCm);
	void BuildHouse(const FS1LevelHouse& House, const FS1HouseGrid& Grid, const TArray<FString>& OpenAirRooms);

	/** Island ground tiles over the outline (a quadtree, 16 m inside down to 2 m at the coast) and the coast walls on it. */
	void BuildIsland(const FS1LevelLayout& Layout);

	template <typename T>
	T* SpawnMarker(const FVector2D& Metres, const FString& Label, FColor Color);

	UPROPERTY(VisibleAnywhere, Category = "Level")
	TArray<TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> Modules;

	UPROPERTY()
	TArray<TObjectPtr<AActor>> Generated;
};
