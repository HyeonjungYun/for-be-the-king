// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/Level/S1LevelLayout.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace S1LevelLayoutJson
{
	using FJsonArray = TArray<TSharedPtr<FJsonValue>>;

	const FJsonArray* ArrayField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name)
	{
		const FJsonArray* Out = nullptr;
		return Object->TryGetArrayField(Name, Out) ? Out : nullptr;
	}

	FString StringField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name)
	{
		FString Out;
		if (!Object->TryGetStringField(Name, Out))
		{
			// Ids are strings for houses / towers and numbers for entries / chests — accept either.
			double Number = 0.0;
			if (Object->TryGetNumberField(Name, Number))
			{
				Out = FString::FromInt(FMath::RoundToInt(Number));
			}
		}
		return Out;
	}

	double NumberField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, double Default = 0.0)
	{
		double Out = Default;
		Object->TryGetNumberField(Name, Out);
		return Out;
	}

	FVector2D XY(const TSharedPtr<FJsonObject>& Object)
	{
		return FVector2D(NumberField(Object, TEXT("x")), NumberField(Object, TEXT("y")));
	}

	/** [[x, y], [x, y], …] */
	bool Points(const FJsonArray* Array, TArray<FVector2D>& Out)
	{
		if (Array == nullptr)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			const FJsonArray& Pair = Value->AsArray();
			if (Pair.Num() != 2)
			{
				return false;
			}
			Out.Add(FVector2D(Pair[0]->AsNumber(), Pair[1]->AsNumber()));
		}
		return Out.Num() >= 2;
	}

	bool Routes(const FJsonArray* Array, TArray<FS1LevelRoute>& Out, FString& OutError, const TCHAR* What)
	{
		if (Array == nullptr)
		{
			return true;	// optional section
		}
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			const TSharedPtr<FJsonObject>& Object = Value->AsObject();
			FS1LevelRoute Route;
			Route.HouseId = StringField(Object, TEXT("house"));
			Route.TowerId = StringField(Object, TEXT("tower"));
			if (!Points(ArrayField(Object, TEXT("points")), Route.Points))
			{
				OutError = FString::Printf(TEXT("%s %s: points must be at least two [x, y] pairs"), What, *Route.HouseId);
				return false;
			}
			Out.Add(MoveTemp(Route));
		}
		return true;
	}
}

namespace
{
	double SegmentDistance(const FVector2D& P, const FVector2D& A, const FVector2D& B)
	{
		const FVector2D V = B - A;
		const double L2 = V.SizeSquared();
		const double T = L2 <= 0.0 ? 0.0 : FMath::Clamp(FVector2D::DotProduct(P - A, V) / L2, 0.0, 1.0);
		return FVector2D::Distance(P, A + V * T);
	}
}

bool FS1LevelLayout::IsInside(const FVector2D& P) const
{
	if (!HasOutline())
		return FMath::Max(FMath::Abs(P.X), FMath::Abs(P.Y)) <= SquareHalf;

	// Ray casting - the same test as check_layout.py inside_outline (x is north, the ray runs along +x)
	bool bInside = false;
	const int32 N = Outline.Num();
	for (int32 i = 0; i < N; i++)
	{
		const FVector2D& A = Outline[i];
		const FVector2D& B = Outline[(i + 1) % N];
		if ((A.Y > P.Y) != (B.Y > P.Y))
		{
			const double X = A.X + (P.Y - A.Y) * (B.X - A.X) / (B.Y - A.Y);
			if (P.X < X)
				bInside = !bInside;
		}
	}
	return bInside;
}

double FS1LevelLayout::BoundaryGap(const FVector2D& P) const
{
	if (!HasOutline())
		return SquareHalf - FMath::Max(FMath::Abs(P.X), FMath::Abs(P.Y));

	double Best = TNumericLimits<double>::Max();
	const int32 N = Outline.Num();
	for (int32 i = 0; i < N; i++)
		Best = FMath::Min(Best, SegmentDistance(P, Outline[i], Outline[(i + 1) % N]));
	return IsInside(P) ? Best : -Best;
}

double FS1LevelLayout::FloorArea() const
{
	if (!HasOutline())
		return FMath::Square(2.0 * SquareHalf);

	double Twice = 0.0;
	const int32 N = Outline.Num();
	for (int32 i = 0; i < N; i++)
	{
		const FVector2D& A = Outline[i];
		const FVector2D& B = Outline[(i + 1) % N];
		Twice += A.X * B.Y - B.X * A.Y;
	}
	return FMath::Abs(Twice) * 0.5;
}

void FS1LevelLayout::FloorBounds(double& OutX0, double& OutX1, double& OutY0, double& OutY1) const
{
	if (!HasOutline())
	{
		OutX0 = OutY0 = -SquareHalf;
		OutX1 = OutY1 = SquareHalf;
		return;
	}
	OutX0 = OutY0 = TNumericLimits<double>::Max();
	OutX1 = OutY1 = -TNumericLimits<double>::Max();
	for (const FVector2D& P : Outline)
	{
		OutX0 = FMath::Min(OutX0, P.X);
		OutX1 = FMath::Max(OutX1, P.X);
		OutY0 = FMath::Min(OutY0, P.Y);
		OutY1 = FMath::Max(OutY1, P.Y);
	}
}

FVector2D FS1LevelHouse::DoorPoint(const FS1LevelDoor& Door) const
{
	switch (Door.Side)
	{
	case TEXT('N'): return FVector2D(X1, Door.At);
	case TEXT('S'): return FVector2D(X0, Door.At);
	case TEXT('E'): return FVector2D(Door.At, Y1);
	default:        return FVector2D(Door.At, Y0);
	}
}

bool FS1LevelLayout::ParseJson(const FString& Json, FString& OutError)
{
	using namespace S1LevelLayoutJson;

	*this = FS1LevelLayout();

	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
	{
		OutError = TEXT("not a JSON object");
		return false;
	}

	Root->TryGetStringField(TEXT("boss_house"), BossHouse);

	if (const FJsonArray* Array = ArrayField(Root, TEXT("houses")))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			const TSharedPtr<FJsonObject>& Object = Value->AsObject();
			FS1LevelHouse House;
			House.Id = StringField(Object, TEXT("id"));
			House.X0 = NumberField(Object, TEXT("x0"));
			House.X1 = NumberField(Object, TEXT("x1"));
			House.Y0 = NumberField(Object, TEXT("y0"));
			House.Y1 = NumberField(Object, TEXT("y1"));
			Object->TryGetStringField(TEXT("grid"), House.GridFile);
			if (const FJsonArray* OpenAir = ArrayField(Object, TEXT("open_air")))
			{
				for (const TSharedPtr<FJsonValue>& RoomValue : *OpenAir)
				{
					House.OpenAirRooms.Add(RoomValue->AsString());
				}
			}

			if (const FJsonArray* Doors = ArrayField(Object, TEXT("doors")))
			{
				for (const TSharedPtr<FJsonValue>& DoorValue : *Doors)
				{
					const TSharedPtr<FJsonObject>& DoorObject = DoorValue->AsObject();
					const FString Side = StringField(DoorObject, TEXT("side"));
					if (Side.Len() != 1 || !FString(TEXT("NSEW")).Contains(Side))
					{
						OutError = FString::Printf(TEXT("house %s: door side '%s' is not N/S/E/W"), *House.Id, *Side);
						return false;
					}
					FS1LevelDoor Door;
					Door.Side = Side[0];
					Door.At = NumberField(DoorObject, TEXT("at"));
					Door.bRubble = StringField(DoorObject, TEXT("kind")) == TEXT("rubble");
					House.Doors.Add(Door);
				}
			}

			const FJsonArray* Inner = ArrayField(Object, TEXT("inner_chest"));
			if (Inner != nullptr && Inner->Num() == 2)
			{
				House.bHasInnerChest = true;
				House.InnerChest = FVector2D((*Inner)[0]->AsNumber(), (*Inner)[1]->AsNumber());
			}

			if (House.X1 <= House.X0 || House.Y1 <= House.Y0)
			{
				OutError = FString::Printf(TEXT("house %s: empty rectangle"), *House.Id);
				return false;
			}
			Houses.Add(MoveTemp(House));
		}
	}

	if (const FJsonArray* Array = ArrayField(Root, TEXT("towers")))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			const TSharedPtr<FJsonObject>& Object = Value->AsObject();
			Towers.Add({ StringField(Object, TEXT("id")), XY(Object) });
		}
	}

	if (const FJsonArray* Array = ArrayField(Root, TEXT("entries")))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			const TSharedPtr<FJsonObject>& Object = Value->AsObject();
			FS1LevelEntry Entry;
			Entry.Id = FMath::RoundToInt(NumberField(Object, TEXT("id")));
			Entry.bOuter = StringField(Object, TEXT("kind")) != TEXT("house");
			Entry.HouseId = StringField(Object, TEXT("house"));
			Entry.P = XY(Object);
			Entries.Add(MoveTemp(Entry));
		}
	}

	auto ReadChests = [](const FJsonArray* Array, TArray<FS1LevelChest>& Out)
	{
		if (Array == nullptr)
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			const TSharedPtr<FJsonObject>& Object = Value->AsObject();
			FS1LevelChest Chest;
			Chest.Id = FMath::RoundToInt(NumberField(Object, TEXT("id")));
			Chest.HouseId = StringField(Object, TEXT("house"));
			Chest.Room = StringField(Object, TEXT("room"));
			Chest.P = XY(Object);
			Out.Add(MoveTemp(Chest));
		}
	};
	ReadChests(ArrayField(Root, TEXT("open_chests")), OpenChests);
	ReadChests(ArrayField(Root, TEXT("house_chests")), HouseChests);

	if (const FJsonArray* Array = ArrayField(Root, TEXT("monsters")))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			const TSharedPtr<FJsonObject>& Object = Value->AsObject();
			Monsters.Add({ StringField(Object, TEXT("id")), StringField(Object, TEXT("house")), XY(Object) });
		}
	}

	if (const FJsonArray* Array = ArrayField(Root, TEXT("trails")))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			const TSharedPtr<FJsonObject>& Object = Value->AsObject();
			FS1LevelTrail Trail;
			Trail.Id = StringField(Object, TEXT("id"));
			Object->TryGetBoolField(TEXT("lantern"), Trail.bLantern);
			if (!Points(ArrayField(Object, TEXT("points")), Trail.Points))
			{
				OutError = FString::Printf(TEXT("trail %s: points must be at least two [x, y] pairs"), *Trail.Id);
				return false;
			}
			Trails.Add(MoveTemp(Trail));
		}
	}

	if (!Routes(ArrayField(Root, TEXT("lantern_routes")), LanternRoutes, OutError, TEXT("lantern route"))
		|| !Routes(ArrayField(Root, TEXT("detours")), Detours, OutError, TEXT("detour")))
	{
		return false;
	}

	if (const FJsonArray* Array = ArrayField(Root, TEXT("cover")))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			Cover.Add(XY(Value->AsObject()));
		}
	}

	// The island coast (2026-10-11) - optional; without it the floor is the old square
	if (const FJsonArray* Array = ArrayField(Root, TEXT("outline")))
	{
		if (!Points(Array, Outline) || Outline.Num() < 3)
		{
			OutError = TEXT("outline must be at least three [x, y] pairs");
			return false;
		}
	}
	double ChestCount = 0.0;
	if (Root->TryGetNumberField(TEXT("open_chest_count"), ChestCount))
		OpenChestCount = FMath::RoundToInt(ChestCount);

	if (Houses.Num() == 0 || Towers.Num() == 0 || Entries.Num() == 0)
	{
		OutError = TEXT("houses, towers and entries are required");
		return false;
	}
	return true;
}

bool FS1LevelLayout::LoadFromFile(const FString& Path, FString& OutError)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		OutError = FString::Printf(TEXT("cannot read %s"), *Path);
		return false;
	}
	return ParseJson(Text, OutError);
}

const FS1LevelHouse* FS1LevelLayout::FindHouse(const FString& Id) const
{
	return Houses.FindByPredicate([&Id](const FS1LevelHouse& House) { return House.Id == Id; });
}

const FS1LevelTower* FS1LevelLayout::FindTower(const FString& Id) const
{
	return Towers.FindByPredicate([&Id](const FS1LevelTower& Tower) { return Tower.Id == Id; });
}

bool FS1HouseGrid::Parse(const FString& Text, FString& OutError)
{
	Rows.Reset();

	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, /*bCullEmpty*/ false);
	for (FString& Line : Lines)
	{
		Line.RemoveFromEnd(TEXT("\r"));
		if (!Line.IsEmpty())
		{
			Rows.Add(Line);
		}
	}

	if (Rows.Num() != Size)
	{
		OutError = FString::Printf(TEXT("grid has %d rows, expected %d"), Rows.Num(), Size);
		return false;
	}
	for (int32 Row = 0; Row < Size; Row++)
	{
		if (Rows[Row].Len() != Size)
		{
			OutError = FString::Printf(TEXT("grid row %d has %d characters, expected %d"), Row, Rows[Row].Len(), Size);
			return false;
		}
	}
	return true;
}
