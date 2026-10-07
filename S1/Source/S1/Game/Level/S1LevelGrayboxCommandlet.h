// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "S1LevelGrayboxCommandlet.generated.h"

/**
 * Builds the floor graybox headless, checks the round trip and saves the map (design/levels/floor-1.md § 9).
 *
 *   UnrealEditor-Cmd.exe S1.uproject -run=S1LevelGraybox -Map=/Game/Maps/Floor1 [-Layout=../tools/level/floor1.json]
 *                        [-Export] [-NoSave] -unattended -nullrhi
 *   … -run=S1LevelGraybox -Map=/Game/Maps/Floor1 -Verify     (load the saved map, round trip + rules only)
 *
 * Note: UnrealEditor-Cmd exits 1 while the project prints the GameFeatureData asset-manager errors at startup,
 * even when this commandlet returns 0 — read "[LEVEL]" lines and "finished execution (result N)" in the log.
 *
 * Steps: new empty world → AS1LevelBuilder::Build → collect the markers back → every rule → compare with the
 * layout file (positions within 1 cm) → optional export → save. Exit code 0 only if every step passes.
 */
UCLASS()
class S1_API US1LevelGrayboxCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	US1LevelGrayboxCommandlet();

	virtual int32 Main(const FString& Params) override;
};
