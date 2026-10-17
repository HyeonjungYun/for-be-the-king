// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/Level/S1LevelBuilder.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/Brush.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Level.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Game/Level/S1LevelMarkers.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#if WITH_EDITOR
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#endif
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "PhysicsEngine/BodySetup.h"
#include "UObject/ConstructorHelpers.h"

#include <type_traits>

DEFINE_LOG_CATEGORY_STATIC(LogS1Level, Log, All);

namespace S1LevelBuild
{
	constexpr double Cm = 100.0;
	constexpr int32 NumModules = static_cast<int32>(ES1LevelModule::Count);

	// Module sizes in cm (length along the wall, thickness, height) — design/levels/floor-1.md § 8.
	const FVector WallSize(400.0, 50.0, 300.0);
	const FVector PostSize(50.0, 50.0, 300.0);
	const FVector PillarSize(50.0, 50.0, 320.0);
	const FVector RubbleSize(400.0, 60.0, 8.0);
	const FVector FloorTileSize(400.0, 400.0, 3.0);
	const FVector CoverSize(600.0, 100.0, 250.0);
	const FVector BoundarySize(400.0, 100.0, 400.0);
	const FVector TowerBodySize(300.0, 300.0, 800.0);

	// Island (2026-10-11): coast pieces are at most 4 m along an outline edge and 3 m thick outward — thick enough to hide
	// the ground tiles that poke past the coast. Ground tiles: 16 m inside, quartered down to 2 m at the coast, where a
	// tile stays if its centre is within CoastReachM outside (2 m tile half diagonal 1.41 m < 1.5 — every inside point is
	// covered, and nothing pokes out further than 1.5 + 1.41 < 3 m)
	constexpr double CoastPieceMaxM = 4.0;
	constexpr double CoastThicknessCm = 300.0;
	constexpr double CoastJointMinTurnDeg = 5.0;
	constexpr double IslandTileMaxM = 16.0;
	constexpr double IslandTileMinM = 2.0;
	constexpr double CoastReachM = 1.5;
	constexpr double IslandTileThicknessCm = 1.0;	// under trail dirt (top 3 cm) and house floor tiles (top 3.5 cm)
	constexpr double SeaMarginM = 40.0;				// ground past the coast — the camera never sees the end of the world

	// Server walls (docs/architecture/adr-0006-server-2d-walls.md § ①): every module that blocks movement except the
	// ground. Since the island (2026-10-11) the coast is a server wall too — kind "boundary", pieces scaled to their edge
	// so no design size (E3). Kind = the walls.csv kind column.
	struct FServerWallModule
	{
		ES1LevelModule	Module;
		const TCHAR*	Kind;
		FVector			DesignSize;		// length x thickness the layout rules assume (E3); zero = not checked
	};

	const FServerWallModule ServerWallModules[] =
	{
		{ ES1LevelModule::Wall,			TEXT("wall"),			WallSize },
		{ ES1LevelModule::DoorPost,		TEXT("door_post"),		PostSize },
		{ ES1LevelModule::Pillar,		TEXT("pillar"),			PillarSize },
		{ ES1LevelModule::FreePillar,	TEXT("free_pillar"),	PillarSize },
		{ ES1LevelModule::Cover,		TEXT("cover"),			CoverSize },
		{ ES1LevelModule::TowerBody,	TEXT("tower"),			FVector::ZeroVector },
		{ ES1LevelModule::Boundary,		TEXT("boundary"),		FVector::ZeroVector },
	};

	constexpr double AngleToleranceDeg = 0.01;
	constexpr double SizeToleranceCm = 1.0;		// E3
	constexpr double StepHeightCm = 20.0;		// Core Rule 2 — anything taller that blocks must be a server wall (E7)

	bool IsServerWall(ES1LevelModule Module)
	{
		for (const FServerWallModule& Spec : ServerWallModules)
		{
			if (Spec.Module == Module)
			{
				return true;
			}
		}
		return false;
	}

	const TCHAR* ModuleName(ES1LevelModule Module)
	{
		switch (Module)
		{
		case ES1LevelModule::Ground:		return TEXT("Ground");
		case ES1LevelModule::Boundary:		return TEXT("Boundary");
		case ES1LevelModule::Wall:			return TEXT("Wall");
		case ES1LevelModule::DoorPost:		return TEXT("DoorPost");
		case ES1LevelModule::Pillar:		return TEXT("Pillar");
		case ES1LevelModule::FreePillar:	return TEXT("FreePillar");
		case ES1LevelModule::FloorTile:		return TEXT("FloorTile");
		case ES1LevelModule::Rubble:		return TEXT("Rubble");
		case ES1LevelModule::TowerBody:		return TEXT("TowerBody");
		case ES1LevelModule::TowerPlaza:	return TEXT("TowerPlaza");
		case ES1LevelModule::Cover:			return TEXT("Cover");
		case ES1LevelModule::IslandGround:	return TEXT("IslandGround");
		default:							return TEXT("Unknown");
		}
	}

	bool BlocksMovement(ES1LevelModule Module)
	{
		switch (Module)
		{
		case ES1LevelModule::FloorTile:
		case ES1LevelModule::Rubble:
		case ES1LevelModule::TowerPlaza:
		case ES1LevelModule::IslandGround:
			return false;	// walkable at floor height (≤ 0.2 m, Core Rule 2)
		default:
			return true;
		}
	}

	// The tower body is a box: a server wall is one oriented box (ADR-0006). The art pass may round its look, not its collision.
	bool IsCylinder(ES1LevelModule Module)
	{
		return Module == ES1LevelModule::TowerPlaza;
	}

	UStaticMesh* BasicShape(ES1LevelModule Module)
	{
		return LoadObject<UStaticMesh>(nullptr, IsCylinder(Module) ? TEXT("/Engine/BasicShapes/Cylinder.Cylinder") : TEXT("/Engine/BasicShapes/Cube.Cube"));
	}

	FString Join(const TArray<FString>& Rows)
	{
		return FString::Join(Rows, TEXT("\r\n")) + TEXT("\r\n");
	}

	int32 RoundCm(double Metres)
	{
		return FMath::RoundToInt(Metres * Cm);
	}

	// The sea around an island (2026-10-11). The graybox has no material of its own and a runtime dynamic instance of the
	// basic shape material did not change the instanced slab in game — so the builder makes one flat-colour material asset
	// once (editor only, flagged for instanced meshes so the game does not fall back to the default) and keeps using it
	const TCHAR* SeaPackage = TEXT("/Game/Level/M_S1Sea");
	const TCHAR* SeaObject = TEXT("/Game/Level/M_S1Sea.M_S1Sea");
	const FLinearColor SeaColor(0.015f, 0.07f, 0.16f);

	UMaterialInterface* SeaMaterial()
	{
		if (UMaterialInterface* Existing = LoadObject<UMaterialInterface>(nullptr, SeaObject, nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			return Existing;
		}
#if WITH_EDITOR
		UPackage* Package = CreatePackage(SeaPackage);
		UMaterial* Material = NewObject<UMaterial>(Package, TEXT("M_S1Sea"), RF_Public | RF_Standalone);
		UMaterialExpressionConstant3Vector* Color = NewObject<UMaterialExpressionConstant3Vector>(Material);
		Color->Constant = SeaColor;
		Material->GetExpressionCollection().AddExpression(Color);
		Material->GetEditorOnlyData()->BaseColor.Connect(0, Color);
		Material->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);		// 5.8: 필드 직접 쓰기는 deprecated (C4996). PostEditChange 가 다시 컴파일한다
		Material->PostEditChange();

		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		const FString File = FPackageName::LongPackageNameToFilename(SeaPackage, FPackageName::GetAssetPackageExtension());
		if (!UPackage::SavePackage(Package, Material, *File, Args))
		{
			UE_LOG(LogS1Level, Error, TEXT("[LEVEL] cannot save the sea material %s"), *File);
		}
		return Material;
#else
		return nullptr;
#endif
	}
}

AS1LevelBuilder::AS1LevelBuilder()
{
	using namespace S1LevelBuild;

	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	// Static like the modules — a movable root refuses static children ("AttachTo ... Aborting"), and the modules
	// would then stay put when the builder moves while the markers do not (ADR-0006 E5)
	RootComponent->SetMobility(EComponentMobility::Static);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cylinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));

	for (int32 i = 0; i < NumModules; i++)
	{
		const ES1LevelModule Module = static_cast<ES1LevelModule>(i);
		UHierarchicalInstancedStaticMeshComponent* Component =
			CreateDefaultSubobject<UHierarchicalInstancedStaticMeshComponent>(FName(*FString::Printf(TEXT("Module_%s"), ModuleName(Module))));
		Component->SetupAttachment(RootComponent);
		Component->SetStaticMesh(IsCylinder(Module) ? Cylinder.Object : Cube.Object);
		Component->SetCollisionProfileName(BlocksMovement(Module) ? TEXT("BlockAll") : TEXT("NoCollision"));
		if (IsServerWall(Module))
		{
			Component->CanCharacterStepUpOn = ECB_No;	// never step onto a wall or cover (ADR-0006 E8)
		}
		if (Module == ES1LevelModule::Boundary)
		{
			Component->ComponentTags.Add(S1LevelCoastTag);	// the ghost dash passes walls but stops here (AS1Player::TickDash)
		}
		Component->SetMobility(EComponentMobility::Static);
		Modules.Add(Component);
	}
}

FString AS1LevelBuilder::LayoutPath() const
{
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), LayoutFile));
}

int32 AS1LevelBuilder::NumInstances(ES1LevelModule Module) const
{
	const int32 Index = static_cast<int32>(Module);
	return Modules.IsValidIndex(Index) && Modules[Index] != nullptr ? Modules[Index]->GetInstanceCount() : 0;
}

// ── Editor buttons ────────────────────────────────────────────────────────────

void AS1LevelBuilder::BuildGraybox()
{
	FString Error;
	if (Build(Error))
	{
		LastResult = FString::Printf(TEXT("built — walls %d, cover %d, markers %d"),
			NumInstances(ES1LevelModule::Wall), NumInstances(ES1LevelModule::Cover), Generated.Num());
		UE_LOG(LogS1Level, Display, TEXT("[LEVEL] graybox %s"), *LastResult);
	}
	else
	{
		LastResult = TEXT("build failed: ") + Error;
		UE_LOG(LogS1Level, Error, TEXT("[LEVEL] %s"), *LastResult);
	}
}

void AS1LevelBuilder::ValidateLevel()
{
	FS1LevelLayout Layout;
	FString Error;
	if (!CollectLayout(Layout, Error))
	{
		LastResult = TEXT("validate failed: ") + Error;
		UE_LOG(LogS1Level, Error, TEXT("[LEVEL] %s"), *LastResult);
		return;
	}
	FS1LevelReport Report;
	Validate(Layout, Report);
	LastResult = FString::Printf(TEXT("validated — %d checks, %d failed"), Report.Checks.Num(), Report.NumFailed());
	UE_LOG(LogS1Level, Display, TEXT("[LEVEL] %s\n%s"), *LastResult, *Report.ToString(/*bFailuresOnly*/ true));
}

void AS1LevelBuilder::ExportLevelData()
{
	FS1LevelReport Report;
	FString Directory, Error;
	if (Export(Report, Directory, Error))
	{
		LastResult = FString::Printf(TEXT("exported to %s — version %s"), *Directory, *LastDataVersion);
		UE_LOG(LogS1Level, Display, TEXT("[LEVEL] %s"), *LastResult);
	}
	else
	{
		LastResult = TEXT("export refused: ") + Error;
		UE_LOG(LogS1Level, Error, TEXT("[LEVEL] %s\n%s"), *LastResult, *Report.ToString(true));
	}
}

// ── Build ─────────────────────────────────────────────────────────────────────

void AS1LevelBuilder::ClearGenerated()
{
	for (AActor* Actor : Generated)
	{
		if (IsValid(Actor))
		{
			Actor->Destroy();
		}
	}
	Generated.Reset();
	for (UHierarchicalInstancedStaticMeshComponent* Component : Modules)
	{
		Component->ClearInstances();
	}
}

bool AS1LevelBuilder::HasModuleMesh(ES1LevelModule Module) const
{
	const TObjectPtr<UStaticMesh>* Mesh = ModuleMeshes.Find(Module);
	return Mesh != nullptr && *Mesh != nullptr;
}

void AS1LevelBuilder::AddModule(ES1LevelModule Module, const FVector& BottomCentreCm, double YawDeg, const FVector& SizeCm)
{
	UHierarchicalInstancedStaticMeshComponent* Component = Modules[static_cast<int32>(Module)];
	const FRotator Yaw(0.0, YawDeg, 0.0);
	if (HasModuleMesh(Module))
	{
		// A real module mesh is authored at module size with its pivot at the bottom centre.
		Component->AddInstance(FTransform(Yaw, BottomCentreCm, FVector::OneVector), /*bWorldSpace*/ true);
	}
	else
	{
		// Engine basic shapes are 1 m with the pivot at the centre.
		const FVector Centre = BottomCentreCm + FVector(0.0, 0.0, SizeCm.Z * 0.5);
		Component->AddInstance(FTransform(Yaw, Centre, SizeCm / 100.0), true);
	}
}

template <typename T>
T* AS1LevelBuilder::SpawnMarker(const FVector2D& Metres, const FString& Label, FColor Color)
{
	FActorSpawnParameters Params;
	Params.Owner = this;
	Params.OverrideLevel = GetLevel();
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	T* Actor = GetWorld()->SpawnActor<T>(T::StaticClass(), FTransform(FVector(Metres.X * S1LevelBuild::Cm, Metres.Y * S1LevelBuild::Cm, 50.0)), Params);
	if (Actor != nullptr)
	{
		if constexpr (std::is_base_of_v<AS1LevelMarker, T>)
		{
			Actor->SetMarkerLabel(Label, Color);
		}
#if WITH_EDITOR
		Actor->SetActorLabel(Label);
#endif
		Actor->Tags.Add(TEXT("S1Generated"));
		Generated.Add(Actor);
	}
	return Actor;
}

bool AS1LevelBuilder::Build(FString& OutError)
{
	using namespace S1LevelBuild;

	FS1LevelLayout Layout;
	if (!Layout.LoadFromFile(LayoutPath(), OutError))
	{
		return false;
	}

	ClearGenerated();

	// The whole transform, not just the location — a rotated or scaled builder shifts every exported row (ADR-0006 E5)
	SetActorTransform(FTransform::Identity);

	// Real mesh if there is one, otherwise the basic shape — also when a mesh was removed from ModuleMeshes
	for (int32 i = 0; i < NumModules; i++)
	{
		const ES1LevelModule Module = static_cast<ES1LevelModule>(i);
		Modules[i]->SetStaticMesh(HasModuleMesh(Module) ? ModuleMeshes[Module].Get() : BasicShape(Module));
	}

	CoastOutlineMetres = Layout.Outline;
	OpenChestCountRule = Layout.OpenChestCount;

	// Ground: one slab under the whole floor — the collision players walk on. The square: 280 m plus 24 m of fill past the
	// boundary (level-map.md § Visual "경계 띠"). The island: its bounding box plus SeaMarginM, coloured as sea at play
	const double Half = S1LevelRules::FloorHalf * Cm;
	if (Layout.HasOutline())
	{
		double X0, X1, Y0, Y1;
		Layout.FloorBounds(X0, X1, Y0, Y1);
		AddModule(ES1LevelModule::Ground, FVector((X0 + X1) * 0.5 * Cm, (Y0 + Y1) * 0.5 * Cm, -20.0), 0.0,
			FVector((X1 - X0 + 2.0 * SeaMarginM) * Cm, (Y1 - Y0 + 2.0 * SeaMarginM) * Cm, 20.0));
	}
	else
	{
		AddModule(ES1LevelModule::Ground, FVector(0.0, 0.0, -20.0), 0.0, FVector((Half + 2400.0) * 2.0, (Half + 2400.0) * 2.0, 20.0));
	}
	if (!HasModuleMesh(ES1LevelModule::Ground))
	{
		Modules[static_cast<int32>(ES1LevelModule::Ground)]->SetMaterial(0, Layout.HasOutline() ? SeaMaterial() : nullptr);
	}

	// Temporary lighting — only so the graybox is visible; the lighting spike decides the real setup (OQ-3).
	if (bTemporaryLighting)
	{
		if (ADirectionalLight* Sun = SpawnMarker<ADirectionalLight>(FVector2D::ZeroVector, TEXT("TEMP Graybox Sun (OQ-3)"), FColor::White))
		{
			Sun->SetActorRotation(FRotator(-50.0, 30.0, 0.0));
			Sun->GetLightComponent()->SetMobility(EComponentMobility::Movable);
			if (UDirectionalLightComponent* Light = Cast<UDirectionalLightComponent>(Sun->GetLightComponent()))
			{
				Light->SetAtmosphereSunLight(true);
			}
		}
		if (ASkyLight* Sky = SpawnMarker<ASkyLight>(FVector2D::ZeroVector, TEXT("TEMP Graybox Sky Light (OQ-3)"), FColor::White))
		{
			Sky->GetLightComponent()->SetMobility(EComponentMobility::Movable);
			Sky->GetLightComponent()->bRealTimeCapture = true;
		}
		SpawnMarker<ASkyAtmosphere>(FVector2D::ZeroVector, TEXT("TEMP Graybox Atmosphere (OQ-3)"), FColor::White);
	}

	// Boundary: a continuous wall just outside the play area (Core Rule 3) — the island's coast, or the square
	if (Layout.HasOutline())
	{
		BuildIsland(Layout);
	}
	else
	{
		for (double Along = -Half + 200.0; Along < Half; Along += 400.0)
		{
			AddModule(ES1LevelModule::Boundary, FVector(Half + 50.0, Along, 0.0), 90.0, BoundarySize);
			AddModule(ES1LevelModule::Boundary, FVector(-Half - 50.0, Along, 0.0), 90.0, BoundarySize);
			AddModule(ES1LevelModule::Boundary, FVector(Along, Half + 50.0, 0.0), 0.0, BoundarySize);
			AddModule(ES1LevelModule::Boundary, FVector(Along, -Half - 50.0, 0.0), 0.0, BoundarySize);
		}
	}

	// Houses, from their 4 m grids.
	const FString LayoutDir = FPaths::GetPath(LayoutPath());
	for (const FS1LevelHouse& House : Layout.Houses)
	{
		AS1HouseVolume* Volume = SpawnMarker<AS1HouseVolume>(House.Centre(), House.Id, FColor(255, 170, 60));
		if (Volume != nullptr)
		{
			Volume->HouseId = House.Id;
			Volume->GridFile = House.GridFile;
			Volume->bHasInnerChest = House.bHasInnerChest;
			Volume->InnerChestMetres = House.InnerChest;
			for (const FS1LevelDoor& Door : House.Doors)
			{
				FS1LevelDoorSpec& Spec = Volume->Doors.AddDefaulted_GetRef();
				Spec.Side = FString::Chr(Door.Side);
				Spec.AtMetres = Door.At;
				Spec.bRubble = Door.bRubble;
			}
			Volume->SetRectMetres(House.X0, House.X1, House.Y0, House.Y1);
		}

		if (House.GridFile.IsEmpty())
		{
			continue;
		}
		FString Text;
		FS1HouseGrid Grid;
		if (!FFileHelper::LoadFileToString(Text, *FPaths::Combine(LayoutDir, House.GridFile)) || !Grid.Parse(Text, OutError))
		{
			OutError = FString::Printf(TEXT("house %s grid %s: %s"), *House.Id, *House.GridFile, *OutError);
			return false;
		}
		BuildHouse(House, Grid, House.OpenAirRooms);
	}

	// Towers: body + paved 12 m ring.
	for (const FS1LevelTower& Tower : Layout.Towers)
	{
		const FVector At(Tower.P.X * Cm, Tower.P.Y * Cm, 0.0);
		AddModule(ES1LevelModule::TowerPlaza, At + FVector(0.0, 0.0, 1.0), 0.0, FVector(2400.0, 2400.0, 4.0));
		AddModule(ES1LevelModule::TowerBody, At, 0.0, TowerBodySize);
		if (AS1TowerMarker* Marker = SpawnMarker<AS1TowerMarker>(Tower.P, Tower.Id, FColor(80, 120, 255)))
		{
			Marker->TowerId = Tower.Id;
		}
	}

	// Entry points, chests, monsters.
	for (const FS1LevelEntry& Entry : Layout.Entries)
	{
		if (AS1EntryPointMarker* Marker = SpawnMarker<AS1EntryPointMarker>(Entry.P, FString::Printf(TEXT("Entry %d"), Entry.Id), FColor(220, 70, 70)))
		{
			Marker->EntryId = Entry.Id;
			Marker->Kind = Entry.bOuter ? ES1EntryKind::Outer : ES1EntryKind::House;
			Marker->HouseId = Entry.HouseId;
		}
	}
	auto SpawnChests = [this](const TArray<FS1LevelChest>& Chests)
	{
		for (const FS1LevelChest& Chest : Chests)
		{
			if (AS1ChestSpotMarker* Marker = SpawnMarker<AS1ChestSpotMarker>(Chest.P, FString::Printf(TEXT("Chest %d"), Chest.Id), FColor(240, 200, 40)))
			{
				Marker->ChestId = Chest.Id;
				Marker->HouseId = Chest.HouseId;
				Marker->Room = Chest.Room;
			}
		}
	};
	SpawnChests(Layout.OpenChests);
	SpawnChests(Layout.HouseChests);
	for (const FS1LevelMonster& Monster : Layout.Monsters)
	{
		if (AS1MonsterSpotMarker* Marker = SpawnMarker<AS1MonsterSpotMarker>(Monster.P, FString::Printf(TEXT("Monster %s"), *Monster.Id), FColor(150, 30, 30)))
		{
			Marker->SpotId = Monster.Id;
			Marker->HouseId = Monster.HouseId;
			Marker->bBoss = Monster.HouseId == Layout.BossHouse && Monster.Id == TEXT("boss");
		}
	}

	// Trails (they lay their own surface and lanterns) and the validation-only routes.
	for (const FS1LevelTrail& Trail : Layout.Trails)
	{
		if (AS1TrailSpline* Actor = SpawnMarker<AS1TrailSpline>(FVector2D::ZeroVector, FString::Printf(TEXT("Trail %s"), *Trail.Id), FColor::White))
		{
			Actor->TrailId = Trail.Id;
			Actor->bLantern = Trail.bLantern;
			Actor->SetPointsMetres(Trail.Points);
		}
	}
	auto SpawnRoutes = [this](const TArray<FS1LevelRoute>& Routes, ES1RouteKind Kind)
	{
		for (const FS1LevelRoute& Route : Routes)
		{
			const FString Name = FString::Printf(TEXT("%s %s->%s"), Kind == ES1RouteKind::Lantern ? TEXT("Lantern") : TEXT("Detour"), *Route.HouseId, *Route.TowerId);
			if (AS1RouteSpline* Actor = SpawnMarker<AS1RouteSpline>(FVector2D::ZeroVector, Name, FColor(120, 200, 120)))
			{
				Actor->Kind = Kind;
				Actor->HouseId = Route.HouseId;
				Actor->TowerId = Route.TowerId;
				Actor->SetPointsMetres(Route.Points);
			}
		}
	};
	SpawnRoutes(Layout.LanternRoutes, ES1RouteKind::Lantern);
	SpawnRoutes(Layout.Detours, ES1RouteKind::Detour);

	// Open-field cover: one wall piece per spot, alternating direction (dressing is the art pass).
	for (int32 i = 0; i < Layout.Cover.Num(); i++)
	{
		AddModule(ES1LevelModule::Cover, FVector(Layout.Cover[i].X * Cm, Layout.Cover[i].Y * Cm, 0.0), (i % 2 == 0) ? 0.0 : 90.0, CoverSize);
	}

	MarkPackageDirty();
	return true;
}

void AS1LevelBuilder::BuildIsland(const FS1LevelLayout& Layout)
{
	using namespace S1LevelBuild;

	// Ground tiles: a square wholly inside is one tile, one across the coast is quartered down to 2 m
	TFunction<void(const FVector2D&, double)> Visit = [&](const FVector2D& C, double Size)
	{
		const double Gap = Layout.BoundaryGap(C);
		const double HalfDiagonal = Size * UE_INV_SQRT_2;
		if (Gap >= HalfDiagonal || (Size <= IslandTileMinM && Gap >= -CoastReachM))
		{
			AddModule(ES1LevelModule::IslandGround, FVector(C.X * Cm, C.Y * Cm, 0.0), 0.0, FVector(Size * Cm, Size * Cm, IslandTileThicknessCm));
			return;
		}
		if (Gap < -HalfDiagonal - CoastReachM || Size <= IslandTileMinM)
		{
			return;		// all sea
		}
		const double Q = Size * 0.25;
		for (const FVector2D& D : { FVector2D(-Q, -Q), FVector2D(-Q, Q), FVector2D(Q, -Q), FVector2D(Q, Q) })
		{
			Visit(C + D, Size * 0.5);
		}
	};
	double X0, X1, Y0, Y1;
	Layout.FloorBounds(X0, X1, Y0, Y1);
	for (double X = FMath::FloorToDouble(X0 / IslandTileMaxM) * IslandTileMaxM; X < X1; X += IslandTileMaxM)
	{
		for (double Y = FMath::FloorToDouble(Y0 / IslandTileMaxM) * IslandTileMaxM; Y < Y1; Y += IslandTileMaxM)
		{
			Visit(FVector2D(X, Y) + FVector2D(IslandTileMaxM * 0.5), IslandTileMaxM);
		}
	}

	// Coast: pieces along every edge, the inner face on the coast line and the body outward. Which side is outward
	// follows the winding (signed area), so the outline may run either way
	const TArray<FVector2D>& P = Layout.Outline;
	const int32 N = P.Num();
	double Twice = 0.0;
	for (int32 i = 0; i < N; i++)
	{
		Twice += P[i].X * P[(i + 1) % N].Y - P[(i + 1) % N].X * P[i].Y;
	}
	const double Winding = Twice >= 0.0 ? 1.0 : -1.0;		// +1: counter-clockwise in (x, y), the inside on the left
	auto Outward = [Winding](const FVector2D& Dir) { return FVector2D(Dir.Y, -Dir.X) * Winding; };

	for (int32 i = 0; i < N; i++)
	{
		const FVector2D A = P[i];
		const FVector2D B = P[(i + 1) % N];
		const double Length = FVector2D::Distance(A, B);
		if (Length < UE_KINDA_SMALL_NUMBER)
		{
			continue;
		}
		const FVector2D Dir = (B - A) / Length;
		const FVector2D Out = Outward(Dir);
		const int32 Pieces = FMath::Max(1, FMath::CeilToInt(Length / CoastPieceMaxM));
		const double Piece = Length / Pieces;
		const double Yaw = FMath::RadiansToDegrees(FMath::Atan2(Dir.Y, Dir.X));
		for (int32 k = 0; k < Pieces; k++)
		{
			const FVector2D Mid = A + Dir * (Piece * (k + 0.5));
			const FVector2D Centre = Mid * Cm + Out * (CoastThicknessCm * 0.5);
			AddModule(ES1LevelModule::Boundary, FVector(Centre, 0.0), Yaw, FVector(Piece * Cm, CoastThicknessCm, BoundarySize.Z));
		}

		// A convex corner opens a wedge between the two pieces on the sea side — a square joint on the outward bisector
		// closes it. Its inner face runs through the corner square to the bisector, so it stays out of the island
		const FVector2D C = P[(i + 2) % N];
		const double NextLength = FVector2D::Distance(B, C);
		if (NextLength < UE_KINDA_SMALL_NUMBER)
		{
			continue;
		}
		const FVector2D NextDir = (C - B) / NextLength;
		const double Turn = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(FVector2D::CrossProduct(Dir, NextDir), -1.0, 1.0)));
		if (Turn * Winding > CoastJointMinTurnDeg)
		{
			const FVector2D Bisector = (Out + Outward(NextDir)).GetSafeNormal();
			const FVector2D Centre = B * Cm + Bisector * (CoastThicknessCm * 0.5);
			AddModule(ES1LevelModule::Boundary, FVector(Centre, 0.0), FMath::RadiansToDegrees(FMath::Atan2(Bisector.Y, Bisector.X)),
				FVector(CoastThicknessCm, CoastThicknessCm, BoundarySize.Z));
		}
	}
}

void AS1LevelBuilder::BuildHouse(const FS1LevelHouse& House, const FS1HouseGrid& Grid, const TArray<FString>& OpenAirRooms)
{
	using namespace S1LevelBuild;
	constexpr int32 N = FS1HouseGrid::N;
	constexpr int32 Size = FS1HouseGrid::Size;

	// Local metres (x from the south wall, y from the west wall) → world cm.
	auto World = [&House](double Lx, double Ly, double Z = 0.0)
	{
		return FVector((House.X0 + Lx) * Cm, (House.Y0 + Ly) * Cm, Z);
	};

	for (int32 R = 0; R < Size; R++)
	{
		for (int32 C = 0; C < Size; C++)
		{
			const TCHAR Ch = Grid.At(R, C);
			const bool bRowLine = (R % 2 == 0);
			const bool bColLine = (C % 2 == 0);

			if (bRowLine && bColLine)
			{
				// Grid joint: x = 56 − 2R, y = 2C (metres).
				const FVector At = World(56.0 - 2.0 * R, 2.0 * C);
				if (Ch == TEXT('+'))
				{
					AddModule(ES1LevelModule::Pillar, At, 0.0, PillarSize);
				}
				else if (Ch == TEXT('o'))
				{
					AddModule(ES1LevelModule::FreePillar, At, 0.0, PillarSize);
				}
				continue;
			}
			if (bRowLine == bColLine)
			{
				continue;	// a cell, not an edge
			}

			// Edge. A row line runs east–west (along y), a column line runs north–south (along x).
			const FVector Centre = World(56.0 - 2.0 * R, 2.0 * C);	// the same formula serves both line directions
			const double Yaw = bRowLine ? 90.0 : 0.0;
			const FVector Along = bRowLine ? FVector(0.0, 1.0, 0.0) : FVector(1.0, 0.0, 0.0);

			if (FS1HouseGrid::IsWall(Ch))
			{
				AddModule(ES1LevelModule::Wall, Centre, Yaw, WallSize);
			}
			else if (Ch == TEXT('D'))
			{
				// Two posts with a 3 m opening between them.
				AddModule(ES1LevelModule::DoorPost, Centre - Along * 175.0, 0.0, PostSize);
				AddModule(ES1LevelModule::DoorPost, Centre + Along * 175.0, 0.0, PostSize);
			}
			else if (Ch == TEXT('~'))
			{
				AddModule(ES1LevelModule::Rubble, Centre, Yaw, RubbleSize);
			}
		}
	}

	// Rooms: floor tiles (except open air), room volumes, kinds.
	FS1HouseRooms Rooms;
	Rooms.Build(Grid);
	const int32 Inner = Rooms.InnerRoom();
	const TSet<int32> First = Rooms.FirstRooms(Grid);
	for (int32 Id = 0; Id < Rooms.Cells.Num(); Id++)
	{
		const FString Letter = FString::Chr(Rooms.Labels[Id]);
		const bool bOpenAir = OpenAirRooms.Contains(Letter);
		if (!bOpenAir)
		{
			for (const FIntPoint& Cell : Rooms.Cells[Id])
			{
				AddModule(ES1LevelModule::FloorTile, World(4.0 * Cell.X + 2.0, 4.0 * Cell.Y + 2.0, 0.5), 0.0, FloorTileSize);
			}
		}

		// One volume per run of cells in a row, merged downwards when the next row has the same run.
		TMap<FIntPoint, FIntPoint> Open;	// (v0, v1) → (u0, u1)
		TArray<FIntPoint> Done;				// pairs of (v0, v1) and (u0, u1), flattened
		auto Flush = [&](const FIntPoint& Run, const FIntPoint& Rows)
		{
			Done.Add(Run);
			Done.Add(Rows);
		};
		for (int32 U = 0; U < N; U++)
		{
			TArray<FIntPoint> RowRuns;
			for (int32 V = 0; V < N; V++)
			{
				if (Rooms.CellRoom.FindRef(FIntPoint(U, V)) != Id)
				{
					continue;
				}
				if (RowRuns.Num() > 0 && RowRuns.Last().Y == V - 1)
				{
					RowRuns.Last().Y = V;
				}
				else
				{
					RowRuns.Add(FIntPoint(V, V));
				}
			}
			TMap<FIntPoint, FIntPoint> Next;
			for (const FIntPoint& Run : RowRuns)
			{
				const FIntPoint* Prev = Open.Find(Run);
				Next.Add(Run, Prev != nullptr && Prev->Y == U - 1 ? FIntPoint(Prev->X, U) : FIntPoint(U, U));
			}
			for (const TPair<FIntPoint, FIntPoint>& Pair : Open)
			{
				if (!Next.Contains(Pair.Key) || Next[Pair.Key].X != Pair.Value.X)
				{
					Flush(Pair.Key, Pair.Value);
				}
			}
			Open = MoveTemp(Next);
		}
		for (const TPair<FIntPoint, FIntPoint>& Pair : Open)
		{
			Flush(Pair.Key, Pair.Value);
		}

		for (int32 k = 0; k + 1 < Done.Num(); k += 2)
		{
			const FIntPoint Run = Done[k];
			const FIntPoint RowsU = Done[k + 1];
			const double X0 = House.X0 + 4.0 * RowsU.X, X1 = House.X0 + 4.0 * (RowsU.Y + 1);
			const double Y0 = House.Y0 + 4.0 * Run.X, Y1 = House.Y0 + 4.0 * (Run.Y + 1);
			const FString Name = FString::Printf(TEXT("%s room %s"), *House.Id, *Letter);
			if (AS1RoomVolume* Volume = SpawnMarker<AS1RoomVolume>(FVector2D((X0 + X1) * 0.5, (Y0 + Y1) * 0.5), Name, FColor(120, 200, 255)))
			{
				Volume->HouseId = House.Id;
				Volume->Room = Letter;
				Volume->Kind = (Id == Inner) ? ES1RoomKind::Inner : First.Contains(Id) ? ES1RoomKind::First
					: (Rooms.Labels[Id] == TEXT('.')) ? ES1RoomKind::Corridor : ES1RoomKind::Other;
				Volume->bOpenAir = bOpenAir;
				Volume->SetRectMetres(X0, X1, Y0, Y1);
			}
		}
	}
}

// ── Collect · validate · export ───────────────────────────────────────────────

bool AS1LevelBuilder::CollectLayout(FS1LevelLayout& Out, FString& OutError) const
{
	using namespace S1LevelBuild;

	Out = FS1LevelLayout();
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		OutError = TEXT("no world");
		return false;
	}

	for (TActorIterator<AS1HouseVolume> It(World); It; ++It)
	{
		FS1LevelHouse& House = Out.Houses.AddDefaulted_GetRef();
		House.Id = It->HouseId;
		It->GetRectMetres(House.X0, House.X1, House.Y0, House.Y1);
		House.GridFile = It->GridFile;
		House.bHasInnerChest = It->bHasInnerChest;
		House.InnerChest = It->InnerChestMetres;
		for (const FS1LevelDoorSpec& Spec : It->Doors)
		{
			FS1LevelDoor& Door = House.Doors.AddDefaulted_GetRef();
			Door.Side = Spec.Side.IsEmpty() ? TEXT('N') : Spec.Side[0];
			Door.At = Spec.AtMetres;
			Door.bRubble = Spec.bRubble;
		}
	}
	for (TActorIterator<AS1TowerMarker> It(World); It; ++It)
	{
		Out.Towers.Add({ It->TowerId, It->FloorMetres() });
	}
	for (TActorIterator<AS1EntryPointMarker> It(World); It; ++It)
	{
		FS1LevelEntry& Entry = Out.Entries.AddDefaulted_GetRef();
		Entry.Id = It->EntryId;
		Entry.bOuter = It->Kind == ES1EntryKind::Outer;
		Entry.HouseId = It->HouseId;
		Entry.P = It->FloorMetres();
	}
	for (TActorIterator<AS1ChestSpotMarker> It(World); It; ++It)
	{
		FS1LevelChest Chest{ It->ChestId, It->HouseId, It->Room, It->FloorMetres() };
		(Chest.HouseId.IsEmpty() ? Out.OpenChests : Out.HouseChests).Add(MoveTemp(Chest));
	}
	for (TActorIterator<AS1MonsterSpotMarker> It(World); It; ++It)
	{
		Out.Monsters.Add({ It->SpotId, It->HouseId, It->FloorMetres() });
		if (It->bBoss)
		{
			Out.BossHouse = It->HouseId;
		}
	}
	for (TActorIterator<AS1TrailSpline> It(World); It; ++It)
	{
		FS1LevelTrail& Trail = Out.Trails.AddDefaulted_GetRef();
		Trail.Id = It->TrailId;
		Trail.bLantern = It->bLantern;
		Trail.Points = It->GetPointsMetres();
	}
	for (TActorIterator<AS1RouteSpline> It(World); It; ++It)
	{
		FS1LevelRoute Route{ It->HouseId, It->TowerId, It->GetPointsMetres() };
		(It->Kind == ES1RouteKind::Lantern ? Out.LanternRoutes : Out.Detours).Add(MoveTemp(Route));
	}

	// Cover: every cover instance in the level's builders. The coast and the chest-count rule: the builder that raised a coast
	for (TActorIterator<AS1LevelBuilder> It(World); It; ++It)
	{
		const UHierarchicalInstancedStaticMeshComponent* Cover = It->Modules[static_cast<int32>(ES1LevelModule::Cover)];
		for (int32 i = 0; i < Cover->GetInstanceCount(); i++)
		{
			FTransform T;
			Cover->GetInstanceTransform(i, T, /*bWorldSpace*/ true);
			Out.Cover.Add(FVector2D(T.GetLocation().X / Cm, T.GetLocation().Y / Cm));
		}
		if (Out.Outline.Num() == 0)
		{
			Out.Outline = It->CoastOutlineMetres;
		}
		Out.OpenChestCount = FMath::Max(Out.OpenChestCount, It->OpenChestCountRule);
	}

	// Stable order — the actor iterator's order is not the file's.
	Out.Houses.Sort([](const FS1LevelHouse& A, const FS1LevelHouse& B) { return A.Id < B.Id; });
	Out.Entries.Sort([](const FS1LevelEntry& A, const FS1LevelEntry& B) { return A.Id < B.Id; });
	Out.OpenChests.Sort([](const FS1LevelChest& A, const FS1LevelChest& B) { return A.Id < B.Id; });
	Out.HouseChests.Sort([](const FS1LevelChest& A, const FS1LevelChest& B) { return A.Id < B.Id; });

	if (Out.Houses.Num() == 0 || Out.Towers.Num() == 0 || Out.Entries.Num() == 0)
	{
		OutError = TEXT("the level has no house volumes, tower markers or entry markers — build the graybox first");
		return false;
	}
	return true;
}

void AS1LevelBuilder::Validate(const FS1LevelLayout& Layout, FS1LevelReport& Report) const
{
	S1LevelRules::CheckLayout(Layout, Report);
	S1LevelRules::CheckRoutes(Layout, Report);

	const FString LayoutDir = FPaths::GetPath(LayoutPath());
	for (const FS1LevelHouse& House : Layout.Houses)
	{
		FString Text, Error;
		FS1HouseGrid Grid;
		if (House.GridFile.IsEmpty() || !FFileHelper::LoadFileToString(Text, *FPaths::Combine(LayoutDir, House.GridFile)) || !Grid.Parse(Text, Error))
		{
			Report.Add(FString::Printf(TEXT("house %s grid readable"), *House.Id), 0, 1, true, Error);
			continue;
		}
		S1LevelRules::CheckHouseGrid(Grid, House, Report);
	}
}

bool AS1LevelBuilder::Export(FS1LevelReport& OutReport, FString& OutDirectory, FString& OutError)
{
	using namespace S1LevelBuild;

	FS1LevelLayout Layout;
	if (!CollectLayout(Layout, OutError))
	{
		return false;
	}
	Validate(Layout, OutReport);
	if (OutReport.NumFailed() > 0)
	{
		OutError = FString::Printf(TEXT("%d rule(s) failed — nothing written, the previous export stays"), OutReport.NumFailed());
		return false;
	}

	const FString Floor = FString::FromInt(FloorId);

	// Same columns as the server's Server/Data files, in cm (z = 100: the server's spawn height).
	TArray<FString> Entries;
	Entries.Add(TEXT("entry_id,floor_id,x,y,z,yaw"));
	for (const FS1LevelEntry& E : Layout.Entries)
	{
		Entries.Add(FString::Printf(TEXT("%d,%s,%d,%d,100,0"), E.Id, *Floor, RoundCm(E.P.X), RoundCm(E.P.Y)));
	}
	TArray<FString> Chests;
	Chests.Add(TEXT("chest_id,floor_id,tier,x,y,z,yaw,respawn_s"));
	for (const TArray<FS1LevelChest>* List : { &Layout.OpenChests, &Layout.HouseChests })
	{
		for (const FS1LevelChest& C : *List)
		{
			Chests.Add(FString::Printf(TEXT("%d,%s,%s,%d,%d,100,0,1800"), C.Id, *Floor, C.HouseId.IsEmpty() ? TEXT("open") : TEXT("house"),
				RoundCm(C.P.X), RoundCm(C.P.Y)));
		}
	}
	TArray<FString> Towers;
	Towers.Add(TEXT("tower_id,floor_id,x,y"));
	for (const FS1LevelTower& T : Layout.Towers)
	{
		Towers.Add(FString::Printf(TEXT("%s,%s,%d,%d"), *T.Id, *Floor, RoundCm(T.P.X), RoundCm(T.P.Y)));
	}
	TArray<FString> Monsters;
	Monsters.Add(TEXT("spot_id,floor_id,house,boss,x,y"));
	for (const FS1LevelMonster& M : Layout.Monsters)
	{
		Monsters.Add(FString::Printf(TEXT("%s,%s,%s,%d,%d,%d"), *M.Id, *Floor, *M.HouseId, M.HouseId == Layout.BossHouse ? 1 : 0, RoundCm(M.P.X), RoundCm(M.P.Y)));
	}
	TArray<FString> Houses;
	Houses.Add(TEXT("house_id,floor_id,x0,x1,y0,y1"));
	for (const FS1LevelHouse& H : Layout.Houses)
	{
		Houses.Add(FString::Printf(TEXT("%s,%s,%d,%d,%d,%d"), *H.Id, *Floor, RoundCm(H.X0), RoundCm(H.X1), RoundCm(H.Y0), RoundCm(H.Y1)));
	}

	// Server walls (docs/architecture/adr-0006-server-2d-walls.md). The builder must sit at the origin (E5) and nothing
	// outside the builder may block characters (E7) — otherwise the server's walls are not the walls players see.
	if (!GetActorTransform().Equals(FTransform::Identity, 0.01))
	{
		OutError = TEXT("the builder actor is moved, rotated or scaled — press Build Graybox, it puts the builder back at the origin (ADR-0006 E5)");
		return false;
	}
	// The collision is instance x component transform — a module component moved inside the builder shifts its rows too (review D2)
	for (const UHierarchicalInstancedStaticMeshComponent* Component : Modules)
	{
		if (Component != nullptr && !Component->GetComponentTransform().Equals(FTransform::Identity, 0.01))
		{
			OutError = FString::Printf(TEXT("module component %s is moved, rotated or scaled inside the builder — reset it to the origin (ADR-0006 E5)"),
				*Component->GetName());
			return false;
		}
	}
	TArray<FString> Blockers;
	FindClientOnlyBlockers(Blockers);
	if (Blockers.Num() > 0)
	{
		OutError = FString::Printf(TEXT("%d primitive(s) block characters above %.0f cm but are not server walls — make them builder modules, "
			"turn their collision off or keep them under %.0f cm (ADR-0006 E7): %s"),
			Blockers.Num(), StepHeightCm, StepHeightCm, *FString::Join(Blockers, TEXT(", ")));
		return false;
	}
	TArray<FString> WallRows;
	if (!CollectServerWalls(WallRows, OutError))
	{
		return false;
	}
	TArray<FString> Walls;
	Walls.Add(TEXT("floor_id,kind,cx,cy,half_length,half_thickness,yaw"));
	for (const FString& Row : WallRows)
	{
		Walls.Add(Floor + TEXT(",") + Row);
	}

	// The coast polygon (2026-10-11 island) — the server keeps every position inside it, also where walls are skipped
	// (the ghost dash) or not swept (blink · teleports). No rows = the square
	TArray<FString> Outline;
	Outline.Add(TEXT("floor_id,seq,x,y"));
	for (int32 i = 0; i < Layout.Outline.Num(); i++)
	{
		Outline.Add(FString::Printf(TEXT("%s,%d,%d,%d"), *Floor, i, RoundCm(Layout.Outline[i].X), RoundCm(Layout.Outline[i].Y)));
	}

	TArray<TPair<FString, FString>> Files;
	Files.Emplace(TEXT("entry_points.csv"), Join(Entries));
	Files.Emplace(TEXT("chest_spawns.csv"), Join(Chests));
	Files.Emplace(TEXT("towers.csv"), Join(Towers));
	Files.Emplace(TEXT("monster_spots.csv"), Join(Monsters));
	Files.Emplace(TEXT("houses.csv"), Join(Houses));
	Files.Emplace(TEXT("walls.csv"), Join(Walls));
	Files.Emplace(TEXT("floor_outline.csv"), Join(Outline));

	// Data version (Core Rule 16): hash of the content with line endings normalised.
	FString All;
	for (const TPair<FString, FString>& File : Files)
	{
		All += File.Key + TEXT("\n") + File.Value.Replace(TEXT("\r\n"), TEXT("\n"));
	}
	LastDataVersion = FMD5::HashAnsiString(*All).Left(12);

	OutDirectory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), ExportDirectory, FString::Printf(TEXT("floor%d"), FloorId)));
	IFileManager::Get().MakeDirectory(*OutDirectory, /*Tree*/ true);
	for (const TPair<FString, FString>& File : Files)
	{
		if (!FFileHelper::SaveStringToFile(File.Value, *FPaths::Combine(OutDirectory, File.Key), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("cannot write %s"), *File.Key);
			return false;
		}
	}
	// The version is the hash of the files above, so it is written after them and is not part of the hash.
	// level_versions.csv also tells the server this floor has a level — then it refuses to boot without its walls.
	TArray<FString> Versions;
	Versions.Add(TEXT("floor_id,data_version"));
	Versions.Add(Floor + TEXT(",") + LastDataVersion);
	if (!FFileHelper::SaveStringToFile(Join(Versions), *FPaths::Combine(OutDirectory, TEXT("level_versions.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = TEXT("cannot write level_versions.csv");
		return false;
	}
	IFileManager::Get().Delete(*FPaths::Combine(OutDirectory, TEXT("data_version.txt")), /*RequireExists*/ false, /*EvenReadOnly*/ false, /*Quiet*/ true);

	UE_LOG(LogS1Level, Display, TEXT("[LEVEL] server walls %d → walls.csv"), WallRows.Num());
	MarkPackageDirty();
	return true;
}

bool AS1LevelBuilder::CollectServerWalls(TArray<FString>& OutRows, FString& OutError) const
{
	using namespace S1LevelBuild;

	struct FRow
	{
		FString	Kind;
		int32	X = 0;
		int32	Y = 0;
		int32	YawCenti = 0;			// hundredths of a degree, [0, 18000)
		int32	HalfLength = 0;
		int32	HalfThickness = 0;
	};
	TArray<FRow> Rows;

	for (const FServerWallModule& Spec : ServerWallModules)
	{
		const UHierarchicalInstancedStaticMeshComponent* Component = Modules[static_cast<int32>(Spec.Module)];
		if (Component == nullptr || Component->GetInstanceCount() == 0)
		{
			continue;
		}

		// E1–E2: the box comes from the mesh's simple collision — what the character capsule actually hits
		const UStaticMesh* Mesh = Component->GetStaticMesh();
		const UBodySetup* Body = Mesh != nullptr ? Mesh->GetBodySetup() : nullptr;
		const FString MeshName = Mesh != nullptr ? Mesh->GetPathName() : TEXT("(none)");
		if (Body == nullptr)
		{
			OutError = FString::Printf(TEXT("%s mesh %s has no collision (ADR-0006 E2)"), ModuleName(Spec.Module), *MeshName);
			return false;
		}
		if (Body->GetCollisionTraceFlag() == CTF_UseComplexAsSimple)
		{
			OutError = FString::Printf(TEXT("%s mesh %s uses complex collision as simple — give it one simple box (ADR-0006 E2)"), ModuleName(Spec.Module), *MeshName);
			return false;
		}
		const FKAggregateGeom& Geom = Body->AggGeom;
		if (Geom.BoxElems.Num() != 1 || Geom.GetElementCount() != 1)
		{
			OutError = FString::Printf(TEXT("%s mesh %s has %d simple shape(s), %d box(es) — a server wall needs exactly one box (ADR-0006 E2)"),
				ModuleName(Spec.Module), *MeshName, Geom.GetElementCount(), Geom.BoxElems.Num());
			return false;
		}
		const FKBoxElem& Box = Geom.BoxElems[0];
		if (!Box.Rotation.IsNearlyZero(AngleToleranceDeg))
		{
			OutError = FString::Printf(TEXT("%s mesh %s has a rotated collision box — keep it axis-aligned in the mesh (ADR-0006 E2)"), ModuleName(Spec.Module), *MeshName);
			return false;
		}

		for (int32 i = 0; i < Component->GetInstanceCount(); i++)
		{
			FTransform T;
			Component->GetInstanceTransform(i, T, /*bWorldSpace*/ true);
			const FRotator R = T.Rotator();
			if (FMath::Abs(R.Pitch) > AngleToleranceDeg || FMath::Abs(R.Roll) > AngleToleranceDeg)
			{
				OutError = FString::Printf(TEXT("%s #%d is tilted (pitch %.2f, roll %.2f) — tilt the mesh shape, not the instance (ADR-0006 E6)"),
					ModuleName(Spec.Module), i, R.Pitch, R.Roll);
				return false;
			}

			// Chaos scales a box element along its own axes (FKBoxElem::GetFinalScaled) — with the element unrotated (E2)
			// that is exactly half = (X·Sx, Y·Sy) / 2 and centre = T(C). A zero or mirrored scale is refused (review B1)
			const FVector RawScale = T.GetScale3D();
			if (RawScale.X <= 0.0 || RawScale.Y <= 0.0 || RawScale.Z <= 0.0)
			{
				OutError = FString::Printf(TEXT("%s #%d has scale %s — zero or mirrored scale is not allowed on a server wall (ADR-0006 E1)"),
					ModuleName(Spec.Module), i, *RawScale.ToString());
				return false;
			}

			// FKBoxElem X / Y are full lengths, not half (BodySetup.cpp halves them the same way)
			const FVector Scale = RawScale;
			const double HalfLength = Box.X * 0.5 * Scale.X;
			const double HalfThickness = Box.Y * 0.5 * Scale.Y;
			const FVector Centre = T.TransformPosition(Box.Center);

			// E3: the layout rules (3 m door gaps, wall lines) assume the design size
			if (!Spec.DesignSize.IsZero()
				&& (FMath::Abs(HalfLength * 2.0 - Spec.DesignSize.X) > SizeToleranceCm || FMath::Abs(HalfThickness * 2.0 - Spec.DesignSize.Y) > SizeToleranceCm))
			{
				OutError = FString::Printf(TEXT("%s #%d collision box is %.1f x %.1f cm, the design size is %.0f x %.0f cm (ADR-0006 E3)"),
					ModuleName(Spec.Module), i, HalfLength * 2.0, HalfThickness * 2.0, Spec.DesignSize.X, Spec.DesignSize.Y);
				return false;
			}

			// E9: a box is the same box turned 180°, so the yaw is written in [0, 180) with two decimals
			int32 YawCenti = static_cast<int32>(FMath::RoundToInt(FMath::Fmod(R.Yaw, 180.0) * 100.0));
			if (YawCenti < 0)
			{
				YawCenti += 18000;
			}
			if (YawCenti >= 18000)
			{
				YawCenti -= 18000;
			}

			// RoundToInt(double) is int64 in UE5 — every value here is well inside ±30,000 cm (FloorBounds)
			Rows.Add({ Spec.Kind, static_cast<int32>(FMath::RoundToInt(Centre.X)), static_cast<int32>(FMath::RoundToInt(Centre.Y)), YawCenti,
				static_cast<int32>(FMath::RoundToInt(HalfLength)), static_cast<int32>(FMath::RoundToInt(HalfThickness)) });
		}
	}

	// E9: the instance order is not part of the data — sort so the same level always writes the same bytes (AC-LM-20)
	// Every column is in the key (review D4) — two rows that tie on position and yaw still sort the same way every time
	Rows.Sort([](const FRow& A, const FRow& B)
	{
		if (A.Kind != B.Kind)					return A.Kind < B.Kind;
		if (A.X != B.X)							return A.X < B.X;
		if (A.Y != B.Y)							return A.Y < B.Y;
		if (A.YawCenti != B.YawCenti)			return A.YawCenti < B.YawCenti;
		if (A.HalfLength != B.HalfLength)		return A.HalfLength < B.HalfLength;
		return A.HalfThickness < B.HalfThickness;
	});

	OutRows.Reset(Rows.Num());
	for (const FRow& Row : Rows)
	{
		OutRows.Add(FString::Printf(TEXT("%s,%d,%d,%d,%d,%d.%02d"), *Row.Kind, Row.X, Row.Y, Row.HalfLength, Row.HalfThickness,
			Row.YawCenti / 100, Row.YawCenti % 100));
	}
	return true;
}

void AS1LevelBuilder::FindClientOnlyBlockers(TArray<FString>& OutNames) const
{
	using namespace S1LevelBuild;

	OutNames.Reset();
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		// The builder's modules are server walls (the coast too) or walkable floor
		if (It->IsA<AS1LevelBuilder>())
		{
			continue;
		}
		// Every level has a default BSP builder brush — an editor tool, not geometry. Blocking volumes still count
		if (const ULevel* Level = It->GetLevel(); Level != nullptr && Level->GetDefaultBrush() == *It)
		{
			continue;
		}

		TInlineComponentArray<UPrimitiveComponent*> Primitives(*It);
		for (const UPrimitiveComponent* Primitive : Primitives)
		{
			if (!Primitive->IsCollisionEnabled() || Primitive->GetCollisionResponseToChannel(ECC_Pawn) != ECR_Block)
			{
				continue;
			}
			if (Primitive->Bounds.GetBox().Max.Z <= StepHeightCm)
			{
				continue;	// floor-level dressing — walkable (Core Rule 2)
			}
			OutNames.Add(FString::Printf(TEXT("%s.%s"), *It->GetActorNameOrLabel(), *Primitive->GetName()));
		}
	}
}
