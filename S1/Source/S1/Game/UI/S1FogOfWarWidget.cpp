// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/UI/S1FogOfWarWidget.h"

#include "Blueprint/WidgetLayoutLibrary.h"
#include "Components/CapsuleComponent.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Framework/Application/SlateApplication.h"
#include "Game/BaseCamp/S1BaseCampSubsystem.h"
#include "Game/Escape/S1EscapeSubsystem.h"
#include "Game/S1Player.h"
#include "Game/Vision/S1VisionSubsystem.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "S1GameInstance.h"
#include "SceneView.h"
#include "Styling/CoreStyle.h"

namespace
{
	// Grid cell in widget units - small enough that the soft edge reads as a curve, large enough to stay cheap
	// (1280 x 720 at 16 -> 81 x 46 = 3,726 vertices). The edge ramp below hides the cell size
	constexpr float CellUnits = 16.f;

	constexpr int32 OwnRays = 240;			// 1.5 degrees - 39 cm apart at 15 m, under the feather
	constexpr int32 ExtraRays = 120;
	constexpr float FeatherCm = 90.f;		// inside the sight edge by this much = fully seen
	constexpr float RevealHoleCm = 150.f;	// a known creature outside my sight shows through a hole this size (feather included)
}

void US1FogOfWarWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	SetVisibility(ESlateVisibility::HitTestInvisible);		// clicks still reach the world
}

float US1FogOfWarWidget::FogAt(const FVector2D& Ground) const
{
	float Seen = 0.f;

	for (const FSourceView& View : Views)
	{
		const FVector2D D = Ground - View.Pos;
		const float Dist = static_cast<float>(D.Size());
		if (Dist > View.RadiusCm)
			continue;

		float Limit = View.RadiusCm;
		const int32 N = View.Rays.Num();
		if (N > 0)
		{
			float Angle = FMath::Atan2(static_cast<float>(D.Y), static_cast<float>(D.X));
			if (Angle < 0.f)
				Angle += 2.f * PI;

			// Between two rays take the shorter one - never show more than the server lets through
			const float F = Angle / (2.f * PI) * static_cast<float>(N);
			const int32 I0 = FMath::FloorToInt(F) % N;
			const int32 I1 = (I0 + 1) % N;
			Limit = FMath::Min(View.Rays[I0], View.Rays[I1]);
		}

		Seen = FMath::Max(Seen, FMath::Clamp((Limit - Dist) / FeatherCm, 0.f, 1.f));
		if (Seen >= 1.f)
			break;
	}

	return 1.f - Seen;
}

void US1FogOfWarWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	bHasFog = false;

	US1VisionSubsystem* Vision = US1VisionSubsystem::Get();
	const US1EscapeSubsystem* Escape = US1EscapeSubsystem::Get();
	const bool bInDungeon = Escape != nullptr && Escape->InDungeon();

	// Leaving the dungeon (escape, death, expiry) drops the fog and gives the camera back its normal distance
	if (bWasInDungeon && bInDungeon == false && Vision != nullptr)
		Vision->Clear();
	bWasInDungeon = bInDungeon;

	if (bInDungeon == false || Vision == nullptr || Vision->IsActive() == false)
		return;

	const US1GameInstance* GameInstance = Cast<US1GameInstance>(GetGameInstance());
	const AS1Player* Me = GameInstance != nullptr ? GameInstance->MyPlayer.Get() : nullptr;
	APlayerController* PC = GetOwningPlayer();
	if (Me == nullptr || PC == nullptr)
		return;

	const US1BaseCampSubsystem* BaseCamp = US1BaseCampSubsystem::Get();
	Vision->EnsureWalls(BaseCamp != nullptr ? BaseCamp->GetFloorId() : 0);

	// ── Sources: my sight, then the server's extras (clone / hawk / search) ──
	const FVector Feet = Me->GetActorLocation();
	Views.Reset();
	{
		FSourceView& Own = Views.AddDefaulted_GetRef();
		Own.Pos = FVector2D(Feet.X, Feet.Y);
		Own.RadiusCm = Vision->GetRadiusCm();
		Vision->CastRays(Own.Pos, Own.RadiusCm, OwnRays, Own.Rays);
	}

	const double Now = FPlatformTime::Seconds();
	for (const FS1VisionSource& Source : Vision->GetSources())
	{
		if (Source.Until > 0.0 && Now >= Source.Until)
			continue;

		FVector2D Pos = Source.Pos;
		if (Source.FollowId != 0)
		{
			const TWeakObjectPtr<AS1Player>* Found = GameInstance->Players.Find(Source.FollowId);
			const AS1Player* Followed = Found != nullptr ? Found->Get() : nullptr;
			if (Followed == nullptr)
				continue;
			Pos = FVector2D(Followed->GetActorLocation().X, Followed->GetActorLocation().Y);
		}

		FSourceView& View = Views.AddDefaulted_GetRef();
		View.Pos = Pos;
		View.RadiusCm = Source.RadiusCm;
		if (Source.bNeedsLos)
			Vision->CastRays(View.Pos, View.RadiusCm, ExtraRays, View.Rays);
	}

	// Creatures this client knows but my sight does not cover - revealed by a hit, 외로운 외침, the hawk's far flight. Only
	// the creature shows through the dark, not the ground around it (2026-10-11 "암흑 시야에서 적만 잠깐 보였으면"). The
	// server decides who is known (S_SPAWN per viewer); a small hole per actor is all the client adds
	const float OwnRadiusSq = FMath::Square(Vision->GetRadiusCm() - FeatherCm);
	for (const TPair<uint64, TWeakObjectPtr<AS1Player>>& Pair : GameInstance->Players)
	{
		const AS1Player* Other = Pair.Value.Get();
		if (Other == nullptr || Other == Me)
			continue;

		const FVector2D At(Other->GetActorLocation().X, Other->GetActorLocation().Y);
		if (FVector2D::DistSquared(At, Views[0].Pos) < OwnRadiusSq && Vision->RayDistance(Views[0].Pos, (At - Views[0].Pos).GetSafeNormal(), Vision->GetRadiusCm()) >= FVector2D::Distance(At, Views[0].Pos))
			continue;		// plainly in my own sight already

		FSourceView& Hole = Views.AddDefaulted_GetRef();
		Hole.Pos = At;
		Hole.RadiusCm = RevealHoleCm;
	}

	// ── Screen grid -> ground plane under my feet ──
	ULocalPlayer* LocalPlayer = PC->GetLocalPlayer();
	if (LocalPlayer == nullptr || LocalPlayer->ViewportClient == nullptr || LocalPlayer->ViewportClient->Viewport == nullptr)
		return;

	FSceneViewProjectionData Projection;
	if (LocalPlayer->GetProjectionData(LocalPlayer->ViewportClient->Viewport, Projection) == false)
		return;

	const FMatrix InvViewProj = Projection.ComputeViewProjectionMatrix().InverseFast();
	const FIntRect ViewRect = Projection.GetConstrainedViewRect();
	const float DpiScale = UWidgetLayoutLibrary::GetViewportScale(this);

	const UCapsuleComponent* Capsule = Me->GetCapsuleComponent();
	const double GroundZ = Feet.Z - (Capsule != nullptr ? Capsule->GetScaledCapsuleHalfHeight() : 90.f);

	const FVector2D Size = MyGeometry.GetLocalSize();
	const int32 NewCols = FMath::CeilToInt(Size.X / CellUnits) + 1;
	const int32 NewRows = FMath::CeilToInt(Size.Y / CellUnits) + 1;
	if (NewCols < 2 || NewRows < 2)
		return;

	if (NewCols != Cols || NewRows != Rows)
	{
		Cols = NewCols;
		Rows = NewRows;
		Indices.Reset((Cols - 1) * (Rows - 1) * 6);
		for (int32 R = 0; R < Rows - 1; R++)
		{
			for (int32 C = 0; C < Cols - 1; C++)
			{
				const SlateIndex A = static_cast<SlateIndex>(R * Cols + C);
				const SlateIndex B = A + 1;
				const SlateIndex D = static_cast<SlateIndex>(A + Cols);
				const SlateIndex E = D + 1;
				Indices.Append({ A, B, E, A, E, D });
			}
		}
	}

	GridLocal.SetNumUninitialized(Cols * Rows);
	GridColour.SetNumUninitialized(Cols * Rows);

	const float FogAlpha = Vision->GetFogAlpha();
	for (int32 R = 0; R < Rows; R++)
	{
		for (int32 C = 0; C < Cols; C++)
		{
			const int32 I = R * Cols + C;
			const FVector2f Local(FMath::Min(C * CellUnits, static_cast<float>(Size.X)), FMath::Min(R * CellUnits, static_cast<float>(Size.Y)));
			GridLocal[I] = Local;

			FVector Origin, Direction;
			FSceneView::DeprojectScreenToWorld(FVector2D(Local.X * DpiScale, Local.Y * DpiScale), ViewRect, InvViewProj, Origin, Direction);

			float Fog = 1.f;
			if (Direction.Z < -1e-4)		// above the horizon stays fogged
			{
				const double T = (GroundZ - Origin.Z) / Direction.Z;
				const FVector Ground = Origin + Direction * T;
				Fog = FogAt(FVector2D(Ground.X, Ground.Y));
			}

			GridColour[I] = FColor(0, 0, 0, static_cast<uint8>(FMath::Clamp(Fog * FogAlpha, 0.f, 1.f) * 255.f));
		}
	}

	bHasFog = true;
}

int32 US1FogOfWarWidget::NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const int32 Layer = Super::NativePaint(Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId, InWidgetStyle, bParentEnabled);
	if (bHasFog == false || GridLocal.Num() != Cols * Rows || Indices.Num() == 0 || FSlateApplication::IsInitialized() == false)
		return Layer;

	const FSlateBrush* Brush = FCoreStyle::Get().GetBrush(TEXT("GenericWhiteBox"));
	const FSlateResourceHandle Handle = FSlateApplication::Get().GetRenderer()->GetResourceHandle(*Brush);
	const FSlateRenderTransform& Transform = AllottedGeometry.GetAccumulatedRenderTransform();

	TArray<FSlateVertex> Vertices;
	Vertices.SetNumUninitialized(GridLocal.Num());
	for (int32 i = 0; i < GridLocal.Num(); i++)
		Vertices[i] = FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform, GridLocal[i], FVector2f(0.5f, 0.5f), GridColour[i]);

	FSlateDrawElement::MakeCustomVerts(OutDrawElements, Layer + 1, Handle, Vertices, Indices, nullptr, 0, 0);
	return Layer + 1;
}
