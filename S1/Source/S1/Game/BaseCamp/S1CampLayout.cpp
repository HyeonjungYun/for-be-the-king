// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/BaseCamp/S1CampLayout.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

bool FS1CampLayout::Parse(const FString& Csv, FString& OutError)
{
	bLoaded = false;
	Area = Spawn = FS1CampPoint();
	Npcs.Reset();
	Props.Reset();

	TArray<FString> Lines;
	Csv.ParseIntoArrayLines(Lines);
	if (Lines.Num() == 0)
	{
		OutError = TEXT("basecamp.csv is empty");
		return false;
	}

	// Header order is fixed by the server parser (BaseCampTable): kind,id,x,y,yaw,half_x,half_y
	TArray<FString> Header;
	Lines[0].TrimStartAndEnd().ParseIntoArray(Header, TEXT(","), false);
	if (Header.Num() < 7 || Header[0].Contains(TEXT("kind")) == false)
	{
		OutError = TEXT("basecamp.csv header must be kind,id,x,y,yaw,half_x,half_y");
		return false;
	}

	bool bHasArea = false;
	bool bHasSpawn = false;
	for (int32 i = 1; i < Lines.Num(); i++)
	{
		const FString Line = Lines[i].TrimStartAndEnd();
		if (Line.IsEmpty())
			continue;

		TArray<FString> Cells;
		Line.ParseIntoArray(Cells, TEXT(","), false);
		if (Cells.Num() < 7)
		{
			OutError = FString::Printf(TEXT("basecamp.csv line %d has %d cells"), i + 1, Cells.Num());
			return false;
		}

		FS1CampPoint P;
		P.Id = Cells[1].TrimStartAndEnd();
		P.X = FCString::Atof(*Cells[2]);
		P.Y = FCString::Atof(*Cells[3]);
		P.Yaw = FCString::Atof(*Cells[4]);
		P.HalfX = FCString::Atof(*Cells[5]);
		P.HalfY = FCString::Atof(*Cells[6]);

		const FString Kind = Cells[0].TrimStartAndEnd();
		if (Kind == TEXT("area")) { Area = P; bHasArea = true; }
		else if (Kind == TEXT("spawn")) { Spawn = P; bHasSpawn = true; }
		else if (Kind == TEXT("npc")) Npcs.Add(P);
		else if (Kind == TEXT("prop")) Props.Add(P);
		// dummy: the server spawns it - nothing to draw here
	}

	if (bHasArea == false || bHasSpawn == false || Area.HalfX <= 0.f || Area.HalfY <= 0.f)
	{
		OutError = TEXT("basecamp.csv needs an area and a spawn");
		return false;
	}

	bLoaded = true;
	return true;
}

bool FS1CampLayout::LoadFromDirectory(const FString& Directory, FString& OutError)
{
	FString Csv;
	const FString Path = FPaths::Combine(Directory, TEXT("basecamp.csv"));
	if (FFileHelper::LoadFileToString(Csv, *Path) == false)
	{
		OutError = FString::Printf(TEXT("cannot read %s"), *Path);
		return false;
	}
	return Parse(Csv, OutError);
}

const FS1CampPoint* FS1CampLayout::FindNpc(const FString& Id) const
{
	return Npcs.FindByPredicate([&Id](const FS1CampPoint& N) { return N.Id == Id; });
}

bool FS1CampLayout::Contains(float X, float Y) const
{
	return bLoaded && FMath::Abs(X - Area.X) <= Area.HalfX && FMath::Abs(Y - Area.Y) <= Area.HalfY;
}

const FS1CampPoint* FS1CampLayout::NearestNpc(float X, float Y, float MaxCm) const
{
	const FS1CampPoint* Best = nullptr;
	float BestDist = TNumericLimits<float>::Max();
	for (const FS1CampPoint& N : Npcs)
	{
		// Distance to the body's edge, not its centre - a wide stall still reads as "next to it"
		const float Dist = FMath::Max(0.f, FVector2D::Distance(FVector2D(X, Y), FVector2D(N.X, N.Y)) - FMath::Max(N.HalfX, N.HalfY));
		if (Dist <= MaxCm && Dist < BestDist)
		{
			Best = &N;
			BestDist = Dist;
		}
	}
	return Best;
}

FS1CampDialogue FS1CampLayout::DialogueFor(const FString& NpcId)
{
	FS1CampDialogue D;
	if (NpcId == TEXT("stash"))
	{
		D.NameKey = TEXT("camp.npc.stash");
		D.LineKey = TEXT("camp.line.stash");
		D.Options = { { TEXT("camp.option.stash"), ES1CampAction::OpenStash }, { TEXT("camp.option.bye"), ES1CampAction::Close } };
	}
	else if (NpcId == TEXT("enhance"))
	{
		D.NameKey = TEXT("camp.npc.enhance");
		D.LineKey = TEXT("camp.line.enhance");
		D.Options = { { TEXT("camp.option.enhance"), ES1CampAction::OpenEnhance }, { TEXT("camp.option.bye"), ES1CampAction::Close } };
	}
	else if (NpcId == TEXT("gate"))
	{
		D.NameKey = TEXT("camp.npc.gate");
		D.LineKey = TEXT("camp.line.gate");
		D.Options = { { TEXT("camp.option.gate"), ES1CampAction::OpenGate }, { TEXT("camp.option.party"), ES1CampAction::OpenParty },
			{ TEXT("camp.option.bye"), ES1CampAction::Close } };
	}
	return D;
}
