// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/Level/S1LevelRules.h"

// ── Report ────────────────────────────────────────────────────────────────────

void FS1LevelReport::Add(const FString& Name, double Value, double Limit, bool bAtLeast, const FString& Note)
{
	FS1LevelCheck& Check = Checks.AddDefaulted_GetRef();
	Check.Name = Name;
	Check.Value = Value;
	Check.Limit = Limit;
	Check.bAtLeast = bAtLeast;
	Check.Note = Note;
}

int32 FS1LevelReport::NumFailed() const
{
	int32 Failed = 0;
	for (const FS1LevelCheck& Check : Checks)
	{
		Failed += Check.Passed() ? 0 : 1;
	}
	return Failed;
}

const FS1LevelCheck* FS1LevelReport::Find(const FString& Prefix) const
{
	return Checks.FindByPredicate([&Prefix](const FS1LevelCheck& Check) { return Check.Name.StartsWith(Prefix); });
}

FString FS1LevelReport::ToString(bool bFailuresOnly) const
{
	FString Out;
	for (const FS1LevelCheck& Check : Checks)
	{
		if (bFailuresOnly && Check.Passed())
		{
			continue;
		}
		Out += FString::Printf(TEXT("%s  %-48s %9.2f %s %-8g margin %+8.2f  %s\n"),
			Check.Passed() ? TEXT("PASS") : TEXT("FAIL"), *Check.Name, Check.Value,
			Check.bAtLeast ? TEXT(">=") : TEXT("<="), Check.Limit, Check.Margin(), *Check.Note);
	}
	Out += FString::Printf(TEXT("%d checks, %d failed\n"), Checks.Num(), NumFailed());
	return Out;
}

namespace S1LevelRules
{
	// ── Geometry ──────────────────────────────────────────────────────────────

	double BoundaryGap(const FVector2D& P)
	{
		return FloorHalf - FMath::Max(FMath::Abs(P.X), FMath::Abs(P.Y));
	}

	void ForFloorGrid(const FS1LevelLayout& L, int32 Step, double Inset, const TFunctionRef<void(const FVector2D&)>& Visit)
	{
		if (!L.HasOutline())
		{
			const int32 Lo = FMath::CeilToInt(-FloorHalf + Inset);
			const int32 Hi = FMath::FloorToInt(FloorHalf - Inset);
			for (int32 X = Lo; X <= Hi; X += Step)
				for (int32 Y = Lo; Y <= Hi; Y += Step)
					Visit(FVector2D(X, Y));
			return;
		}

		double X0, X1, Y0, Y1;
		L.FloorBounds(X0, X1, Y0, Y1);
		for (int32 X = FMath::CeilToInt(X0); X <= FMath::FloorToInt(X1); X += Step)
		{
			for (int32 Y = FMath::CeilToInt(Y0); Y <= FMath::FloorToInt(Y1); Y += Step)
			{
				const FVector2D P(X, Y);
				if (L.BoundaryGap(P) >= Inset)
					Visit(P);
			}
		}
	}

	double RectDist(const FVector2D& P, const FS1LevelHouse& House)
	{
		const double Dx = FMath::Max3(House.X0 - P.X, 0.0, P.X - House.X1);
		const double Dy = FMath::Max3(House.Y0 - P.Y, 0.0, P.Y - House.Y1);
		return FMath::Sqrt(Dx * Dx + Dy * Dy);
	}

	double SegmentDist(const FVector2D& P, const FVector2D& A, const FVector2D& B)
	{
		const FVector2D V = B - A;
		const double L2 = V.SizeSquared();
		const double T = (L2 == 0.0) ? 0.0 : FMath::Clamp(FVector2D::DotProduct(P - A, V) / L2, 0.0, 1.0);
		return FVector2D::Distance(P, A + V * T);
	}

	double PolylineDist(const FVector2D& P, const TArray<FVector2D>& Points)
	{
		double Best = TNumericLimits<double>::Max();
		for (int32 i = 0; i + 1 < Points.Num(); i++)
		{
			Best = FMath::Min(Best, SegmentDist(P, Points[i], Points[i + 1]));
		}
		return Best;
	}

	double PolylineLength(const TArray<FVector2D>& Points)
	{
		double Total = 0.0;
		for (int32 i = 0; i + 1 < Points.Num(); i++)
		{
			Total += FVector2D::Distance(Points[i], Points[i + 1]);
		}
		return Total;
	}

	TArray<FVector2D> Sample(const TArray<FVector2D>& Points, double Step, double T0, double T1)
	{
		TArray<FVector2D> Out;
		const double Total = PolylineLength(Points);
		double Acc = 0.0;
		for (int32 i = 0; i + 1 < Points.Num(); i++)
		{
			const FVector2D A = Points[i];
			const FVector2D B = Points[i + 1];
			const double Seg = FVector2D::Distance(A, B);
			for (double S = 0.0; S <= Seg; S += Step)
			{
				const double F = (Total > 0.0) ? (Acc + S) / Total : 0.0;
				if (F >= T0 - 1e-9 && F <= T1 + 1e-9)
				{
					const double K = (Seg > 0.0) ? S / Seg : 0.0;
					Out.Add(A + (B - A) * K);
				}
			}
			Acc += Seg;
		}
		return Out;
	}

	bool Sees(double Dx, double Dy, double Margin)
	{
		constexpr double Ahead = 13.1, Behind = 8.0, HalfWidth = 15.5, Widening = 0.2887;
		if (Dx < -(Behind + Margin) || Dx > Ahead + Margin)
		{
			return false;
		}
		return FMath::Abs(Dy) <= HalfWidth + Widening * Dx + Margin;
	}

	// ── Formulas ──────────────────────────────────────────────────────────────

	double HouseMinShortSide(double RadarRadius, double Safety, double Band)
	{
		return 2.0 * (RadarRadius + Safety) + Band;
	}

	double LanternSpacingMax(double HeadingDeg, double HudBottom)
	{
		// level-map.md F2. x_bot_eff: the ground point at the bottom of the visible screen, HUD excluded.
		constexpr double Ahead = 13.1, HeightLoss = 1.111, LanternHeight = 2.0, HalfWidth = 15.5, Widening = 0.2887;
		constexpr double CameraBack = 13.0, CameraHeight = 23.5, PitchDeg = 60.0, Steering = 1.0;
		const double HalfVTan = FMath::Tan(FMath::DegreesToRadians(30.0)) * 9.0 / 16.0;	// 16:9, 60° horizontal
		const double BottomRay = FMath::DegreesToRadians(PitchDeg) + FMath::Atan(HalfVTan * (1.0 - 2.0 * HudBottom));
		const double Behind = -(-CameraBack + CameraHeight / FMath::Tan(BottomRay));

		const double Rad = FMath::DegreesToRadians(HeadingDeg);
		const double C = FMath::Cos(Rad);
		const double S = FMath::Abs(FMath::Sin(Rad));

		double Best = TNumericLimits<double>::Max();
		if (C > 1e-9)
		{
			Best = FMath::Min(Best, (Ahead - HeightLoss * LanternHeight) / C);
		}
		if (C < -1e-9)
		{
			Best = FMath::Min(Best, Behind / FMath::Abs(C));
		}
		const double Side = S - Widening * C;
		if (Side > 1e-9)
		{
			Best = FMath::Min(Best, HalfWidth / Side);
		}
		return Best - Steering;
	}

	double DetourClearance(double BetaDeg, double Margin)
	{
		const double Rad = FMath::DegreesToRadians(BetaDeg);
		return 13.1 * FMath::Abs(FMath::Sin(Rad)) + 19.3 * FMath::Abs(FMath::Cos(Rad)) + Margin;
	}

	double BlindSearchSeconds(double Area, int32 Towers, double RadarRadius, double Speed)
	{
		return Area / (Towers * 2.0 * RadarRadius * Speed);
	}

	// ── Layout checks (check_layout.py check) ─────────────────────────────────

	void CheckLayout(const FS1LevelLayout& L, FS1LevelReport& R, const FS1LevelLimits& Lim)
	{
		auto Corners = [](const FS1LevelHouse& H)
		{
			return TArray<FVector2D>{ { H.X0, H.Y0 }, { H.X0, H.Y1 }, { H.X1, H.Y0 }, { H.X1, H.Y1 } };
		};

		double Area = 0.0;
		for (const FS1LevelHouse& H : L.Houses)
		{
			const double W = H.X1 - H.X0;
			const double D = H.Y1 - H.Y0;
			Area += W * D;
			R.Add(FString::Printf(TEXT("house %s short side"), *H.Id), FMath::Min(W, D), Lim.HouseShortMin, true);
			R.Add(FString::Printf(TEXT("house %s long side"), *H.Id), FMath::Max(W, D), Lim.HouseLongMax, false);

			double Gap = TNumericLimits<double>::Max();
			for (const FVector2D& C : Corners(H))
			{
				Gap = FMath::Min(Gap, L.BoundaryGap(C));
			}
			R.Add(FString::Printf(TEXT("house %s boundary gap"), *H.Id), Gap, 0.0, true);

			TSet<TCHAR> Sides;
			for (const FS1LevelDoor& Door : H.Doors)
			{
				Sides.Add(Door.Side);
			}
			R.Add(FString::Printf(TEXT("house %s door sides"), *H.Id), Sides.Num(), 2, true);

			if (H.bHasInnerChest)
			{
				const FVector2D& P = H.InnerChest;
				const double Depth = FMath::Min(FMath::Min(P.X - H.X0, H.X1 - P.X), FMath::Min(P.Y - H.Y0, H.Y1 - P.Y));
				R.Add(FString::Printf(TEXT("house %s inner chest depth (F1)"), *H.Id), Depth, Lim.InnerChestWall, true);
			}
		}
		for (int32 i = 0; i < L.Houses.Num(); i++)
		{
			for (int32 j = i + 1; j < L.Houses.Num(); j++)
			{
				const FS1LevelHouse& A = L.Houses[i];
				const FS1LevelHouse& B = L.Houses[j];
				double Gap = TNumericLimits<double>::Max();
				for (const FVector2D& C : Corners(A)) { Gap = FMath::Min(Gap, RectDist(C, B)); }
				for (const FVector2D& C : Corners(B)) { Gap = FMath::Min(Gap, RectDist(C, A)); }
				R.Add(FString::Printf(TEXT("house %s-%s gap"), *A.Id, *B.Id), Gap, 0.0, true);
			}
		}
		const double Share = Area / L.FloorArea();
		R.Add(TEXT("house area share (R4) min"), Share, Lim.HouseAreaMin, true);
		R.Add(TEXT("house area share (R4) max"), Share, Lim.HouseAreaMax, false);

		// Towers (Core Rule 6) - 3 on the old square, 4 on the island (one near tower per house, the rest cover the far coast)
		R.Add(TEXT("tower count min"), L.Towers.Num(), Lim.TowerCountMin, true);
		R.Add(TEXT("tower count max"), L.Towers.Num(), Lim.TowerCountMax, false);
		for (int32 i = 0; i < L.Towers.Num(); i++)
		{
			for (int32 j = i + 1; j < L.Towers.Num(); j++)
			{
				R.Add(FString::Printf(TEXT("tower %s-%s separation"), *L.Towers[i].Id, *L.Towers[j].Id),
					FVector2D::Distance(L.Towers[i].P, L.Towers[j].P), Lim.TowerSeparation, true);
			}
		}
		for (const FS1LevelTower& T : L.Towers)
		{
			R.Add(FString::Printf(TEXT("tower %s boundary"), *T.Id), L.BoundaryGap(T.P), Lim.TowerBoundary, true);
			for (const FS1LevelHouse& H : L.Houses)
			{
				R.Add(FString::Printf(TEXT("tower %s - house %s wall"), *T.Id, *H.Id), RectDist(T.P, H), Lim.TowerHouse, true);
			}
		}
		for (const FS1LevelHouse& H : L.Houses)
		{
			TArray<TPair<double, FString>> PerTower;
			for (const FS1LevelTower& T : L.Towers)
			{
				double Best = TNumericLimits<double>::Max();
				for (const FS1LevelDoor& Door : H.Doors)
				{
					Best = FMath::Min(Best, FVector2D::Distance(H.DoorPoint(Door), T.P));
				}
				PerTower.Add({ Best, T.Id });
			}
			PerTower.Sort([](const TPair<double, FString>& A, const TPair<double, FString>& B) { return A.Key < B.Key; });
			if (PerTower.Num() == 0)
			{
				continue;
			}
			R.Add(FString::Printf(TEXT("house %s near tower %s (straight) min"), *H.Id, *PerTower[0].Value),
				PerTower[0].Key, Lim.NearTowerMin / 1.2, true, TEXT("path ~ x1.1-1.2"));
			R.Add(FString::Printf(TEXT("house %s near tower %s (straight) max"), *H.Id, *PerTower[0].Value),
				PerTower[0].Key, Lim.NearTowerMax / 1.1, false, TEXT("path <= 80"));
			for (int32 k = 1; k < PerTower.Num(); k++)
			{
				R.Add(FString::Printf(TEXT("house %s far tower %s"), *H.Id, *PerTower[k].Value), PerTower[k].Key, Lim.FarTowerMin, true);
			}
		}

		// Entry points (Core Rule 7)
		TArray<const FS1LevelEntry*> Outer;
		for (const FS1LevelEntry& E : L.Entries)
		{
			if (E.bOuter)
			{
				Outer.Add(&E);
			}
		}
		R.Add(TEXT("entry count"), L.Entries.Num(), Lim.EntryCount, true);
		R.Add(TEXT("entry count max"), L.Entries.Num(), Lim.EntryCount, false);
		for (const FS1LevelEntry* E : Outer)
		{
			const double Gap = L.BoundaryGap(E->P);
			R.Add(FString::Printf(TEXT("entry %d band min"), E->Id), Gap, Lim.EntryBandMin, true);
			R.Add(FString::Printf(TEXT("entry %d band max"), E->Id), Gap, Lim.EntryBandMax, false);
		}
		for (int32 i = 0; i < Outer.Num(); i++)
		{
			for (int32 j = i + 1; j < Outer.Num(); j++)
			{
				R.Add(FString::Printf(TEXT("entry %d-%d outer separation"), Outer[i]->Id, Outer[j]->Id),
					FVector2D::Distance(Outer[i]->P, Outer[j]->P), Lim.EntryOuterSeparation, true);
			}
		}
		if (Outer.Num() > 0)
		{
			// Numbered clockwise from north: compass bearing atan2(y, x) increases with the id.
			TArray<const FS1LevelEntry*> ByBearing = Outer;
			auto Bearing = [](const FS1LevelEntry* E)
			{
				return FMath::Fmod(FMath::RadiansToDegrees(FMath::Atan2(E->P.Y, E->P.X)) + 360.0, 360.0);
			};
			ByBearing.Sort([&](const FS1LevelEntry& A, const FS1LevelEntry& B) { return Bearing(&A) < Bearing(&B); });
			TArray<int32> Ids;
			for (const FS1LevelEntry* E : Outer) { Ids.Add(E->Id); }
			Ids.Sort();
			int32 Start = ByBearing.IndexOfByPredicate([&](const FS1LevelEntry* E) { return E->Id == Ids[0]; });
			bool bClockwise = true;
			for (int32 k = 0; k < Ids.Num(); k++)
			{
				bClockwise &= ByBearing[(Start + k) % ByBearing.Num()]->Id == Ids[k];
			}
			R.Add(TEXT("outer entries numbered clockwise from north"), bClockwise ? 1 : 0, 1, true);
		}
		for (const FS1LevelEntry& E : L.Entries)
		{
			if (E.bOuter)
			{
				continue;
			}
			const FS1LevelHouse* H = L.FindHouse(E.HouseId);
			if (H == nullptr)
			{
				R.Add(FString::Printf(TEXT("entry %d house %s exists"), E.Id, *E.HouseId), 0, 1, true);
				continue;
			}
			double Best = TNumericLimits<double>::Max();
			for (const FS1LevelDoor& Door : H->Doors)
			{
				Best = FMath::Min(Best, FVector2D::Distance(E.P, H->DoorPoint(Door)));
			}
			R.Add(FString::Printf(TEXT("entry %d to house %s door min"), E.Id, *H->Id), Best, Lim.EntryHouseMin, true);
			R.Add(FString::Printf(TEXT("entry %d to house %s door max"), E.Id, *H->Id), Best, Lim.EntryHouseMax, false);
		}
		for (const FS1LevelEntry& E : L.Entries)
		{
			double Tower = TNumericLimits<double>::Max();
			for (const FS1LevelTower& T : L.Towers) { Tower = FMath::Min(Tower, FVector2D::Distance(E.P, T.P)); }
			double Wall = TNumericLimits<double>::Max();
			for (const FS1LevelHouse& H : L.Houses) { Wall = FMath::Min(Wall, RectDist(E.P, H)); }
			R.Add(FString::Printf(TEXT("entry %d nearest tower"), E.Id), Tower, Lim.EntryTower, true);
			R.Add(FString::Printf(TEXT("entry %d clear radius (house walls)"), E.Id), Wall, Lim.EntryClear, true);
		}

		// Open-field chests (Core Rule 10)
		if (L.OpenChests.Num() > 0)
		{
			R.Add(TEXT("open chest count"), L.OpenChests.Num(), L.OpenChestCount > 0 ? L.OpenChestCount : Lim.OpenChestCount, true);
			for (const FS1LevelChest& C : L.OpenChests)
			{
				double House = TNumericLimits<double>::Max(), Tower = House, Entry = House;
				for (const FS1LevelHouse& H : L.Houses) { House = FMath::Min(House, RectDist(C.P, H)); }
				for (const FS1LevelTower& T : L.Towers) { Tower = FMath::Min(Tower, FVector2D::Distance(C.P, T.P)); }
				for (const FS1LevelEntry& E : L.Entries) { Entry = FMath::Min(Entry, FVector2D::Distance(C.P, E.P)); }
				R.Add(FString::Printf(TEXT("chest %d boundary"), C.Id), L.BoundaryGap(C.P), Lim.ChestBoundary, true);
				R.Add(FString::Printf(TEXT("chest %d house"), C.Id), House, Lim.ChestHouse, true);
				R.Add(FString::Printf(TEXT("chest %d tower"), C.Id), Tower, Lim.ChestTower, true);
				R.Add(FString::Printf(TEXT("chest %d entry"), C.Id), Entry, Lim.ChestEntry, true);
			}
			for (int32 i = 0; i < L.OpenChests.Num(); i++)
			{
				for (int32 j = i + 1; j < L.OpenChests.Num(); j++)
				{
					R.Add(FString::Printf(TEXT("chest %d-%d"), L.OpenChests[i].Id, L.OpenChests[j].Id),
						FVector2D::Distance(L.OpenChests[i].P, L.OpenChests[j].P), Lim.ChestSeparation, true);
				}
			}
		}

		// Monster spots (Core Rule 11)
		for (const FS1LevelMonster& M : L.Monsters)
		{
			double Entry = TNumericLimits<double>::Max();
			for (const FS1LevelEntry& E : L.Entries) { Entry = FMath::Min(Entry, FVector2D::Distance(M.P, E.P)); }
			R.Add(FString::Printf(TEXT("monster %s entry"), *M.Id), Entry, Lim.MonsterEntry, true);
		}

		// F5 — worst distance from anywhere on the floor (the island only, not the sea) to the nearest tower
		double Worst = 0.0;
		FVector2D WorstAt = FVector2D::ZeroVector;
		ForFloorGrid(L, 2, 0.0, [&](const FVector2D& P)
		{
			double Nearest = TNumericLimits<double>::Max();
			for (const FS1LevelTower& T : L.Towers) { Nearest = FMath::Min(Nearest, FVector2D::Distance(P, T.P)); }
			if (Nearest > Worst)
			{
				Worst = Nearest;
				WorstAt = P;
			}
		});
		R.Add(FString::Printf(TEXT("d_cover (F5) worst at (%.0f, %.0f)"), WorstAt.X, WorstAt.Y), Worst, Lim.DCoverMax, false,
			FString::Printf(TEXT("t_reach %.1fs"), Worst * 1.2 / 3.4));
	}

	// ── Routes (check_layout.py check_routes) ─────────────────────────────────

	namespace
	{
		bool SegmentHitsHouse(const FVector2D& A, const FVector2D& B, const FS1LevelHouse& H, double Shrink)
		{
			for (const FVector2D& P : Sample({ A, B }, 0.5))
			{
				if (H.X0 + Shrink < P.X && P.X < H.X1 - Shrink && H.Y0 + Shrink < P.Y && P.Y < H.Y1 - Shrink)
				{
					return true;
				}
			}
			return false;
		}

		bool PolylineHitsHouse(const TArray<FVector2D>& Points, const FS1LevelHouse& H)
		{
			for (int32 i = 0; i + 1 < Points.Num(); i++)
			{
				if (SegmentHitsHouse(Points[i], Points[i + 1], H, 0.6))
				{
					return true;
				}
			}
			return false;
		}

		double NearestDoor(const FVector2D& P, const FS1LevelHouse& H)
		{
			double Best = TNumericLimits<double>::Max();
			for (const FS1LevelDoor& Door : H.Doors)
			{
				Best = FMath::Min(Best, FVector2D::Distance(P, H.DoorPoint(Door)));
			}
			return Best;
		}
	}

	void CheckRoutes(const FS1LevelLayout& L, FS1LevelReport& R, const FS1LevelLimits& Lim)
	{
		if (L.Trails.Num() == 0)
		{
			return;
		}

		for (const FS1LevelTrail& T : L.Trails)
		{
			for (const FS1LevelHouse& H : L.Houses)
			{
				R.Add(FString::Printf(TEXT("trail %s stays out of house %s"), *T.Id, *H.Id), PolylineHitsHouse(T.Points, H) ? 0 : 1, 1, true);
			}
		}

		// One connected network: polylines touch where an end lies within 0.6 m of another polyline.
		const int32 N = L.Trails.Num();
		TArray<TSet<int32>> Adjacent;
		Adjacent.SetNum(N);
		for (int32 i = 0; i < N; i++)
		{
			for (int32 j = 0; j < N; j++)
			{
				if (i != j && (PolylineDist(L.Trails[i].Points[0], L.Trails[j].Points) < 0.6
					|| PolylineDist(L.Trails[i].Points.Last(), L.Trails[j].Points) < 0.6))
				{
					Adjacent[i].Add(j);
					Adjacent[j].Add(i);
				}
			}
		}
		TSet<int32> Seen;
		Seen.Add(0);
		TArray<int32> Stack;
		Stack.Add(0);
		while (Stack.Num() > 0)
		{
			for (int32 k : Adjacent[Stack.Pop()])
			{
				if (!Seen.Contains(k))
				{
					Seen.Add(k);
					Stack.Push(k);
				}
			}
		}
		R.Add(TEXT("trail network connected (polylines reached)"), Seen.Num(), N, true, FString::Printf(TEXT("%d/%d"), Seen.Num(), N));

		auto NearTrail = [&L](const FVector2D& P)
		{
			double Best = TNumericLimits<double>::Max();
			for (const FS1LevelTrail& T : L.Trails) { Best = FMath::Min(Best, PolylineDist(P, T.Points)); }
			return Best;
		};
		for (const FS1LevelHouse& H : L.Houses)
		{
			for (const FS1LevelDoor& Door : H.Doors)
			{
				R.Add(FString::Printf(TEXT("house %s door %c on a trail"), *H.Id, Door.Side), NearTrail(H.DoorPoint(Door)), 1.0, false);
			}
		}
		for (const FS1LevelTower& T : L.Towers)
		{
			R.Add(FString::Printf(TEXT("tower %s reached by a trail (ring edge)"), *T.Id), NearTrail(T.P), Lim.TowerRing + 0.5, false);
		}
		for (const FS1LevelEntry& E : L.Entries)
		{
			R.Add(FString::Printf(TEXT("entry %d beside a trail"), E.Id), NearTrail(E.P), 12.0, false);
		}

		// Lantern routes: from a door, on lantern trails, to a tower.
		for (const FS1LevelRoute& Route : L.LanternRoutes)
		{
			const FS1LevelHouse* H = L.FindHouse(Route.HouseId);
			const FS1LevelTower* T = L.FindTower(Route.TowerId);
			if (H == nullptr || T == nullptr)
			{
				R.Add(FString::Printf(TEXT("lantern %s->%s refers to a known house and tower"), *Route.HouseId, *Route.TowerId), 0, 1, true);
				continue;
			}
			const FString Tag = FString::Printf(TEXT("lantern %s->%s"), *Route.HouseId, *Route.TowerId);
			R.Add(Tag + TEXT(" starts at a door"), NearestDoor(Route.Points[0], *H), 1.0, false);
			R.Add(Tag + TEXT(" ends at the tower ring"), FVector2D::Distance(Route.Points.Last(), T->P), Lim.TowerRing + 0.5, false);

			double Off = 0.0;
			for (const FVector2D& P : Sample(Route.Points, 1.0))
			{
				if (FVector2D::Distance(P, T->P) <= Lim.TowerRing)
				{
					continue;	// the plaza itself is paved
				}
				double Nearest = TNumericLimits<double>::Max();
				for (const FS1LevelTrail& Trail : L.Trails)
				{
					if (Trail.bLantern)
					{
						Nearest = FMath::Min(Nearest, PolylineDist(P, Trail.Points));
					}
				}
				Off = FMath::Max(Off, Nearest);
			}
			R.Add(Tag + TEXT(" lies on lantern trails"), Off, 0.6, false);

			const double Length = PolylineLength(Route.Points);
			R.Add(Tag + TEXT(" path length"), Length, Lim.NearTowerMax, false, TEXT("40-80 m"));
			R.Add(Tag + TEXT(" path length min"), Length, Lim.NearTowerMin, true);
		}

		// Detours (F4, AC-LM-16): nobody on the lantern route ever has a detour walker (middle third) on screen.
		for (const FS1LevelRoute& Detour : L.Detours)
		{
			const FS1LevelRoute* Route = L.LanternRoutes.FindByPredicate([&](const FS1LevelRoute& Lr) { return Lr.HouseId == Detour.HouseId; });
			const FS1LevelHouse* H = L.FindHouse(Detour.HouseId);
			const FS1LevelTower* T = L.FindTower(Detour.TowerId);
			const FString Tag = FString::Printf(TEXT("detour %s"), *Detour.HouseId);
			if (Route == nullptr || H == nullptr || T == nullptr)
			{
				R.Add(Tag + TEXT(" has a lantern route, house and tower"), 0, 1, true);
				continue;
			}

			const TArray<FVector2D> A = Sample(Route->Points, 1.0);
			const TArray<FVector2D> B = Sample(Detour.Points, 1.0, 1.0 / 3.0, 2.0 / 3.0);
			int32 Visible = 0;
			double Clearance = TNumericLimits<double>::Max();
			for (const FVector2D& Pb : B)
			{
				Clearance = FMath::Min(Clearance, PolylineDist(Pb, Route->Points));
				for (const FVector2D& Pa : A)
				{
					const FVector2D D = Pb - Pa;
					if (Sees(D.X, D.Y) || Sees(-D.X, -D.Y))
					{
						Visible++;
					}
				}
			}
			R.Add(Tag + TEXT(" middle third never on lantern walkers' screen"), -Visible, 0, true,
				FString::Printf(TEXT("%d visible pairs"), Visible));

			const double Extra = PolylineLength(Detour.Points) - PolylineLength(Route->Points);
			R.Add(Tag + TEXT(" min distance to lantern route"), Clearance, 0.0, true,
				FString::Printf(TEXT("detour %.0f m, +%.0f m = +%.1fs"), PolylineLength(Detour.Points), Extra, Extra / 3.4));
			R.Add(Tag + TEXT(" starts at a door"), NearestDoor(Detour.Points[0], *H), 1.0, false);
			R.Add(Tag + TEXT(" ends at tower ring"), FVector2D::Distance(Detour.Points.Last(), T->P), Lim.TowerRing + 0.5, false);
			for (const FS1LevelHouse& Other : L.Houses)
			{
				R.Add(FString::Printf(TEXT("%s stays out of house %s"), *Tag, *Other.Id), PolylineHitsHouse(Detour.Points, Other) ? 0 : 1, 1, true);
			}
		}

		// Cover (Core Rule 9, F7): house walls and the boundary count as cover too.
		if (L.Cover.Num() == 0)
		{
			return;
		}
		double Worst = 0.0;
		FVector2D WorstAt = FVector2D::ZeroVector;
		ForFloorGrid(L, 2, 1.0, [&](const FVector2D& P)
		{
			bool bSkip = false;
			for (const FS1LevelHouse& H : L.Houses)
			{
				bSkip |= (H.X0 - 1 <= P.X && P.X <= H.X1 + 1 && H.Y0 - 1 <= P.Y && P.Y <= H.Y1 + 1);
			}
			for (const FS1LevelTower& T : L.Towers) { bSkip |= FVector2D::Distance(P, T.P) <= Lim.TowerRing; }
			for (const FS1LevelEntry& E : L.Entries) { bSkip |= FVector2D::Distance(P, E.P) <= Lim.EntryClear; }
			if (bSkip)
			{
				return;
			}
			double D = L.BoundaryGap(P);		// the coast is cover too (it blocks skills - the island's boundary walls)
			for (const FVector2D& C : L.Cover) { D = FMath::Min(D, FVector2D::Distance(P, C)); }
			for (const FS1LevelHouse& H : L.Houses) { D = FMath::Min(D, RectDist(P, H)); }
			if (D > Worst)
			{
				Worst = D;
				WorstAt = P;
			}
		});
		R.Add(FString::Printf(TEXT("cover gap (R9) worst at (%.0f, %.0f)"), WorstAt.X, WorstAt.Y), Worst, Lim.CoverMaxGap, false,
			FString::Printf(TEXT("%d cover pieces"), L.Cover.Num()));
		for (const FVector2D& C : L.Cover)
		{
			double Tower = TNumericLimits<double>::Max(), Entry = Tower;
			for (const FS1LevelTower& T : L.Towers) { Tower = FMath::Min(Tower, FVector2D::Distance(C, T.P)); }
			for (const FS1LevelEntry& E : L.Entries) { Entry = FMath::Min(Entry, FVector2D::Distance(C, E.P)); }
			R.Add(FString::Printf(TEXT("cover (%.0f,%.0f) outside tower rings"), C.X, C.Y), Tower, Lim.TowerRing + 1.0, true);
			R.Add(FString::Printf(TEXT("cover (%.0f,%.0f) outside entry circles"), C.X, C.Y), Entry, Lim.EntryClear + 1.0, true);
			R.Add(FString::Printf(TEXT("cover (%.0f,%.0f) off trails"), C.X, C.Y), NearTrail(C), 3.0, true);
		}
	}

	// ── House interior (check_house.py) ───────────────────────────────────────

	void CheckHouseGrid(const FS1HouseGrid& G, const FS1LevelHouse& House, FS1LevelReport& R, const FS1LevelLimits& Lim)
	{
		constexpr int32 N = FS1HouseGrid::N;
		const FString Tag = FString::Printf(TEXT("house %s grid"), *House.Id);
		const FIntPoint Steps[4] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
		auto InGrid = [](const FIntPoint& C) { return C.X >= 0 && C.X < N && C.Y >= 0 && C.Y < N; };

		// Rooms = components over open (' ') edges. FIntPoint(X = u, Y = v).
		TMap<FIntPoint, int32> Room;
		TArray<TArray<FIntPoint>> Rooms;
		for (int32 U = 0; U < N; U++)
		{
			for (int32 V = 0; V < N; V++)
			{
				if (Room.Contains({ U, V }))
				{
					continue;
				}
				const int32 Id = Rooms.Num();
				Rooms.AddDefaulted();
				TArray<FIntPoint> Queue;
				Queue.Add(FIntPoint(U, V));
				Room.Add(FIntPoint(U, V), Id);
				while (Queue.Num() > 0)
				{
					const FIntPoint C = Queue.Pop();
					Rooms[Id].Add(C);
					for (const FIntPoint& S : Steps)
					{
						const FIntPoint Next = C + S;
						if (InGrid(Next) && !Room.Contains(Next) && G.Edge(C.X, C.Y, S.X, S.Y) == TEXT(' '))
						{
							Room.Add(Next, Id);
							Queue.Add(Next);
						}
					}
				}
			}
		}

		// Openings: internal points (local metres) and outer ones with their cell and kind.
		struct FOpening { FVector2D P; TCHAR Kind; FIntPoint Cell; TCHAR Side; };
		TArray<FVector2D> AllOpenings;
		TArray<FOpening> Outer;
		for (int32 U = 0; U < N; U++)
		{
			for (int32 V = 0; V < N; V++)
			{
				for (const FIntPoint& S : { FIntPoint(1, 0), FIntPoint(0, 1) })
				{
					if (U + S.X < N && V + S.Y < N)
					{
						const TCHAR Ch = G.Edge(U, V, S.X, S.Y);
						if (Ch == TEXT('D') || Ch == TEXT('~'))
						{
							AllOpenings.Add(FVector2D(4 * U + 2 + 2 * S.X, 4 * V + 2 + 2 * S.Y));
						}
					}
				}
			}
		}
		for (int32 V = 0; V < N; V++)
		{
			const TCHAR North = G.At(0, 2 * V + 1);
			const TCHAR South = G.At(28, 2 * V + 1);
			if (North == TEXT('D') || North == TEXT('~')) { Outer.Add({ FVector2D(56, 4 * V + 2), North, { N - 1, V }, TEXT('N') }); }
			if (South == TEXT('D') || South == TEXT('~')) { Outer.Add({ FVector2D(0, 4 * V + 2), South, { 0, V }, TEXT('S') }); }
		}
		for (int32 U = 0; U < N; U++)
		{
			const TCHAR West = G.At(27 - 2 * U, 0);
			const TCHAR East = G.At(27 - 2 * U, 28);
			if (West == TEXT('D') || West == TEXT('~')) { Outer.Add({ FVector2D(4 * U + 2, 0), West, { U, 0 }, TEXT('W') }); }
			if (East == TEXT('D') || East == TEXT('~')) { Outer.Add({ FVector2D(4 * U + 2, 56), East, { U, N - 1 }, TEXT('E') }); }
		}
		for (const FOpening& O : Outer)
		{
			AllOpenings.Add(O.P);
		}

		auto Reach = [&](const TArray<FIntPoint>& Starts)
		{
			TSet<FIntPoint> Seen;
			Seen.Append(Starts);
			TArray<FIntPoint> Queue = Starts;
			while (Queue.Num() > 0)
			{
				const FIntPoint C = Queue.Pop();
				for (const FIntPoint& S : Steps)
				{
					const FIntPoint Next = C + S;
					if (InGrid(Next) && !Seen.Contains(Next) && FS1HouseGrid::IsPassable(G.Edge(C.X, C.Y, S.X, S.Y)))
					{
						Seen.Add(Next);
						Queue.Add(Next);
					}
				}
			}
			return Seen.Num();
		};

		TArray<FIntPoint> Starts;
		for (const FOpening& O : Outer) { Starts.Add(O.Cell); }
		R.Add(Tag + TEXT(" every cell reachable from outside"), Reach(Starts), N * N, true);
		for (int32 i = 0; i < Outer.Num(); i++)
		{
			TArray<FIntPoint> Rest;
			for (int32 j = 0; j < Outer.Num(); j++)
			{
				if (j != i) { Rest.Add(Outer[j].Cell); }
			}
			R.Add(FString::Printf(TEXT("%s blocking outer %c (%d,%d) keeps every cell reachable"), *Tag, Outer[i].Side, Outer[i].Cell.X, Outer[i].Cell.Y),
				Reach(Rest), N * N, true);
		}

		// Every outer opening matches a layout door at the same world point, and vice versa.
		int32 Unmatched = 0;
		for (const FOpening& O : Outer)
		{
			const FVector2D World(House.X0 + O.P.X, House.Y0 + O.P.Y);
			bool bFound = false;
			for (const FS1LevelDoor& Door : House.Doors) { bFound |= FVector2D::Distance(World, House.DoorPoint(Door)) < 0.5; }
			Unmatched += bFound ? 0 : 1;
		}
		R.Add(Tag + TEXT(" outer openings match layout doors"), -Unmatched, 0, true);
		R.Add(Tag + TEXT(" layout doors match outer openings"), Outer.Num(), House.Doors.Num(), true,
			FString::Printf(TEXT("%d openings, %d doors"), Outer.Num(), House.Doors.Num()));

		// The inner room covers the centre four cells.
		const int32 Inner = Room[{ 6, 6 }];
		const bool bCentre = Room[{ 6, 7 }] == Inner && Room[{ 7, 6 }] == Inner && Room[{ 7, 7 }] == Inner;
		R.Add(Tag + TEXT(" centre cells are one room"), bCentre ? 1 : 0, 1, true);

		TSet<int32> FirstRooms;
		for (const FOpening& O : Outer)
		{
			if (O.Kind == TEXT('D')) { FirstRooms.Add(Room[O.Cell]); }
		}

		int32 Chests = 0;
		bool bInner = false, bFirst = false;
		for (int32 U = 0; U < N; U++)
		{
			for (int32 V = 0; V < N; V++)
			{
				if (G.CellChar(U, V) != TEXT('c'))
				{
					continue;
				}
				Chests++;
				const FVector2D Lp(4 * U + 2, 4 * V + 2);
				const double Depth = FMath::Min(FMath::Min(Lp.X, 56.0 - Lp.X), FMath::Min(Lp.Y, 56.0 - Lp.Y));
				double Opening = TNumericLimits<double>::Max();
				for (const FVector2D& O : AllOpenings) { Opening = FMath::Min(Opening, FVector2D::Distance(Lp, O)); }
				R.Add(FString::Printf(TEXT("%s chest (%d,%d) from openings"), *Tag, U, V), Opening, 3.0, true);

				const int32 Id = Room[{ U, V }];
				bFirst |= FirstRooms.Contains(Id);
				if (Id == Inner)
				{
					bInner = true;
					R.Add(FString::Printf(TEXT("%s inner chest (%d,%d) depth"), *Tag, U, V), Depth, Lim.InnerChestWall, true);
					R.Add(FString::Printf(TEXT("%s inner chest (%d,%d) off centre"), *Tag, U, V),
						FMath::Max(FMath::Abs(Lp.X - 28.0), FMath::Abs(Lp.Y - 28.0)), 4.0, false);
					if (House.bHasInnerChest)
					{
						const FVector2D World(House.X0 + Lp.X, House.Y0 + Lp.Y);
						R.Add(Tag + TEXT(" inner chest matches layout"), FVector2D::Distance(World, House.InnerChest), 0.5, false);
					}
				}
			}
		}
		R.Add(Tag + TEXT(" chest count"), Chests, 4, true);
		R.Add(Tag + TEXT(" chest count max"), Chests, 4, false);
		R.Add(Tag + TEXT(" has an inner-room chest"), bInner ? 1 : 0, 1, true);
		R.Add(Tag + TEXT(" has a first-room chest"), bFirst ? 1 : 0, 1, true);
	}

	// ── Round trip ────────────────────────────────────────────────────────────

	TArray<FString> CompareLayouts(const FS1LevelLayout& E, const FS1LevelLayout& A, double Tol)
	{
		TArray<FString> Diffs;
		auto Near = [Tol](const FVector2D& P, const FVector2D& Q) { return FVector2D::Distance(P, Q) <= Tol; };
		auto Count = [&Diffs](const TCHAR* What, int32 Expected, int32 Actual)
		{
			if (Expected != Actual)
			{
				Diffs.Add(FString::Printf(TEXT("%s: expected %d, got %d"), What, Expected, Actual));
				return false;
			}
			return true;
		};

		if (Count(TEXT("houses"), E.Houses.Num(), A.Houses.Num()))
		{
			for (const FS1LevelHouse& H : E.Houses)
			{
				const FS1LevelHouse* G = A.FindHouse(H.Id);
				if (G == nullptr)
				{
					Diffs.Add(FString::Printf(TEXT("house %s missing"), *H.Id));
					continue;
				}
				if (!Near({ H.X0, H.Y0 }, { G->X0, G->Y0 }) || !Near({ H.X1, H.Y1 }, { G->X1, G->Y1 }))
				{
					Diffs.Add(FString::Printf(TEXT("house %s rectangle moved"), *H.Id));
				}
				if (H.Doors.Num() != G->Doors.Num())
				{
					Diffs.Add(FString::Printf(TEXT("house %s doors: expected %d, got %d"), *H.Id, H.Doors.Num(), G->Doors.Num()));
				}
				else
				{
					for (int32 i = 0; i < H.Doors.Num(); i++)
					{
						if (H.Doors[i].Side != G->Doors[i].Side || FMath::Abs(H.Doors[i].At - G->Doors[i].At) > Tol || H.Doors[i].bRubble != G->Doors[i].bRubble)
						{
							Diffs.Add(FString::Printf(TEXT("house %s door %d differs"), *H.Id, i));
						}
					}
				}
				if (H.bHasInnerChest != G->bHasInnerChest || (H.bHasInnerChest && !Near(H.InnerChest, G->InnerChest)))
				{
					Diffs.Add(FString::Printf(TEXT("house %s inner chest differs"), *H.Id));
				}
			}
		}

		if (Count(TEXT("towers"), E.Towers.Num(), A.Towers.Num()))
		{
			for (const FS1LevelTower& T : E.Towers)
			{
				const FS1LevelTower* G = A.FindTower(T.Id);
				if (G == nullptr || !Near(T.P, G->P))
				{
					Diffs.Add(FString::Printf(TEXT("tower %s missing or moved"), *T.Id));
				}
			}
		}

		if (Count(TEXT("entries"), E.Entries.Num(), A.Entries.Num()))
		{
			for (const FS1LevelEntry& En : E.Entries)
			{
				const FS1LevelEntry* G = A.Entries.FindByPredicate([&](const FS1LevelEntry& X) { return X.Id == En.Id; });
				if (G == nullptr || !Near(En.P, G->P) || En.bOuter != G->bOuter || En.HouseId != G->HouseId)
				{
					Diffs.Add(FString::Printf(TEXT("entry %d missing or differs"), En.Id));
				}
			}
		}

		auto Chests = [&](const TCHAR* What, const TArray<FS1LevelChest>& Ex, const TArray<FS1LevelChest>& Ac)
		{
			if (!Count(What, Ex.Num(), Ac.Num()))
			{
				return;
			}
			for (const FS1LevelChest& C : Ex)
			{
				const FS1LevelChest* G = Ac.FindByPredicate([&](const FS1LevelChest& X) { return X.Id == C.Id; });
				if (G == nullptr || !Near(C.P, G->P) || C.HouseId != G->HouseId)
				{
					Diffs.Add(FString::Printf(TEXT("%s %d missing or differs"), What, C.Id));
				}
			}
		};
		Chests(TEXT("open chest"), E.OpenChests, A.OpenChests);
		Chests(TEXT("house chest"), E.HouseChests, A.HouseChests);

		if (Count(TEXT("monsters"), E.Monsters.Num(), A.Monsters.Num()))
		{
			for (const FS1LevelMonster& M : E.Monsters)
			{
				const FS1LevelMonster* G = A.Monsters.FindByPredicate([&](const FS1LevelMonster& X) { return X.Id == M.Id; });
				if (G == nullptr || !Near(M.P, G->P))
				{
					Diffs.Add(FString::Printf(TEXT("monster %s missing or moved"), *M.Id));
				}
			}
		}

		auto Polylines = [&](const TCHAR* What, const FString& Id, const TArray<FVector2D>& Ex, const TArray<FVector2D>& Ac)
		{
			if (Ex.Num() != Ac.Num())
			{
				Diffs.Add(FString::Printf(TEXT("%s %s: expected %d points, got %d"), What, *Id, Ex.Num(), Ac.Num()));
				return;
			}
			for (int32 i = 0; i < Ex.Num(); i++)
			{
				if (!Near(Ex[i], Ac[i]))
				{
					Diffs.Add(FString::Printf(TEXT("%s %s point %d moved"), What, *Id, i));
					return;
				}
			}
		};
		if (Count(TEXT("trails"), E.Trails.Num(), A.Trails.Num()))
		{
			for (const FS1LevelTrail& T : E.Trails)
			{
				const FS1LevelTrail* G = A.Trails.FindByPredicate([&](const FS1LevelTrail& X) { return X.Id == T.Id; });
				if (G == nullptr || G->bLantern != T.bLantern)
				{
					Diffs.Add(FString::Printf(TEXT("trail %s missing or lantern flag differs"), *T.Id));
					continue;
				}
				Polylines(TEXT("trail"), T.Id, T.Points, G->Points);
			}
		}
		auto Routes = [&](const TCHAR* What, const TArray<FS1LevelRoute>& Ex, const TArray<FS1LevelRoute>& Ac)
		{
			if (!Count(What, Ex.Num(), Ac.Num()))
			{
				return;
			}
			for (const FS1LevelRoute& Ro : Ex)
			{
				const FS1LevelRoute* G = Ac.FindByPredicate([&](const FS1LevelRoute& X) { return X.HouseId == Ro.HouseId && X.TowerId == Ro.TowerId; });
				if (G == nullptr)
				{
					Diffs.Add(FString::Printf(TEXT("%s %s->%s missing"), What, *Ro.HouseId, *Ro.TowerId));
					continue;
				}
				Polylines(What, Ro.HouseId, Ro.Points, G->Points);
			}
		};
		Routes(TEXT("lantern route"), E.LanternRoutes, A.LanternRoutes);
		Routes(TEXT("detour"), E.Detours, A.Detours);

		if (Count(TEXT("cover"), E.Cover.Num(), A.Cover.Num()))
		{
			for (const FVector2D& C : E.Cover)
			{
				if (!A.Cover.ContainsByPredicate([&](const FVector2D& X) { return Near(C, X); }))
				{
					Diffs.Add(FString::Printf(TEXT("cover (%.1f, %.1f) missing"), C.X, C.Y));
				}
			}
		}

		if (E.BossHouse != A.BossHouse)
		{
			Diffs.Add(FString::Printf(TEXT("boss house: expected %s, got %s"), *E.BossHouse, *A.BossHouse));
		}

		// The coast is part of the floor (island 2026-10-11) - its walls and floor_outline.csv come from it
		Polylines(TEXT("outline"), TEXT("coast"), E.Outline, A.Outline);
		if (E.OpenChestCount != A.OpenChestCount)
		{
			Diffs.Add(FString::Printf(TEXT("open chest count rule: expected %d, got %d"), E.OpenChestCount, A.OpenChestCount));
		}
		return Diffs;
	}
}

// ── House rooms ───────────────────────────────────────────────────────────────

void FS1HouseRooms::Build(const FS1HouseGrid& G)
{
	constexpr int32 N = FS1HouseGrid::N;
	const FIntPoint Steps[4] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };

	CellRoom.Reset();
	Cells.Reset();
	Labels.Reset();
	for (int32 U = 0; U < N; U++)
	{
		for (int32 V = 0; V < N; V++)
		{
			if (CellRoom.Contains(FIntPoint(U, V)))
			{
				continue;
			}
			const int32 Id = Cells.Num();
			Cells.AddDefaulted();
			TArray<FIntPoint> Queue;
			Queue.Add(FIntPoint(U, V));
			CellRoom.Add(FIntPoint(U, V), Id);
			while (Queue.Num() > 0)
			{
				const FIntPoint C = Queue.Pop();
				Cells[Id].Add(C);
				for (const FIntPoint& S : Steps)
				{
					const FIntPoint Next = C + S;
					if (Next.X >= 0 && Next.X < N && Next.Y >= 0 && Next.Y < N && !CellRoom.Contains(Next)
						&& G.Edge(C.X, C.Y, S.X, S.Y) == TEXT(' '))
					{
						CellRoom.Add(Next, Id);
						Queue.Add(Next);
					}
				}
			}

			// The room's letter: the most common cell character other than chest / boss marks.
			TMap<TCHAR, int32> Votes;
			for (const FIntPoint& C : Cells[Id])
			{
				const TCHAR Ch = G.CellChar(C.X, C.Y);
				if (Ch != TEXT('c') && Ch != TEXT('B'))
				{
					Votes.FindOrAdd(Ch)++;
				}
			}
			TCHAR Best = TEXT('?');
			int32 BestVotes = 0;
			for (const TPair<TCHAR, int32>& Vote : Votes)
			{
				if (Vote.Value > BestVotes)
				{
					Best = Vote.Key;
					BestVotes = Vote.Value;
				}
			}
			Labels.Add(Best);
		}
	}
}

TSet<int32> FS1HouseRooms::FirstRooms(const FS1HouseGrid& G) const
{
	constexpr int32 N = FS1HouseGrid::N;
	TSet<int32> Out;
	for (int32 V = 0; V < N; V++)
	{
		if (G.At(0, 2 * V + 1) == TEXT('D')) { Out.Add(CellRoom.FindRef(FIntPoint(N - 1, V))); }
		if (G.At(28, 2 * V + 1) == TEXT('D')) { Out.Add(CellRoom.FindRef(FIntPoint(0, V))); }
	}
	for (int32 U = 0; U < N; U++)
	{
		if (G.At(27 - 2 * U, 0) == TEXT('D')) { Out.Add(CellRoom.FindRef(FIntPoint(U, 0))); }
		if (G.At(27 - 2 * U, 28) == TEXT('D')) { Out.Add(CellRoom.FindRef(FIntPoint(U, N - 1))); }
	}
	return Out;
}
