// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/Level/S1LevelGrayboxCommandlet.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Components/CapsuleComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Game/Level/S1LevelBuilder.h"
#include "Game/Level/S1LevelMarkers.h"
#include "Game/Level/S1NetworkPump.h"
#include "Game/S1Player.h"
#include "GameFramework/PlayerStart.h"
#include "Misc/ConfigCacheIni.h"
#include "S1GameInstance.h"
#include "Game/Level/S1LevelLayout.h"
#include "Math/RandomStream.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Physics/Experimental/PhysScene_Chaos.h"
#include "UObject/Package.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#if WITH_EDITOR
#include "StaticMeshCompiler.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/LevelScriptBlueprint.h"
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

		// Where to sample: the houses' bounding box (dense walls) and the floor's — read from the level, so the island and
		// the old square both work (2026-10-11)
		FS1LevelLayout Layout;
		FString LayoutError;
		if (!Builder->CollectLayout(Layout, LayoutError))
		{
			UE_LOG(LogS1LevelGraybox, Error, TEXT("[LEVEL] parity: %s"), *LayoutError);
			return false;
		}
		FBox2D HouseBox(ForceInit);
		for (const FS1LevelHouse& House : Layout.Houses)
		{
			HouseBox += FVector2D(House.X0, House.Y0) * 100.0;
			HouseBox += FVector2D(House.X1, House.Y1) * 100.0;
		}
		HouseBox = HouseBox.ExpandBy(400.0);
		double FX0, FX1, FY0, FY1;
		Layout.FloorBounds(FX0, FX1, FY0, FY1);

		FRandomStream Rng(Seed);
		auto Range = [&Rng](double Lo, double Hi) { return Lo + (Hi - Lo) * Rng.GetFraction(); };

		TArray<FString> Rows;
		Rows.Add(TEXT("ax,ay,bx,by,ue_start_blocked,ue_hit,ue_stop_cm"));
		int32 StartBlocked = 0;
		int32 Hits = 0;

		for (int32 i = 0; i < Samples; i++)
		{
			// 60 % among the houses (dense walls), 25 % anywhere on the floor — cover, towers, open field — and 15 % within
			// 6 m of the coast, so its walls are compared too
			const double Pick = Rng.GetFraction();
			FVector2D Start;
			if (Pick < 0.6)
			{
				Start = FVector2D(Range(HouseBox.Min.X, HouseBox.Max.X), Range(HouseBox.Min.Y, HouseBox.Max.Y));
			}
			else if (Pick < 0.85 || !Layout.HasOutline())
			{
				// The square: up to its boundary walls (1 m in). The island: its bounding box 10 m in — the corners are sea with
				// no walls, and a segment (≤ 8 m) must not reach the server's outer safety square, which has no wall in UE
				const double In = Layout.HasOutline() ? 10.0 : 1.0;
				Start = FVector2D(Range((FX0 + In) * 100.0, (FX1 - In) * 100.0), Range((FY0 + In) * 100.0, (FY1 - In) * 100.0));
			}
			else
			{
				const int32 Edge = Rng.RandRange(0, Layout.Outline.Num() - 1);
				const FVector2D EA = Layout.Outline[Edge];
				const FVector2D EB = Layout.Outline[(Edge + 1) % Layout.Outline.Num()];
				const FVector2D OnCoast = EA + (EB - EA) * Rng.GetFraction();
				FVector2D Inward = FVector2D(-(EB - EA).Y, (EB - EA).X).GetSafeNormal();
				if (!Layout.IsInside(OnCoast + Inward * 0.5))
				{
					Inward = -Inward;
				}
				Start = (OnCoast + Inward * Range(0.5, 6.0)) * 100.0;
			}
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

namespace S1LevelGameSetup
{
	const TCHAR* GameModePath = TEXT("/Game/Blueprints/BP_GameMode.BP_GameMode_C");

	// What DevMap carried besides geometry (read with -Inspect, 2026-10-07): the BP_GameMode override, a PlayerStart and a
	// level blueprint that connects and pumps packets — here an AS1NetworkPump. Without these the client opens Floor1
	// and never talks to the server (ADR-0006 Migration 4)
	bool Place(UWorld& World, FString& OutError)
	{
		UClass* GameMode = LoadClass<AGameModeBase>(nullptr, GameModePath);
		if (GameMode == nullptr)
		{
			OutError = FString::Printf(TEXT("cannot load the game mode %s"), GameModePath);
			return false;
		}
		World.GetWorldSettings()->DefaultGameMode = GameMode;

		// The game mode spawns the pawn here before the server places the player. Entry point 1, not the origin — the
		// origin is inside house H2
		FVector Start(0.0, 0.0, 100.0);
		for (TActorIterator<AS1EntryPointMarker> It(&World); It; ++It)
		{
			if (It->EntryId == 1)
			{
				Start = FVector(It->GetActorLocation().X, It->GetActorLocation().Y, 100.0);
				break;
			}
		}

		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		APlayerStart* PlayerStart = World.SpawnActor<APlayerStart>(Start, FRotator::ZeroRotator, Params);
		AS1NetworkPump* Pump = World.SpawnActor<AS1NetworkPump>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
		if (PlayerStart == nullptr || Pump == nullptr)
		{
			OutError = TEXT("cannot spawn the player start or the network pump");
			return false;
		}
#if WITH_EDITOR
		PlayerStart->SetActorLabel(TEXT("PlayerStart"));
		Pump->SetActorLabel(TEXT("S1NetworkPump"));
#endif
		return true;
	}

	// The saved map must still be playable: the game mode, one pump (two would connect twice), a player start
	bool Check(UWorld& World, FString& OutError)
	{
		const TSubclassOf<AGameModeBase> GameMode = World.GetWorldSettings()->DefaultGameMode;
		int32 Pumps = 0;
		int32 Starts = 0;
		for (TActorIterator<AS1NetworkPump> It(&World); It; ++It)
		{
			Pumps++;
		}
		for (TActorIterator<APlayerStart> It(&World); It; ++It)
		{
			Starts++;
		}
		if (GameMode == nullptr || GameMode->GetPathName() != GameModePath || Pumps != 1 || Starts < 1)
		{
			OutError = FString::Printf(TEXT("game setup — game mode %s · network pumps %d (need 1) · player starts %d"),
				GameMode != nullptr ? *GameMode->GetPathName() : TEXT("none"), Pumps, Starts);
			return false;
		}
		return true;
	}
}

#if WITH_EDITOR
namespace S1LevelInspect
{
	// What a map carries besides geometry — game mode override, actors, level blueprint nodes. Written to move DevMap's
	// setup into Floor1 (ADR-0006 Migration 4); any map can be inspected
	void Dump(UWorld& World)
	{
		const AWorldSettings* Settings = World.GetWorldSettings();
		const TSubclassOf<AGameModeBase> GameMode = Settings != nullptr ? Settings->DefaultGameMode : nullptr;
		UE_LOG(LogS1LevelGraybox, Display, TEXT("[INSPECT] game mode override: %s"), GameMode != nullptr ? *GameMode->GetPathName() : TEXT("none"));
		if (GameMode != nullptr)
		{
			const AGameModeBase* Defaults = GameMode->GetDefaultObject<AGameModeBase>();
			UE_LOG(LogS1LevelGraybox, Display, TEXT("[INSPECT]   pawn %s · controller %s · hud %s"),
				Defaults->DefaultPawnClass != nullptr ? *Defaults->DefaultPawnClass->GetPathName() : TEXT("none"),
				Defaults->PlayerControllerClass != nullptr ? *Defaults->PlayerControllerClass->GetPathName() : TEXT("none"),
				Defaults->HUDClass != nullptr ? *Defaults->HUDClass->GetPathName() : TEXT("none"));
		}

		int32 Actors = 0;
		for (TActorIterator<AActor> It(&World); It; ++It)
		{
			Actors++;
			UE_LOG(LogS1LevelGraybox, Display, TEXT("[INSPECT] actor %s (%s) at %s"), *It->GetActorNameOrLabel(), *It->GetClass()->GetPathName(), *It->GetActorLocation().ToString());
		}
		UE_LOG(LogS1LevelGraybox, Display, TEXT("[INSPECT] %d actors"), Actors);

		ULevelScriptBlueprint* Script = World.PersistentLevel->GetLevelScriptBlueprint(/*bDontCreate*/ true);
		int32 Nodes = 0;
		if (Script != nullptr)
		{
			for (UEdGraph* Graph : Script->UbergraphPages)
			{
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					Nodes++;
					UE_LOG(LogS1LevelGraybox, Display, TEXT("[INSPECT] level blueprint %s: %s"), *Graph->GetName(), *Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString().Replace(TEXT("\n"), TEXT(" / ")));
				}
			}
		}
		UE_LOG(LogS1LevelGraybox, Display, TEXT("[INSPECT] level blueprint: %s · %d nodes"), Script != nullptr ? TEXT("present") : TEXT("none"), Nodes);
	}
}
#endif

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

	// -Inspect: print what the map carries besides geometry (game mode, actors, level blueprint) and change nothing
	const bool bInspect = FParse::Param(*Params, TEXT("Inspect"));

	// -Verify: load the saved map from disk and run the same round trip — proves what was saved, not what was built.
	if (FParse::Param(*Params, TEXT("Verify")) || bParity || bInspect)
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

		if (bInspect)
		{
			S1LevelInspect::Dump(*Loaded);
			GEngine->DestroyWorldContext(Loaded);
			Loaded->DestroyWorld(false);
			return 0;
		}

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
		FString SetupError;
		if (!S1LevelGameSetup::Check(*Loaded, SetupError))
		{
			UE_LOG(LogS1LevelGraybox, Error, TEXT("[LEVEL] verify: %s"), *SetupError);
			VerifyResult = 1;
		}
		else
		{
			UE_LOG(LogS1LevelGraybox, Display, TEXT("[LEVEL] verify: game setup ok — %s · one network pump · player start"), S1LevelGameSetup::GameModePath);
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
		else if (!S1LevelGameSetup::Place(*World, Error))
		{
			UE_LOG(LogS1LevelGraybox, Error, TEXT("[LEVEL] game setup failed: %s"), *Error);
			Result = 1;
		}
		else
		{
			UE_LOG(LogS1LevelGraybox, Display, TEXT("[LEVEL] built — wall %d · door post %d · pillar %d · free pillar %d · floor tile %d · rubble %d · boundary %d · tower %d · cover %d · island ground %d"),
				Builder->NumInstances(ES1LevelModule::Wall), Builder->NumInstances(ES1LevelModule::DoorPost),
				Builder->NumInstances(ES1LevelModule::Pillar), Builder->NumInstances(ES1LevelModule::FreePillar),
				Builder->NumInstances(ES1LevelModule::FloorTile), Builder->NumInstances(ES1LevelModule::Rubble),
				Builder->NumInstances(ES1LevelModule::Boundary), Builder->NumInstances(ES1LevelModule::TowerBody),
				Builder->NumInstances(ES1LevelModule::Cover), Builder->NumInstances(ES1LevelModule::IslandGround));

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
