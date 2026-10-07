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
 *   … -run=S1LevelGraybox -Map=/Game/Maps/Floor1 -Verify     (load the saved map, round trip + rules + server walls)
 *   … -run=S1LevelGraybox -Map=/Game/Maps/Floor1 -Parity=../Server/GameServerTests/Data/floor1_parity.csv
 *                        (capsule sweeps over the saved map for the server's collision parity test, AC-LM-21)
 *
 * The process exit code is this commandlet's result (0 = every step passed). Before 2026-10-07 it was 1 regardless,
 * because of a missing GameFeatureData asset-manager rule (S1/Config/DefaultGame.ini) — if it is 1 again, read the log.
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
