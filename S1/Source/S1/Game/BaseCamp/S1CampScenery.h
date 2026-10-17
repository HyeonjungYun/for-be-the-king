// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "S1CampScenery.generated.h"

class AS1Player;
class FS1CampLayout;
class UMaterialInterface;
class UStaticMesh;
class UStaticMeshComponent;

/** One NPC body standing in the camp — an AS1Player the server knows nothing about. */
struct FS1CampNpcBody
{
	FString						Id;			// stash · enhance · gate
	TWeakObjectPtr<AS1Player>	Body;
};

/**
 * The base-camp map's look (design/quick-specs/basecamp-map-2026-10-17.md) — built at runtime from basecamp.csv, the
 * same file the server walls are made from. Nothing here is a level asset: the camp lives in Floor1's sea, south of
 * the island, out of every dungeon player's sight, and is hidden while we are in the dungeon.
 *
 *   sea plate · ground plate (blocks the pawn) · fence with a gap at the gate · props (anvil, chests, gate posts + lintel)
 *   NPC bodies — AS1Player of the remote-player class, posed and dressed by role (the blacksmith holds a hammer)
 *
 * The server owns everything that moves: our body, the scarecrow, the walls. This only draws.
 */
UCLASS()
class S1_API AS1CampScenery : public AActor
{
	GENERATED_BODY()

public:
	AS1CampScenery();

	/** Builds once. BodyClass = US1GameInstance::OtherPlayerClass (the remote-player look). */
	void Build(const FS1CampLayout& Layout, TSubclassOf<AS1Player> BodyClass);

	/** Hidden in the dungeon — the camp is far out at sea, but never let it be the thing on screen. */
	void SetShown(bool bShown);

	const TArray<FS1CampNpcBody>& GetNpcBodies() const { return NpcBodies; }

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	UStaticMeshComponent* AddBox(const FVector& CenterCm, const FVector& SizeCm, float YawDeg, const FLinearColor& Color, bool bCollide,
		UMaterialInterface* Material = nullptr, bool bCylinder = false);

	UPROPERTY() TObjectPtr<UStaticMesh>			CubeMesh;
	UPROPERTY() TObjectPtr<UStaticMesh>			CylinderMesh;
	UPROPERTY() TObjectPtr<UMaterialInterface>	BaseMaterial;

	TArray<FS1CampNpcBody>	NpcBodies;
	bool					bBuilt = false;
};
