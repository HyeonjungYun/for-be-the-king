// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Math/RandomStream.h"
#include "HAL/PlatformTime.h"
#include "Game/Vision/S1VisionSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * TD-009 (docs/tech-debt-register.md, 2026-10-18) — the fog's ray casting moved the per-wall work out of the per-ray loop.
 * The fog polygon must not move by a single bit: this compares the new path with the old one, byte for byte.
 *
 * Run headless:
 *   UnrealEditor-Cmd.exe S1.uproject -ExecCmds="Automation RunTests S1.Vision; Quit" -nullrhi -unattended
 */
namespace
{
	constexpr EAutomationTestFlags GS1VisionTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	using FWall = US1VisionSubsystem::FWall;

	FWall MakeWall(const FVector2D& Centre, float YawDeg, float HalfLength, float HalfThickness)
	{
		FWall Wall;
		Wall.Centre = Centre;
		Wall.HalfLength = HalfLength;
		Wall.HalfThickness = HalfThickness;
		const float YawRad = FMath::DegreesToRadians(YawDeg);
		Wall.Axis = FVector2D(FMath::Cos(YawRad), FMath::Sin(YawRad));
		Wall.BoundRadius = FMath::Sqrt(HalfLength * HalfLength + HalfThickness * HalfThickness);
		return Wall;
	}

	bool SameBits(const TArray<float>& A, const TArray<float>& B)
	{
		return A.Num() == B.Num() && FMemory::Memcmp(A.GetData(), B.GetData(), sizeof(float) * A.Num()) == 0;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1VisionRaysIdenticalTest, "S1.Vision.Rays.PreparedPath_IsBitIdenticalToReference", GS1VisionTestFlags)
bool FS1VisionRaysIdenticalTest::RunTest(const FString& Parameters)
{
	// Fixed seed - the same walls and origins every run
	FRandomStream Rng(1018);

	TArray<FWall> Walls;
	for (int32 i = 0; i < 400; i++)
	{
		const FVector2D Centre(Rng.FRandRange(-6000.f, 6000.f), Rng.FRandRange(-6000.f, 6000.f));
		// a quarter axis-aligned (rays parallel to a face hit the |D| < 1e-9 branch), the rest any angle
		const float Yaw = (Rng.RandRange(0, 3) == 0) ? 90.f * Rng.RandRange(0, 3) : Rng.FRandRange(0.f, 180.f);
		Walls.Add(MakeWall(Centre, Yaw, Rng.FRandRange(25.f, 600.f), Rng.FRandRange(10.f, 60.f)));
	}

	// Edge cases: an origin inside a wall, on a face, and on a corner
	Walls.Add(MakeWall(FVector2D(0.f, 0.f), 0.f, 200.f, 25.f));
	TArray<FVector2D> Origins = { FVector2D(0.f, 0.f), FVector2D(0.f, 25.f), FVector2D(200.f, 25.f), FVector2D(-200.f, -25.f) };
	for (int32 i = 0; i < 300; i++)
		Origins.Add(FVector2D(Rng.FRandRange(-6500.f, 6500.f), Rng.FRandRange(-6500.f, 6500.f)));

	int32 Mismatches = 0;
	double RefSeconds = 0.0, NewSeconds = 0.0;
	for (const int32 NumRays : { 240, 120 })
	{
		for (const float MaxCm : { 1500.f, 3000.f })
		{
			for (const FVector2D& Origin : Origins)
			{
				TArray<FWall> Near;
				for (const FWall& Wall : Walls)
				{
					if (FVector2D::Distance(Origin, Wall.Centre) - Wall.BoundRadius <= MaxCm)
						Near.Add(Wall);
				}

				TArray<float> Ref, New;
				const double T0 = FPlatformTime::Seconds();
				US1VisionSubsystem::CastRaysReference(Near, Origin, MaxCm, NumRays, Ref);
				const double T1 = FPlatformTime::Seconds();
				US1VisionSubsystem::CastRaysOver(Near, Origin, MaxCm, NumRays, New);
				const double T2 = FPlatformTime::Seconds();
				RefSeconds += T1 - T0;
				NewSeconds += T2 - T1;

				if (SameBits(Ref, New) == false)
				{
					if (Mismatches < 5)
						AddError(FString::Printf(TEXT("origin (%.1f, %.1f) rays %d max %.0f - distances differ"), Origin.X, Origin.Y, NumRays, MaxCm));
					Mismatches++;
				}
			}
		}
	}

	TestEqual(TEXT("mismatching origins"), Mismatches, 0);
	// Timing is informational only - a test must not depend on the machine
	AddInfo(FString::Printf(TEXT("reference %.1f ms · prepared %.1f ms (%d origins x 4 settings)"), RefSeconds * 1000.0, NewSeconds * 1000.0, Origins.Num()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1VisionRaysBasicsTest, "S1.Vision.Rays.FirstWallAndCap", GS1VisionTestFlags)
bool FS1VisionRaysBasicsTest::RunTest(const FString& Parameters)
{
	// A wall 10 m east (face at x = 975): ray 0 (+X) stops at the face, ray 180 deg (-X) runs to the cap
	TArray<FWall> Walls = { MakeWall(FVector2D(1000.f, 0.f), 90.f, 300.f, 25.f) };
	TArray<float> Out;
	US1VisionSubsystem::CastRaysOver(Walls, FVector2D::ZeroVector, 1500.f, 4, Out);
	TestEqual(TEXT("ray count"), Out.Num(), 4);
	TestTrue(TEXT("+X hits the face at 975"), FMath::IsNearlyEqual(Out[0], 975.f, 0.01f));
	TestEqual(TEXT("-X reaches the cap"), Out[2], 1500.f);
	return true;
}

#endif
