// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/Level/S1LevelGrayboxCommandlet.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Components/CapsuleComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Game/Level/S1LevelBuilder.h"
#include "Game/S1Player.h"
#include "Misc/ConfigCacheIni.h"
#include "S1GameInstance.h"
#include "Game/Level/S1LevelLayout.h"
#include "Math/RandomStream.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Physics/Experimental/PhysScene_Chaos.h"
#include "UObject/Package.h"
#if WITH_EDITOR
#include "StaticMeshCompiler.h"
#endif
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogS1LevelGraybox, Log, All);

namespace S1LevelParity
{
	// AC-LM-21 · docs/architecture/adr-0006-server-2d-walls.md § Validation — the character capsule sweeps seeded segments
	// over the saved level and writes where it stops. The server test (WallMapTest.cpp) runs the same segments through
	// WallMap (r = 42) and compares. The server code is not copied into UE — each side answers with its own geometry.
	constexpr float		ServerRadius = 42.f;		// ADR-0006 § ⑦ — knockback · dash · blink radius on the server
	constexpr double	SweepLift = 25.0;			// capsule bottom 0.25 m (over 0.2 m rubble); top stays under 2.5 m cover
	constexpr int32		Samples = 2000;
	constexpr int32		Seed = 20261007;

	double Round2(double V)
	{
		return FMath::RoundToDouble(V * 100.0) / 100.0;
	}

	// The capsule of the character players actually get (review C7): the game instance's player class — a Blueprint —
	// else the C++ AS1Player. Size and collision profile both come from it, not from constants here
	const UCapsuleComponent* PlayerCapsule()
	{
		const AS1Player* Player = GetDefault<AS1Player>();
		FString GameInstancePath;
		if (GConfig->GetString(TEXT("/Script/EngineSettings.GameMapsSettings"), TEXT("GameInstanceClass"), GameInstancePath, GEngineIni))
		{
			if (UClass* Class = LoadClass<US1GameInstance>(nullptr, *GameInstancePath))
			{
				const US1GameInstance* Instance = Class->GetDefaultObject<US1GameInstance>();
				if (Instance->OtherPlayerClass != nullptr)
				{
					Player = Instance->OtherPlayerClass->GetDefaultObject<AS1Player>();
				}
			}
		}
		return Player->GetCapsuleComponent();
	}

	bool Record(UWorld& World, const FString& OutFile)
	{
		// A commandlet world never ticks, and two things only finish on a tick (found by the canary, 2026-10-07):
		//  1. a mesh still compiling when the map loaded skips its physics state — "recreated after compilation finishes"
		//     (StaticMeshComponent.cpp), and nothing finishes it here. The level had 0 instance bodies
		//  2. bodies added to the scene are queued until the scene runs — "required when querying against a currently
		//     non-running scene" (ChaosScene.h Flush)
#if WITH_EDITOR
		FStaticMeshCompilingManager::Get().FinishAllCompilation();
#endif
		int32 Recreated = 0;
		for (TActorIterator<AActor> It(&World); It; ++It)
		{
			TInlineComponentArray<UPrimitiveComponent*> Primitives(*It);
			for (UPrimitiveComponent* Primitive : Primitives)
			{
				if (Primitive->IsRegistered() && Primitive->IsCollisionEnabled() && !Primitive->IsPhysicsStateCreated())
				{
					Primitive->RecreatePhysicsState();
					Recreated++;
				}
			}
		}
		if (FPhysScene* Scene = World.GetPhysicsScene())
		{
			Scene->Flush();
		}

		AS1LevelBuilder* Builder = nullptr;
		for (TActorIterator<AS1LevelBuilder> It(&World); It; ++It)
		{
			Builder = *It;
		}
		if (Builder == nullptr)
		{
			UE_LOG(LogS1LevelGraybox, Error, TEXT("[LEVEL] parity: the map has no S1LevelBuilder"));
			return false;
		}

		// Every colliding module has one body per instance (review A6) — a missing body is a wall the sweeps cannot see.
		// 5.8: read GetInstanceBodies(); the InstanceBodies member is deprecated and stays empty
		int32 Bodies = 0;
		TInlineComponentArray<UInstancedStaticMeshComponent*> Modules(Builder);
		for (const UInstancedStaticMeshComponent* Module : Modules)
		{
			if (!Module->IsCollisionEnabled())
			{
				continue;
			}
			Bodies += Module->GetInstanceBodies().Num();
			if (Module->GetInstanceBodies().Num() != Module->GetInstanceCount())
			{
				UE_LOG(LogS1LevelGraybox, Error, TEXT("[LEVEL] parity: %s has %d bodies for %d instances — nothing written"),
					*Module->GetName(), Module->GetInstanceBodies().Num(), Module->GetInstanceCount());
				return false;
			}
		}
		UE_LOG(LogS1LevelGraybox, Display, TEXT("[LEVEL] parity — physics state created for %d component(s) · %d instance bodies, one per instance"),
			Recreated, Bodies);

		// The real character capsule (C7). The server judges with r = 42 — if the character changes, the ADR and the server must too
		const UCapsuleComponent* CharacterCapsule = PlayerCapsule();
		const float Radius = CharacterCapsule->GetUnscaledCapsuleRadius();
		const float HalfHeight = CharacterCapsule->GetUnscaledCapsuleHalfHeight();
		const FName Profile = CharacterCapsule->GetCollisionProfileName();
		UE_LOG(LogS1LevelGraybox, Display, TEXT("[LEVEL] parity — character capsule %.1f x %.1f cm, profile %s"), Radius, HalfHeight, *Profile.ToString());
		if (!FMath::IsNearlyEqual(Radius, ServerRadius, 0.01f))
		{
			UE_LOG(LogS1LevelGraybox, Error, TEXT("[LEVEL] parity: the character capsule radius is %.1f cm but the server judges with %.1f — "
				"update ADR-0006 § ⑦ and the server first; nothing written"), Radius, ServerRadius);
			return false;
		}

		const double SweepZ = HalfHeight + SweepLift;
		const FCollisionShape Capsule = FCollisionShape::MakeCapsule(Radius, HalfHeight);
		const FCollisionQueryParams Params(SCENE_QUERY_STAT(S1LevelParity), /*bTraceComplex*/ false);

		auto Sweep = [&](const FVector2D& A, const FVector2D& B, FHitResult& Hit)
		{
			return World.SweepSingleByProfile(Hit, FVector(A, SweepZ), FVector(B, SweepZ), FQuat::Identity, Profile, Capsule, Params);
		};

		// Canaries, one per server-wall kind (review A6): from 1 m outside the first box of each kind straight at it — some side
		// must stop the capsule at 100 cm. If physics did not see the level every sweep would "miss" and the comparison would
		// pass for nothing (that is what the first spike did)
		{
			TArray<FString> WallRows;
			FString Error;
			if (!Builder->CollectServerWalls(WallRows, Error))
			{
				UE_LOG(LogS1LevelGraybox, Error, TEXT("[LEVEL] parity: server walls: %s"), *Error);
				return false;
			}

			TSet<FString> Checked;
			for (const FString& Row : WallRows)
			{
				TArray<FString> Cells;
				Row.ParseIntoArray(Cells, TEXT(","));		// kind,cx,cy,half_length,half_thickness,yaw
				if (Cells.Num() != 6 || Checked.Contains(Cells[0]))
				{
					continue;
				}

				const FVector2D Centre(FCString::Atod(*Cells[1]), FCString::Atod(*Cells[2]));
				const double Half[2] = { FCString::Atod(*Cells[3]), FCString::Atod(*Cells[4]) };
				const double YawRad = FMath::DegreesToRadians(FCString::Atod(*Cells[5]));
				const FVector2D Axis[2] = { FVector2D(FMath::Cos(YawRad), FMath::Sin(YawRad)), FVector2D(-FMath::Sin(YawRad), FMath::Cos(YawRad)) };

				double Best = -1.0;
				for (int32 a = 0; a < 2 && Best < 0.0; a++)
				{
					for (const double Side : { -1.0, 1.0 })
					{
						const FVector2D Start = Centre + Axis[a] * Side * (Half[a] + Radius + 100.0);
						FHitResult Hit;
						if (!World.OverlapBlockingTestByProfile(FVector(Start, SweepZ), FQuat::Identity, Profile, Capsule, Params)
							&& Sweep(Start, Centre, Hit) && FMath::Abs(Hit.Time * FVector2D::Distance(Start, Centre) - 100.0) <= 5.0)
						{
							Best = Hit.Time * FVector2D::Distance(Start, Centre);
							break;
						}
					}
				}

				UE_LOG(LogS1LevelGraybox, Display, TEXT("[LEVEL] parity canary %s — %s"), *Cells[0],
					Best >= 0.0 ? *FString::Printf(TEXT("stopped at %.2f cm (expected 100)"), Best) : TEXT("MISSED"));
				if (Best < 0.0)
				{
					UE_LOG(LogS1LevelGraybox, Error, TEXT("[LEVEL] parity canary failed — physics queries do not see the %s module; nothing written"), *Cells[0]);
					return false;
				}
				Checked.Add(Cells[0]);
			}
		}

		FRandomStream Rng(Seed);
		auto Range = [&Rng](double Lo, double Hi) { return Lo + (Hi - Lo) * Rng.GetFraction(); };

		TArray<FString> Rows;
		Rows.Add(TEXT("ax,ay,bx,by,ue_start_blocked,ue_hit,ue_stop_cm"));
		int32 StartBlocked = 0;
		int32 Hits = 0;

		for (int32 i = 0; i < Samples; i++)
		{
			// 70 % in the band of houses (dense walls), the rest anywhere — cover, towers, open field
			const bool bHouses = Rng.GetFraction() < 0.7f;
			const FVector2D Start = bHouses ? FVector2D(Range(-4000.0, 2400.0), Range(-11600.0, 11600.0)) : FVector2D(Range(-13900.0, 13900.0), Range(-13900.0, 13900.0));
			const double Angle = Range(0.0, 2.0 * UE_DOUBLE_PI);
			const double Reach = Range(50.0, 800.0);

			// Written with two decimals — sweep exactly what is written, so the server tests the same segment
			const FVector2D A(Round2(Start.X), Round2(Start.Y));
			const FVector2D B(Round2(Start.X + Reach * FMath::Cos(Angle)), Round2(Start.Y + Reach * FMath::Sin(Angle)));
			const double Length = FVector2D::Distance(A, B);

			if (World.OverlapBlockingTestByProfile(FVector(A, SweepZ), FQuat::Identity, Profile, Capsule, Params))
			{
				StartBlocked++;
				Rows.Add(FString::Printf(TEXT("%.2f,%.2f,%.2f,%.2f,1,0,0.00"), A.X, A.Y, B.X, B.Y));
				continue;
			}

			FHitResult Hit;
			const bool bHit = Sweep(A, B, Hit) && !Hit.bStartPenetrating;
			Hits += bHit ? 1 : 0;
			Rows.Add(FString::Printf(TEXT("%.2f,%.2f,%.2f,%.2f,0,%d,%.2f"), A.X, A.Y, B.X, B.Y, bHit ? 1 : 0, bHit ? Hit.Time * Length : Length));
		}

		const FString Path = FPaths::IsRelative(OutFile) ? FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), OutFile)) : OutFile;
		if (!FFileHelper::SaveStringToFile(FString::Join(Rows, TEXT("\r\n")) + TEXT("\r\n"), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogS1LevelGraybox, Error, TEXT("[LEVEL] parity: cannot write %s"), *Path);
			return false;
		}

		UE_LOG(LogS1LevelGraybox, Display, TEXT("[LEVEL] parity — %d segments · start blocked %d · hit %d · written to %s"), Samples, StartBlocked, Hits, *Path);
		return true;
	}
}

US1LevelGrayboxCommandlet::US1LevelGrayboxCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

int32 US1LevelGrayboxCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	FString MapName = TEXT("/Game/Maps/Floor1");
	FParse::Value(*Params, TEXT("Map="), MapName);
	FString LayoutFile;
	FParse::Value(*Params, TEXT("Layout="), LayoutFile);
	const bool bExport = FParse::Param(*Params, TEXT("Export"));
	const bool bSave = !FParse::Param(*Params, TEXT("NoSave"));

	// -Parity=<file>: sweep the character capsule over the saved map and write the stops for the server test (AC-LM-21)
	FString ParityFile;
	const bool bParity = FParse::Value(*Params, TEXT("Parity="), ParityFile);

	// -Verify: load the saved map from disk and run the same round trip — proves what was saved, not what was built.
	if (FParse::Param(*Params, TEXT("Verify")) || bParity)
	{
		UPackage* Saved = LoadPackage(nullptr, *MapName, LOAD_None);
		UWorld* Loaded = Saved != nullptr ? UWorld::FindWorldInPackage(Saved) : nullptr;
		if (Loaded == nullptr)
		{
			UE_LOG(LogS1LevelGraybox, Error, TEXT("[LEVEL] verify: cannot load %s"), *MapName);
			return 1;
		}
		Loaded->WorldType = EWorldType::Editor;
		FWorldContext& LoadedContext = GEngine->CreateNewWorldContext(EWorldType::Editor);
		LoadedContext.SetCurrentWorld(Loaded);
		Loaded->InitWorld(UWorld::InitializationValues().AllowAudioPlayback(false).CreateNavigation(false).CreateAISystem(false));
		// Register components so actor locations are computed — without this every marker reads as the origin.
		// Construction scripts are NOT re-run: we check what was saved, not what a rebuild would make.
		Loaded->UpdateWorldComponents(/*bRerunConstructionScripts*/ false, /*bCurrentLevelOnly*/ false);

		if (bParity)
		{
			const int32 ParityResult = S1LevelParity::Record(*Loaded, ParityFile) ? 0 : 1;
			GEngine->DestroyWorldContext(Loaded);
			Loaded->DestroyWorld(false);
			return ParityResult;
		}

		int32 VerifyResult = 1;
		int32 Builders = 0;
		for (TActorIterator<AS1LevelBuilder> It(Loaded); It; ++It)
		{
			Builders++;
			if (!LayoutFile.IsEmpty())
			{
				It->LayoutFile = LayoutFile;
			}
			FString Error;
			FS1LevelLayout Expected, Collected;
			FS1LevelReport Report;
			if (!Expected.LoadFromFile(It->LayoutPath(), Error) || !It->CollectLayout(Collected, Error))
			{
				UE_LOG(LogS1LevelGraybox, Error, TEXT("[LEVEL] verify: %s"), *Error);
				continue;
			}
			const TArray<FString> Diffs = S1LevelRules::CompareLayouts(Expected, Collected);
			for (const FString& Diff : Diffs)
			{
				UE_LOG(LogS1LevelGraybox, Error, TEXT("[LEVEL] verify: %s"), *Diff);
			}
			It->Validate(Collected, Report);

			// Server walls of the saved map — the same rows Export would write (ADR-0006 E1–E9)
			TArray<FString> WallRows, Blockers;
			FString WallError;
			const bool bWallsOk = It->CollectServerWalls(WallRows, WallError);
			It->FindClientOnlyBlockers(Blockers);
			if (!bWallsOk)
			{
				UE_LOG(LogS1LevelGraybox, Error, TEXT("[LEVEL] verify: server walls: %s"), *WallError);
			}
			for (const FString& Blocker : Blockers)
			{
				UE_LOG(LogS1LevelGraybox, Error, TEXT("[LEVEL] verify: blocks characters but is not a server wall (E7): %s"), *Blocker);
			}

			UE_LOG(LogS1LevelGraybox, Display, TEXT("[LEVEL] verify %s — differences %d · rules %d checks, %d failed · wall %d · cover %d · server walls %d"),
				*MapName, Diffs.Num(), Report.Checks.Num(), Report.NumFailed(),
				It->NumInstances(ES1LevelModule::Wall), It->NumInstances(ES1LevelModule::Cover), WallRows.Num());
			VerifyResult = (Diffs.Num() == 0 && Report.NumFailed() == 0 && bWallsOk && Blockers.Num() == 0) ? 0 : 1;
		}
		if (Builders != 1)
		{
			UE_LOG(LogS1LevelGraybox, Error, TEXT("[LEVEL] verify: expected one S1LevelBuilder, found %d"), Builders);
			VerifyResult = 1;
		}
		GEngine->DestroyWorldContext(Loaded);
		Loaded->DestroyWorld(false);
		return VerifyResult;
	}

	UPackage* Package = CreatePackage(*MapName);

	// The map is rebuilt from nothing. Without this, an existing Floor1.umap on disk makes the new package count as
	// "only partially loaded" and SavePackage refuses to overwrite it (2026-10-07)
	Package->MarkAsFullyLoaded();
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, FName(*FPackageName::GetShortName(MapName)), Package);
	if (World == nullptr)
	{
		UE_LOG(LogS1LevelGraybox, Error, TEXT("[LEVEL] cannot create a world for %s"), *MapName);
		return 1;
	}
	World->SetFlags(RF_Public | RF_Standalone);
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
	Context.SetCurrentWorld(World);

	int32 Result = 0;
	AS1LevelBuilder* Builder = World->SpawnActor<AS1LevelBuilder>();
	if (Builder == nullptr)
	{
		UE_LOG(LogS1LevelGraybox, Error, TEXT("[LEVEL] cannot spawn the builder"));
		Result = 1;
	}
	else
	{
		Builder->SetActorLabel(TEXT("S1LevelBuilder"));
		if (!LayoutFile.IsEmpty())
		{
			Builder->LayoutFile = LayoutFile;
		}

		FString Error;
		if (!Builder->Build(Error))
		{
			UE_LOG(LogS1LevelGraybox, Error, TEXT("[LEVEL] build failed: %s"), *Error);
			Result = 1;
		}
		else
		{
			UE_LOG(LogS1LevelGraybox, Display, TEXT("[LEVEL] built — wall %d · door post %d · pillar %d · free pillar %d · floor tile %d · rubble %d · boundary %d · tower %d · cover %d"),
				Builder->NumInstances(ES1LevelModule::Wall), Builder->NumInstances(ES1LevelModule::DoorPost),
				Builder->NumInstances(ES1LevelModule::Pillar), Builder->NumInstances(ES1LevelModule::FreePillar),
				Builder->NumInstances(ES1LevelModule::FloorTile), Builder->NumInstances(ES1LevelModule::Rubble),
				Builder->NumInstances(ES1LevelModule::Boundary), Builder->NumInstances(ES1LevelModule::TowerBody),
				Builder->NumInstances(ES1LevelModule::Cover));

			// Round trip: what the level now holds must be the layout file, and pass every rule.
			FS1LevelLayout Expected, Collected;
			FS1LevelReport Report;
			if (!Expected.LoadFromFile(Builder->LayoutPath(), Error) || !Builder->CollectLayout(Collected, Error))
			{
				UE_LOG(LogS1LevelGraybox, Error, TEXT("[LEVEL] round trip: %s"), *Error);
				Result = 1;
			}
			else
			{
				const TArray<FString> Diffs = S1LevelRules::CompareLayouts(Expected, Collected);
				for (const FString& Diff : Diffs)
				{
					UE_LOG(LogS1LevelGraybox, Error, TEXT("[LEVEL] round trip: %s"), *Diff);
				}
				Builder->Validate(Collected, Report);
				UE_LOG(LogS1LevelGraybox, Display, TEXT("[LEVEL] round trip differences %d · rules %d checks, %d failed"),
					Diffs.Num(), Report.Checks.Num(), Report.NumFailed());
				if (Report.NumFailed() > 0)
				{
					UE_LOG(LogS1LevelGraybox, Error, TEXT("%s"), *Report.ToString(/*bFailuresOnly*/ true));
				}
				Result = (Diffs.Num() == 0 && Report.NumFailed() == 0) ? 0 : 1;
			}

			if (Result == 0 && bExport)
			{
				FS1LevelReport ExportReport;
				FString Directory;
				if (Builder->Export(ExportReport, Directory, Error))
				{
					UE_LOG(LogS1LevelGraybox, Display, TEXT("[LEVEL] exported to %s — data version %s"), *Directory, *Builder->LastDataVersion);
				}
				else
				{
					UE_LOG(LogS1LevelGraybox, Error, TEXT("[LEVEL] export refused: %s"), *Error);
					Result = 1;
				}
			}
		}
	}

	if (Result == 0 && bSave)
	{
		const FString Filename = FPackageName::LongPackageNameToFilename(MapName, FPackageName::GetMapPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_None;		// not SAVE_NoError — that hides why a save failed
		Args.Error = GWarn;				// the default GError would make a save error fatal
		if (UPackage::SavePackage(Package, World, *Filename, Args))
		{
			UE_LOG(LogS1LevelGraybox, Display, TEXT("[LEVEL] saved %s"), *Filename);
		}
		else
		{
			UE_LOG(LogS1LevelGraybox, Error, TEXT("[LEVEL] cannot save %s"), *Filename);
			Result = 1;
		}
	}

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(/*bInformEngineOfWorld*/ false);
	return Result;
#else
	return 1;
#endif
}
