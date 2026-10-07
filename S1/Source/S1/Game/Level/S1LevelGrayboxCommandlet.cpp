// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/Level/S1LevelGrayboxCommandlet.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Game/Level/S1LevelBuilder.h"
#include "Game/Level/S1LevelLayout.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogS1LevelGraybox, Log, All);

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

	// -Verify: load the saved map from disk and run the same round trip — proves what was saved, not what was built.
	if (FParse::Param(*Params, TEXT("Verify")))
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
			UE_LOG(LogS1LevelGraybox, Display, TEXT("[LEVEL] verify %s — differences %d · rules %d checks, %d failed · wall %d · cover %d"),
				*MapName, Diffs.Num(), Report.Checks.Num(), Report.NumFailed(),
				It->NumInstances(ES1LevelModule::Wall), It->NumInstances(ES1LevelModule::Cover));
			VerifyResult = (Diffs.Num() == 0 && Report.NumFailed() == 0) ? 0 : 1;
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
		Args.SaveFlags = SAVE_NoError;
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
