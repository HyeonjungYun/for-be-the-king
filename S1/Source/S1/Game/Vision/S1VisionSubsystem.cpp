// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/Vision/S1VisionSubsystem.h"

#include "Engine/GameInstance.h"
#include "Game/S1MyPlayer.h"
#include "HAL/PlatformTime.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Protocol.pb.h"
#include "S1GameInstance.h"

namespace
{
	TWeakObjectPtr<US1VisionSubsystem> GVision;

	// Server/Data - the same files the server reads (ItemDataDirectory in DefaultGame.ini)
	FString DataDirectory()
	{
		FString Relative;
		GConfig->GetString(TEXT("/Script/S1.S1ItemSubsystem"), TEXT("ItemDataDirectory"), Relative, GGameIni);
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), Relative));
	}

	// Comma separated, no quotes (Server/Data convention). Header row names the columns
	bool ReadCsv(const FString& Path, TArray<FString>& OutHeader, TArray<TArray<FString>>& OutRows)
	{
		FString Text;
		if (FFileHelper::LoadFileToString(Text, *Path) == false)
			return false;

		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, true);
		if (Lines.Num() == 0)
			return false;

		Lines[0].TrimStartAndEndInline();
		Lines[0].RemoveFromStart(TEXT("\xFEFF"));
		Lines[0].ParseIntoArray(OutHeader, TEXT(","), false);

		for (int32 i = 1; i < Lines.Num(); i++)
		{
			TArray<FString> Cells;
			Lines[i].TrimStartAndEndInline();
			if (Lines[i].IsEmpty())
				continue;
			Lines[i].ParseIntoArray(Cells, TEXT(","), false);
			OutRows.Add(MoveTemp(Cells));
		}
		return true;
	}

	void ApplyCamera(UGameInstance* GameInstance, float Scale)
	{
		const US1GameInstance* S1 = Cast<US1GameInstance>(GameInstance);
		if (AS1MyPlayer* Me = S1 != nullptr ? Cast<AS1MyPlayer>(S1->MyPlayer.Get()) : nullptr)
			Me->SetCameraScale(Scale);
	}
}

US1VisionSubsystem* US1VisionSubsystem::Get()
{
	return GVision.Get();
}

void US1VisionSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	GVision = this;
	LoadRules();
}

void US1VisionSubsystem::Deinitialize()
{
	Walls.Reset();
	DynamicWalls.Reset();
	Sources.Reset();
	if (GVision.Get() == this)
		GVision = nullptr;
	Super::Deinitialize();
}

void US1VisionSubsystem::LoadRules()
{
	TArray<FString> Header;
	TArray<TArray<FString>> Rows;
	if (ReadCsv(FPaths::Combine(DataDirectory(), TEXT("vision_rules.csv")), Header, Rows) == false)
		return;

	for (const TArray<FString>& Row : Rows)
	{
		if (Row.Num() >= 2 && Row[0] == TEXT("fog_alpha"))
			FogAlpha = FMath::Clamp(FCString::Atof(*Row[1]), 0.f, 1.f);
	}
}

void US1VisionSubsystem::HandleVision(const Protocol::S_VISION& Pkt)
{
	RadiusCm = Pkt.radius_cm();
	CameraScale = Pkt.camera_scale() > 0.f ? Pkt.camera_scale() : 1.f;

	const double Now = FPlatformTime::Seconds();
	Sources.Reset();
	for (const Protocol::VisionSource& S : Pkt.sources())
	{
		FS1VisionSource& Source = Sources.AddDefaulted_GetRef();
		Source.SourceId = S.source_id();
		Source.FollowId = S.follow_id();
		Source.Pos = FVector2D(S.x(), S.y());
		Source.RadiusCm = S.radius_cm();
		Source.Until = S.remaining_ms() > 0 ? Now + S.remaining_ms() / 1000.0 : 0.0;
		Source.bNeedsLos = S.needs_los();
		Source.bSeesStealth = S.sees_stealth();
		Source.Kind = UTF8_TO_TCHAR(S.kind().c_str());
	}

	ApplyCamera(GetGameInstance(), CameraScale);

	UE_LOG(LogTemp, Log, TEXT("[FOW] vision radius=%.0f camera=%.2f sources=%d"), RadiusCm, CameraScale, Sources.Num());
}

void US1VisionSubsystem::Clear()
{
	DynamicWalls.Reset();

	if (RadiusCm <= 0.f && Sources.Num() == 0 && CameraScale == 1.f)
		return;

	RadiusCm = 0.f;
	CameraScale = 1.f;
	Sources.Reset();
	ApplyCamera(GetGameInstance(), 1.f);
	UE_LOG(LogTemp, Log, TEXT("[FOW] cleared (left the dungeon)"));
}

bool US1VisionSubsystem::EnsureWalls(uint32 FloorId)
{
	if (LoadedFloor == static_cast<int32>(FloorId))
		return Walls.Num() > 0;

	LoadedFloor = static_cast<int32>(FloorId);
	Walls.Reset();

	TArray<FString> Header;
	TArray<TArray<FString>> Rows;
	const FString Path = FPaths::Combine(DataDirectory(), TEXT("walls.csv"));
	if (ReadCsv(Path, Header, Rows) == false)
	{
		UE_LOG(LogTemp, Warning, TEXT("[FOW] walls.csv not found (%s) - fog draws plain circles"), *Path);
		return false;
	}

	const int32 ColFloor = Header.IndexOfByKey(TEXT("floor_id"));
	const int32 ColKind = Header.IndexOfByKey(TEXT("kind"));
	const int32 ColCx = Header.IndexOfByKey(TEXT("cx"));
	const int32 ColCy = Header.IndexOfByKey(TEXT("cy"));
	const int32 ColHalfL = Header.IndexOfByKey(TEXT("half_length"));
	const int32 ColHalfT = Header.IndexOfByKey(TEXT("half_thickness"));
	const int32 ColYaw = Header.IndexOfByKey(TEXT("yaw"));

	if (ColFloor == INDEX_NONE || ColKind == INDEX_NONE || ColCx == INDEX_NONE || ColCy == INDEX_NONE
		|| ColHalfL == INDEX_NONE || ColHalfT == INDEX_NONE || ColYaw == INDEX_NONE)
	{
		UE_LOG(LogTemp, Warning, TEXT("[FOW] walls.csv header is missing a column"));
		return false;
	}

	for (const TArray<FString>& Row : Rows)
	{
		if (Row.Num() < Header.Num() || FCString::Atoi(*Row[ColFloor]) != static_cast<int32>(FloorId))
			continue;

		// Cover is low - the server lets sight pass over it (WallMap::SightBlocked), so must the fog
		if (Row[ColKind] == TEXT("cover"))
			continue;

		FWall Wall;
		Wall.Centre = FVector2D(FCString::Atof(*Row[ColCx]), FCString::Atof(*Row[ColCy]));
		Wall.HalfLength = FCString::Atof(*Row[ColHalfL]);
		Wall.HalfThickness = FCString::Atof(*Row[ColHalfT]);
		const float YawRad = FMath::DegreesToRadians(FCString::Atof(*Row[ColYaw]));
		Wall.Axis = FVector2D(FMath::Cos(YawRad), FMath::Sin(YawRad));
		Wall.BoundRadius = FMath::Sqrt(Wall.HalfLength * Wall.HalfLength + Wall.HalfThickness * Wall.HalfThickness);
		Walls.Add(Wall);
	}

	UE_LOG(LogTemp, Log, TEXT("[FOW] walls loaded floor=%u sight-blocking=%d"), FloorId, Walls.Num());
	return Walls.Num() > 0;
}

void US1VisionSubsystem::AddDynamicWall(uint64 Id, const FVector2D& Centre, float YawDeg, float HalfLength, float HalfThickness)
{
	FWall Wall;
	Wall.Centre = Centre;
	Wall.HalfLength = HalfLength;
	Wall.HalfThickness = HalfThickness;
	const float YawRad = FMath::DegreesToRadians(YawDeg);
	Wall.Axis = FVector2D(FMath::Cos(YawRad), FMath::Sin(YawRad));
	Wall.BoundRadius = FMath::Sqrt(HalfLength * HalfLength + HalfThickness * HalfThickness);
	DynamicWalls.Add(Id, Wall);

	UE_LOG(LogTemp, Log, TEXT("[FOW] dynamic wall +%llu at=(%.0f,%.0f) len=%.0f"), Id, Centre.X, Centre.Y, HalfLength * 2.f);
}

void US1VisionSubsystem::RemoveDynamicWall(uint64 Id)
{
	if (DynamicWalls.Remove(Id) > 0)
		UE_LOG(LogTemp, Log, TEXT("[FOW] dynamic wall -%llu"), Id);
}

float US1VisionSubsystem::RayDistance(const FVector2D& Origin, const FVector2D& Dir, float MaxCm) const
{
	float Best = RayDistanceIn(Walls, Origin, Dir, MaxCm);
	if (DynamicWalls.Num() > 0)
	{
		TArray<FWall, TInlineAllocator<8>> Extra;
		for (const TPair<uint64, FWall>& Pair : DynamicWalls)
			Extra.Add(Pair.Value);
		Best = RayDistanceIn(Extra, Origin, Dir, Best);
	}
	return Best;
}

float US1VisionSubsystem::RayDistanceIn(TArrayView<const FWall> Set, const FVector2D& Origin, const FVector2D& Dir, float MaxCm)
{
	float Best = MaxCm;

	for (const FWall& Wall : Set)
	{
		const FVector2D Rel = Origin - Wall.Centre;
		if (Rel.Size() - Wall.BoundRadius > Best)
			continue;

		// Into the box's frame: X along its length, Y across its thickness
		const FVector2D Perp(-Wall.Axis.Y, Wall.Axis.X);
		const double Px = FVector2D::DotProduct(Rel, Wall.Axis);
		const double Py = FVector2D::DotProduct(Rel, Perp);
		const double Dx = FVector2D::DotProduct(Dir, Wall.Axis);
		const double Dy = FVector2D::DotProduct(Dir, Perp);

		double Lo = 0.0;
		double Hi = Best;
		bool bMiss = false;

		auto Clip = [&](double Pos, double D, double Half)
			{
				if (FMath::Abs(D) < 1e-9)
				{
					if (FMath::Abs(Pos) >= Half)
						bMiss = true;		// parallel and outside (touching a face does not block - same as the server)
					return;
				}
				double T0 = (-Half - Pos) / D;
				double T1 = (Half - Pos) / D;
				if (T0 > T1)
					Swap(T0, T1);
				Lo = FMath::Max(Lo, T0);
				Hi = FMath::Min(Hi, T1);
			};

		Clip(Px, Dx, Wall.HalfLength);
		Clip(Py, Dy, Wall.HalfThickness);

		if (bMiss == false && Lo < Hi && Lo < Best)
			Best = static_cast<float>(Lo);
	}

	return Best;
}

void US1VisionSubsystem::CastRays(const FVector2D& Origin, float MaxCm, int32 NumRays, TArray<float>& OutDistances) const
{
	OutDistances.SetNumUninitialized(NumRays);

	// Only the walls that can be reached at all this frame
	TArray<FWall, TInlineAllocator<128>> Near;
	for (const FWall& Wall : Walls)
	{
		if (FVector2D::Distance(Origin, Wall.Centre) - Wall.BoundRadius <= MaxCm)
			Near.Add(Wall);
	}
	for (const TPair<uint64, FWall>& Pair : DynamicWalls)
	{
		if (FVector2D::Distance(Origin, Pair.Value.Centre) - Pair.Value.BoundRadius <= MaxCm)
			Near.Add(Pair.Value);
	}

	CastRaysOver(Near, Origin, MaxCm, NumRays, OutDistances);
}

void US1VisionSubsystem::CastRaysReference(TArrayView<const FWall> Near, const FVector2D& Origin, float MaxCm, int32 NumRays, TArray<float>& OutDistances)
{
	OutDistances.SetNumUninitialized(NumRays);
	for (int32 i = 0; i < NumRays; i++)
	{
		const float Angle = 2.f * PI * static_cast<float>(i) / static_cast<float>(NumRays);
		OutDistances[i] = RayDistanceIn(Near, Origin, FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)), MaxCm);
	}
}

void US1VisionSubsystem::CastRaysOver(TArrayView<const FWall> Near, const FVector2D& Origin, float MaxCm, int32 NumRays, TArray<float>& OutDistances)
{
	OutDistances.SetNumUninitialized(NumRays);

	// Per wall, once: the parts of RayDistanceIn that do not depend on the ray direction (TD-009, 2026-10-18).
	// 240 rays + 120 per extra source x every near wall used to redo these for every ray. Same expressions -> same bits
	struct FPrepared
	{
		double RelSize;
		float BoundRadius;
		FVector2D Axis;
		FVector2D Perp;
		double Px;
		double Py;
		float HalfLength;
		float HalfThickness;
	};
	TArray<FPrepared, TInlineAllocator<128>> Prepared;
	Prepared.Reserve(Near.Num());
	for (const FWall& Wall : Near)
	{
		const FVector2D Rel = Origin - Wall.Centre;
		const FVector2D Perp(-Wall.Axis.Y, Wall.Axis.X);
		Prepared.Add({ Rel.Size(), Wall.BoundRadius, Wall.Axis, Perp,
			FVector2D::DotProduct(Rel, Wall.Axis), FVector2D::DotProduct(Rel, Perp), Wall.HalfLength, Wall.HalfThickness });
	}

	for (int32 i = 0; i < NumRays; i++)
	{
		const float Angle = 2.f * PI * static_cast<float>(i) / static_cast<float>(NumRays);
		const FVector2D Dir(FMath::Cos(Angle), FMath::Sin(Angle));
		float Best = MaxCm;

		for (const FPrepared& W : Prepared)
		{
			if (W.RelSize - W.BoundRadius > Best)
				continue;

			const double Dx = FVector2D::DotProduct(Dir, W.Axis);
			const double Dy = FVector2D::DotProduct(Dir, W.Perp);

			double Lo = 0.0;
			double Hi = Best;
			bool bMiss = false;

			auto Clip = [&](double Pos, double D, double Half)
				{
					if (FMath::Abs(D) < 1e-9)
					{
						if (FMath::Abs(Pos) >= Half)
							bMiss = true;		// parallel and outside (touching a face does not block - same as the server)
						return;
					}
					double T0 = (-Half - Pos) / D;
					double T1 = (Half - Pos) / D;
					if (T0 > T1)
						Swap(T0, T1);
					Lo = FMath::Max(Lo, T0);
					Hi = FMath::Min(Hi, T1);
				};

			Clip(W.Px, Dx, W.HalfLength);
			Clip(W.Py, Dy, W.HalfThickness);

			if (bMiss == false && Lo < Hi && Lo < Best)
				Best = static_cast<float>(Lo);
		}

		OutDistances[i] = Best;
	}
}
