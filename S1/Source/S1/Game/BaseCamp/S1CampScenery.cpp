// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/BaseCamp/S1CampScenery.h"
#include "Game/BaseCamp/S1CampLayout.h"
#include "Game/S1Player.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Protocol.pb.h"
#include "UObject/ConstructorHelpers.h"

namespace S1CampLook
{
	// Ground top matches Floor1's ground (centre -20, 20 cm thick → top -10) so the pawn's feet sit the same way
	constexpr float GroundCentreZ = -20.f;
	constexpr float GroundThicknessCm = 20.f;
	constexpr float GroundMarginCm = 400.f;			// a little earth past the fence
	constexpr float SeaSizeCm = 14000.f;			// 140 m of sea under the camp - the camera never finds the edge
	constexpr float FencePostStepCm = 250.f;
	constexpr float FenceHeightCm = 110.f;
	constexpr float GateGapPadCm = 40.f;

	const FLinearColor Earth(0.30f, 0.25f, 0.18f);
	const FLinearColor Path(0.46f, 0.40f, 0.30f);
	const FLinearColor Wood(0.34f, 0.22f, 0.12f);
	const FLinearColor Iron(0.18f, 0.18f, 0.20f);
	const FLinearColor Crate(0.45f, 0.31f, 0.16f);
	const FLinearColor Banner(0.55f, 0.08f, 0.08f);
	const FLinearColor SeaFallback(0.015f, 0.07f, 0.16f);

	/** NPC object ids — far above every server band (players · bags 2e9 · monsters 4e9 · projectiles 5e9 · decoys 6e9). */
	constexpr uint64 NpcIdBase = 9'000'000'000'000'000'000ull;

	/** Who holds what — the blacksmith a hammer, the others empty-handed (item_types.csv 102 = hammer). */
	uint32 WeaponOf(const FString& NpcId)
	{
		return NpcId == TEXT("enhance") ? 102u : 0u;		// 매입 행수(trade)는 빈손 - 장부를 든 상인
	}
}

AS1CampScenery::AS1CampScenery()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cylinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> Basic(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	CubeMesh = Cube.Object;
	CylinderMesh = Cylinder.Object;
	BaseMaterial = Basic.Object;
}

UStaticMeshComponent* AS1CampScenery::AddBox(const FVector& CenterCm, const FVector& SizeCm, float YawDeg, const FLinearColor& Color, bool bCollide,
	UMaterialInterface* Material, bool bCylinder)
{
	UStaticMeshComponent* Box = NewObject<UStaticMeshComponent>(this);
	Box->SetStaticMesh(bCylinder ? CylinderMesh : CubeMesh);
	Box->SetupAttachment(RootComponent);
	Box->SetWorldLocationAndRotation(CenterCm, FRotator(0.f, YawDeg, 0.f));
	Box->SetWorldScale3D(SizeCm / 100.f);			// the basic shapes are 100 cm
	Box->SetCollisionProfileName(bCollide ? TEXT("BlockAll") : TEXT("NoCollision"));
	Box->CanCharacterStepUpOn = ECB_No;

	if (Material != nullptr)
	{
		Box->SetMaterial(0, Material);
	}
	else if (BaseMaterial != nullptr)
	{
		// The same runtime tint the monster disc uses (AS1Player::SetCreatureInfo) - fine on a plain component
		UMaterialInstanceDynamic* Tint = UMaterialInstanceDynamic::Create(BaseMaterial, this);
		Tint->SetVectorParameterValue(TEXT("Color"), Color);
		Box->SetMaterial(0, Tint);
	}

	Box->RegisterComponent();
	return Box;
}

void AS1CampScenery::Build(const FS1CampLayout& Layout, TSubclassOf<AS1Player> BodyClass)
{
	using namespace S1CampLook;
	if (bBuilt || Layout.IsLoaded() == false)
		return;
	bBuilt = true;

	const FS1CampPoint& Area = Layout.GetArea();
	const FVector Centre(Area.X, Area.Y, 0.f);

	// ── Sea and ground ──
	UMaterialInterface* Sea = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Level/M_S1Sea.M_S1Sea"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	AddBox(Centre + FVector(0.f, 0.f, GroundCentreZ - 30.f), FVector(SeaSizeCm, SeaSizeCm, GroundThicknessCm), 0.f, SeaFallback, false, Sea);
	AddBox(Centre + FVector(0.f, 0.f, GroundCentreZ), FVector(Area.HalfX * 2.f + GroundMarginCm, Area.HalfY * 2.f + GroundMarginCm, GroundThicknessCm),
		0.f, Earth, true);

	// A trodden path from where we stand to the gate - the eye goes where the feet should
	const FS1CampPoint& Spawn = Layout.GetSpawn();
	if (const FS1CampPoint* Gate = Layout.FindNpc(TEXT("gate")))
	{
		const FVector2D A(Spawn.X, Spawn.Y);
		const FVector2D B(Gate->X - 150.f, Gate->Y);
		const FVector2D Mid = (A + B) * 0.5f;
		const float Length = FVector2D::Distance(A, B);
		const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(B.Y - A.Y, B.X - A.X));
		AddBox(FVector(Mid.X, Mid.Y, GroundCentreZ + 11.f), FVector(Length, 220.f, 2.f), Yaw, Path, false);
	}

	// ── Fence on the walkable square - the server's coast. A gap at the gate posts on the north edge ──
	float GapMin = TNumericLimits<float>::Max();
	float GapMax = -TNumericLimits<float>::Max();
	for (const FS1CampPoint& Prop : Layout.GetProps())
	{
		if (Prop.Id.StartsWith(TEXT("gate_post")))
		{
			GapMin = FMath::Min(GapMin, Prop.Y - Prop.HalfY - GateGapPadCm);
			GapMax = FMath::Max(GapMax, Prop.Y + Prop.HalfY + GateGapPadCm);
		}
	}

	const float X0 = Area.X - Area.HalfX, X1 = Area.X + Area.HalfX;
	const float Y0 = Area.Y - Area.HalfY, Y1 = Area.Y + Area.HalfY;
	auto Post = [this](float X, float Y) { AddBox(FVector(X, Y, FenceHeightCm * 0.5f - 10.f), FVector(22.f, 22.f, FenceHeightCm), 0.f, Wood, false); };
	auto Rail = [this](float Xa, float Ya, float Xb, float Yb)
		{
			const float Length = FVector2D::Distance(FVector2D(Xa, Ya), FVector2D(Xb, Yb));
			if (Length < 1.f)
				return;
			const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(Yb - Ya, Xb - Xa));
			for (float Z : { 45.f, 85.f })
				AddBox(FVector((Xa + Xb) * 0.5f, (Ya + Yb) * 0.5f, Z), FVector(Length, 8.f, 10.f), Yaw, Wood, false);
		};

	// West / east edges (along X) and the south edge - whole
	for (float Y : { Y0, Y1 })
	{
		for (float X = X0; X <= X1 + 1.f; X += FencePostStepCm)
			Post(X, Y);
		Rail(X0, Y, X1, Y);
	}
	for (float Y = Y0; Y <= Y1 + 1.f; Y += FencePostStepCm)
		Post(X0, Y);
	Rail(X0, Y0, X0, Y1);

	// North edge - the gate gap
	for (float Y = Y0; Y <= Y1 + 1.f; Y += FencePostStepCm)
	{
		if (Y > GapMin && Y < GapMax)
			continue;
		Post(X1, Y);
	}
	if (GapMin < GapMax)
	{
		Rail(X1, Y0, X1, GapMin);
		Rail(X1, GapMax, X1, Y1);
	}
	else
	{
		Rail(X1, Y0, X1, Y1);
	}

	// ── Props - the server blocks the same boxes ──
	for (const FS1CampPoint& Prop : Layout.GetProps())
	{
		const FVector At(Prop.X, Prop.Y, 0.f);
		const FVector Foot(Prop.HalfX * 2.f, Prop.HalfY * 2.f, 0.f);
		if (Prop.Id == TEXT("anvil"))
		{
			AddBox(At + FVector(0.f, 0.f, 25.f), FVector(Foot.X * 0.7f, Foot.Y * 0.7f, 60.f), Prop.Yaw, Wood, true, nullptr, true);	// stump
			AddBox(At + FVector(0.f, 0.f, 70.f), FVector(Foot.X, Foot.Y * 0.8f, 30.f), Prop.Yaw, Iron, true);						// anvil
		}
		else if (Prop.Id.StartsWith(TEXT("chest")))
		{
			AddBox(At + FVector(0.f, 0.f, 30.f), FVector(Foot.X, Foot.Y, 60.f), Prop.Yaw, Crate, true);
			AddBox(At + FVector(10.f, -15.f, 85.f), FVector(Foot.X * 0.6f, Foot.Y * 0.7f, 50.f), Prop.Yaw + 8.f, Crate * 0.85f, true);
		}
		else if (Prop.Id.StartsWith(TEXT("gate_post")))
		{
			AddBox(At + FVector(0.f, 0.f, 150.f), FVector(Foot.X, Foot.Y, 320.f), Prop.Yaw, Wood, true);
		}
		else
		{
			AddBox(At + FVector(0.f, 0.f, 40.f), FVector(Foot.X, Foot.Y, 80.f), Prop.Yaw, Crate, true);
		}
	}

	// Lintel and a red banner over the gate gap
	if (GapMin < GapMax)
	{
		const float Mid = (GapMin + GapMax) * 0.5f;
		AddBox(FVector(X1, Mid, 300.f), FVector(30.f, GapMax - GapMin + 40.f, 30.f), 0.f, Wood, false);
		AddBox(FVector(X1 - 5.f, Mid, 240.f), FVector(6.f, (GapMax - GapMin) * 0.55f, 90.f), 0.f, Banner, false);
	}

	// ── NPC bodies ──
	UWorld* World = GetWorld();
	if (World == nullptr || BodyClass == nullptr)
		return;

	for (int32 i = 0; i < Layout.GetNpcs().Num(); i++)
	{
		const FS1CampPoint& Npc = Layout.GetNpcs()[i];

		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		const FVector At(Npc.X, Npc.Y, 100.f);
		AS1Player* Body = World->SpawnActor<AS1Player>(BodyClass, At, FRotator(0.f, Npc.Yaw, 0.f), Params);
		if (Body == nullptr)
			continue;

		Protocol::PosInfo Pos;
		Pos.set_object_id(NpcIdBase + i);
		Pos.set_x(Npc.X);
		Pos.set_y(Npc.Y);
		Pos.set_z(100.f);
		Pos.set_yaw(Npc.Yaw);
		Pos.set_state(Protocol::MOVE_STATE_IDLE);
		Body->SetPlayerInfo(Pos);

		Protocol::ObjectInfo Info;
		Info.set_object_id(NpcIdBase + i);
		Info.set_creature_type(Protocol::CREATE_TYPE_NPC);
		Info.set_weapon_type_id(WeaponOf(Npc.Id));
		Body->SetCreatureInfo(Info);
		Body->SetActorRotation(FRotator(0.f, Npc.Yaw, 0.f));
		Body->SetCombatDebugShown(false);		// no HP bar over a shopkeeper

		NpcBodies.Add({ Npc.Id, Body });
	}

	UE_LOG(LogTemp, Log, TEXT("[CAMP] scenery built - area (%.0f, %.0f) %.0fx%.0f cm, props %d, npcs %d"),
		Area.X, Area.Y, Area.HalfX * 2.f, Area.HalfY * 2.f, Layout.GetProps().Num(), NpcBodies.Num());
}

void AS1CampScenery::SetShown(bool bShown)
{
	SetActorHiddenInGame(bShown == false);
	for (const FS1CampNpcBody& Npc : NpcBodies)
	{
		if (AS1Player* Body = Npc.Body.Get())
		{
			Body->SetActorHiddenInGame(bShown == false);
			// 숨겨도 틱은 돌았다(애니메이션 · 보간) - 던전에 있는 내내 NPC 네 명이 포즈를 계산했다. 같이 멈추고 같이 깨운다 (2026-10-17)
			Body->SetActorTickEnabled(bShown);
			if (USkeletalMeshComponent* Mesh = Body->GetMesh())
				Mesh->SetComponentTickEnabled(bShown);
			if (UCharacterMovementComponent* Movement = Body->GetCharacterMovement())
				Movement->SetComponentTickEnabled(bShown);
		}
	}
}

void AS1CampScenery::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	for (const FS1CampNpcBody& Npc : NpcBodies)
	{
		if (AS1Player* Body = Npc.Body.Get())
			Body->Destroy();
	}
	NpcBodies.Reset();
	Super::EndPlay(EndPlayReason);
}
