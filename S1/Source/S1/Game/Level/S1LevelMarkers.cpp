// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/Level/S1LevelMarkers.h"

#include "Components/BoxComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/SplineComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"

namespace S1LevelMarkerUnits
{
	constexpr double Cm = 100.0;		// layout metres → world cm
	constexpr double MarkerZ = 50.0;	// markers float half a metre above the floor
}

// ── Base ──────────────────────────────────────────────────────────────────────

AS1LevelMarker::AS1LevelMarker()
{
	PrimaryActorTick.bCanEverTick = false;
	bIsEditorOnlyActor = true;		// stripped from the game — nothing marks these spots in the world
	Tags.Add(TEXT("S1LevelMarker"));

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;

	Label = CreateDefaultSubobject<UTextRenderComponent>(TEXT("Label"));
	Label->SetupAttachment(Root);
	Label->SetHorizontalAlignment(EHTA_Center);
	Label->SetWorldSize(200.f);
	Label->SetRelativeLocation(FVector(0.0, 0.0, 300.0));
	Label->SetRelativeRotation(FRotator(90.0, 0.0, 180.0));		// readable from the top-down camera
	Label->bIsEditorOnly = true;
	Label->SetHiddenInGame(true);
}

void AS1LevelMarker::SetMarkerLabel(const FString& Text, FColor Color)
{
	Label->SetText(FText::FromString(Text));
	Label->SetTextRenderColor(Color);
}

FVector2D AS1LevelMarker::FloorMetres() const
{
	const FVector L = GetActorLocation();
	return FVector2D(L.X / S1LevelMarkerUnits::Cm, L.Y / S1LevelMarkerUnits::Cm);
}

AS1EntryPointMarker::AS1EntryPointMarker()
{
}

// ── House / room volumes ──────────────────────────────────────────────────────

namespace
{
	void SetBoxRect(AActor* Actor, UBoxComponent* Box, double X0, double X1, double Y0, double Y1, double HeightCm)
	{
		using namespace S1LevelMarkerUnits;
		Actor->SetActorLocation(FVector((X0 + X1) * 0.5 * Cm, (Y0 + Y1) * 0.5 * Cm, HeightCm * 0.5));
		Box->SetBoxExtent(FVector((X1 - X0) * 0.5 * Cm, (Y1 - Y0) * 0.5 * Cm, HeightCm * 0.5));
	}
}

AS1HouseVolume::AS1HouseVolume()
{
	Box = CreateDefaultSubobject<UBoxComponent>(TEXT("Box"));
	Box->SetupAttachment(Root);
	Box->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Box->ShapeColor = FColor(255, 170, 60);
	Box->bIsEditorOnly = true;
}

void AS1HouseVolume::SetRectMetres(double X0, double X1, double Y0, double Y1)
{
	SetBoxRect(this, Box, X0, X1, Y0, Y1, 400.0);
}

void AS1HouseVolume::GetRectMetres(double& X0, double& X1, double& Y0, double& Y1) const
{
	using namespace S1LevelMarkerUnits;
	const FVector C = GetActorLocation();
	const FVector E = Box->GetScaledBoxExtent();
	X0 = (C.X - E.X) / Cm;
	X1 = (C.X + E.X) / Cm;
	Y0 = (C.Y - E.Y) / Cm;
	Y1 = (C.Y + E.Y) / Cm;
}

AS1RoomVolume::AS1RoomVolume()
{
	Box = CreateDefaultSubobject<UBoxComponent>(TEXT("Box"));
	Box->SetupAttachment(Root);
	Box->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Box->ShapeColor = FColor(120, 200, 255);
	Box->bIsEditorOnly = true;
}

void AS1RoomVolume::SetRectMetres(double X0, double X1, double Y0, double Y1)
{
	SetBoxRect(this, Box, X0, X1, Y0, Y1, 300.0);
}

// ── Splines ───────────────────────────────────────────────────────────────────

namespace
{
	void SetSplineMetres(USplineComponent* Spline, const TArray<FVector2D>& Points, double ZCm)
	{
		Spline->ClearSplinePoints(false);
		for (const FVector2D& P : Points)
		{
			Spline->AddSplinePoint(FVector(P.X * S1LevelMarkerUnits::Cm, P.Y * S1LevelMarkerUnits::Cm, ZCm),
				ESplineCoordinateSpace::World, false);
		}
		// Straight segments — trails and routes are polylines in the layout, not curves.
		for (int32 i = 0; i < Spline->GetNumberOfSplinePoints(); i++)
		{
			Spline->SetSplinePointType(i, ESplinePointType::Linear, false);
		}
		Spline->UpdateSpline();
	}

	TArray<FVector2D> GetSplineMetres(const USplineComponent* Spline)
	{
		TArray<FVector2D> Out;
		for (int32 i = 0; i < Spline->GetNumberOfSplinePoints(); i++)
		{
			const FVector P = Spline->GetLocationAtSplinePoint(i, ESplineCoordinateSpace::World);
			Out.Add(FVector2D(P.X / S1LevelMarkerUnits::Cm, P.Y / S1LevelMarkerUnits::Cm));
		}
		return Out;
	}
}

AS1RouteSpline::AS1RouteSpline()
{
	Spline = CreateDefaultSubobject<USplineComponent>(TEXT("Spline"));
	Spline->SetupAttachment(Root);
	Spline->bIsEditorOnly = true;
#if WITH_EDITORONLY_DATA
	Spline->EditorUnselectedSplineSegmentColor = FLinearColor(0.3f, 0.7f, 0.3f);
#endif
}

void AS1RouteSpline::SetPointsMetres(const TArray<FVector2D>& Points)
{
	SetActorLocation(FVector::ZeroVector);
	SetSplineMetres(Spline, Points, S1LevelMarkerUnits::MarkerZ);
}

TArray<FVector2D> AS1RouteSpline::GetPointsMetres() const
{
	return GetSplineMetres(Spline);
}

AS1TrailSpline::AS1TrailSpline()
{
	PrimaryActorTick.bCanEverTick = false;
	Tags.Add(TEXT("S1LevelTrail"));

	Spline = CreateDefaultSubobject<USplineComponent>(TEXT("Spline"));
	RootComponent = Spline;

	Surface = CreateDefaultSubobject<UHierarchicalInstancedStaticMeshComponent>(TEXT("Surface"));
	Surface->SetupAttachment(Spline);
	Surface->SetCollisionEnabled(ECollisionEnabled::NoCollision);	// walkable dirt, not a step

	Lanterns = CreateDefaultSubobject<UHierarchicalInstancedStaticMeshComponent>(TEXT("Lanterns"));
	Lanterns->SetupAttachment(Spline);
	Lanterns->SetCollisionEnabled(ECollisionEnabled::NoCollision);	// lanterns do not block — not server walls

	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cylinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	SurfaceMesh = Cube.Object;
	LanternMesh = Cylinder.Object;
}

void AS1TrailSpline::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	RebuildDressing();
}

void AS1TrailSpline::SetPointsMetres(const TArray<FVector2D>& Points)
{
	SetActorLocation(FVector::ZeroVector);
	SetSplineMetres(Spline, Points, 2.0);
	RebuildDressing();
}

TArray<FVector2D> AS1TrailSpline::GetPointsMetres() const
{
	return GetSplineMetres(Spline);
}

int32 AS1TrailSpline::NumLanterns() const
{
	return Lanterns->GetInstanceCount();
}

void AS1TrailSpline::RebuildDressing()
{
	using namespace S1LevelMarkerUnits;

	Surface->ClearInstances();
	Lanterns->ClearInstances();
	Surface->SetStaticMesh(SurfaceMesh);
	Lanterns->SetStaticMesh(LanternMesh);

	const int32 Num = Spline->GetNumberOfSplinePoints();
	if (Num < 2 || SurfaceMesh == nullptr)
	{
		return;
	}

	// One flat piece per straight segment (the basic cube is 1 m on a side, pivot at its centre).
	for (int32 i = 0; i + 1 < Num; i++)
	{
		const FVector A = Spline->GetLocationAtSplinePoint(i, ESplineCoordinateSpace::World);
		const FVector B = Spline->GetLocationAtSplinePoint(i + 1, ESplineCoordinateSpace::World);
		const FVector D = B - A;
		const double Length = D.Size2D();
		if (Length < 1.0)
		{
			continue;
		}
		const FRotator Yaw(0.0, FMath::RadiansToDegrees(FMath::Atan2(D.Y, D.X)), 0.0);
		const FVector Mid = (A + B) * 0.5;
		Surface->AddInstance(FTransform(Yaw, FVector(Mid.X, Mid.Y, 1.0), FVector(Length / 100.0, Width, 0.04)), /*bWorldSpace*/ true);
	}

	// Lanterns every LanternSpacing metres along the whole trail, both ends included (F2).
	if (bLantern && LanternMesh != nullptr)
	{
		const double Total = Spline->GetSplineLength();
		const double Step = FMath::Max(LanternSpacing, 2.0) * Cm;
		const int32 Count = FMath::FloorToInt(Total / Step + 1e-6);
		for (int32 k = 0; k <= Count; k++)
		{
			const float Distance = static_cast<float>(FMath::Min(k * Step, Total));
			FVector P = Spline->GetLocationAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World);
			const FVector Side = Spline->GetRightVectorAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World);
			P += Side * (Width * 0.5 * Cm + 30.0);		// at the trail's edge, not in the walking line
			Lanterns->AddInstance(FTransform(FRotator::ZeroRotator, FVector(P.X, P.Y, 100.0), FVector(0.25, 0.25, 2.0)), true);
		}
	}
}
