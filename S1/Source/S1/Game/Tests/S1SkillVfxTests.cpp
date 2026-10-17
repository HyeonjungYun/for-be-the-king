// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Game/Skill/S1SwordWave.h"

#if WITH_DEV_AUTOMATION_TESTS

/** Projectile looks (skill_vfx.csv) and the upright sword wave of 검의 찬양 1009 (2026-10-15). */
namespace
{
	constexpr EAutomationTestFlags GS1VfxTestFlags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1SkillVfxParseTest, "S1.Vfx.Looks_ParseAndReject", GS1VfxTestFlags)
bool FS1SkillVfxParseTest::RunTest(const FString& Parameters)
{
	// Arrange
	const FString Header = TEXT("skill_id,projectile,height_cm,depth_cm,thickness_cm,colour\n");
	TMap<uint32, FS1ProjectileLook> Looks;
	FString Error;

	// Act / Assert
	TestTrue(TEXT("good row"), S1SwordWave::ParseLooks(Header + TEXT("1009,crescent_v,220,55,28,0.78 0.35 0.9\r\n"), Looks, Error));
	if (const FS1ProjectileLook* Look = Looks.Find(1009))
	{
		TestEqual(TEXT("kind"), Look->Kind, FName(TEXT("crescent_v")));
		TestEqual(TEXT("height"), Look->HeightCm, 220.f);
		TestEqual(TEXT("colour g"), Look->Colour.G, 0.35f);
	}
	else
	{
		AddError(TEXT("1009 not parsed"));
	}
	TestFalse(TEXT("unknown kind rejected"), S1SwordWave::ParseLooks(Header + TEXT("1009,bolt,220,55,28,1 1 1\n"), Looks, Error));
	TestFalse(TEXT("thickness = depth rejected (boundary)"), S1SwordWave::ParseLooks(Header + TEXT("1009,crescent_v,220,55,55,1 1 1\n"), Looks, Error));
	TestTrue(TEXT("thickness just under depth accepted"), S1SwordWave::ParseLooks(Header + TEXT("1009,crescent_v,220,55,54,1 1 1\n"), Looks, Error));
	TestFalse(TEXT("two-number colour rejected"), S1SwordWave::ParseLooks(Header + TEXT("1009,crescent_v,220,55,28,1 1\n"), Looks, Error));
	TestFalse(TEXT("duplicate skill rejected"), S1SwordWave::ParseLooks(Header + TEXT("1009,crescent_v,220,55,28,1 1 1\n1009,crescent_v,220,55,28,1 1 1\n"), Looks, Error));
	TestFalse(TEXT("missing column rejected"), S1SwordWave::ParseLooks(TEXT("skill_id,projectile\n1009,crescent_v\n"), Looks, Error));

	// sink_pct (optional): read, 0 when the column is absent, 0..90
	const FString SinkHeader = TEXT("skill_id,projectile,height_cm,depth_cm,thickness_cm,colour,sink_pct\n");
	TestTrue(TEXT("sink read"), S1SwordWave::ParseLooks(SinkHeader + TEXT("1009,crescent_v,700,175,70,0.05 0.3 1,50\n"), Looks, Error));
	TestEqual(TEXT("sink 50"), Looks.FindRef(1009).SinkPct, 50.f);
	TestTrue(TEXT("no sink column"), S1SwordWave::ParseLooks(Header + TEXT("1009,crescent_v,220,55,28,1 1 1\n"), Looks, Error));
	TestEqual(TEXT("sink defaults to 0"), Looks.FindRef(1009).SinkPct, 0.f);
	TestTrue(TEXT("sink 90 accepted (boundary)"), S1SwordWave::ParseLooks(SinkHeader + TEXT("1009,crescent_v,700,175,70,1 1 1,90\n"), Looks, Error));
	TestFalse(TEXT("sink 91 rejected"), S1SwordWave::ParseLooks(SinkHeader + TEXT("1009,crescent_v,700,175,70,1 1 1,91\n"), Looks, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1SkillVfxCrescentTest, "S1.Vfx.Crescent_StandsUprightEdgeFirst", GS1VfxTestFlags)
bool FS1SkillVfxCrescentTest::RunTest(const FString& Parameters)
{
	// Arrange
	FS1ProjectileLook Look;
	Look.HeightCm = 220.f;
	Look.DepthCm = 55.f;
	Look.ThicknessCm = 28.f;
	constexpr int32 Segments = 24;

	// Act
	const FS1SwordWaveMesh Mesh = S1SwordWave::BuildCrescent(Look, Segments, 0);

	// Assert - one strip: lead · body · trail per step, two bands (four triangles) per segment, every index in range
	constexpr int32 Rows = FS1SwordWaveMesh::RowsPerStep;
	TestEqual(TEXT("vertices"), Mesh.Vertices.Num(), (Segments + 1) * Rows);
	TestEqual(TEXT("colours match"), Mesh.Colours.Num(), Mesh.Vertices.Num());
	TestEqual(TEXT("indices"), Mesh.Triangles.Num(), Segments * 12);
	bool bInRange = true;
	for (int32 Index : Mesh.Triangles)
		bInRange &= Mesh.Vertices.IsValidIndex(Index);
	TestTrue(TEXT("indices valid"), bInRange);

	// Upright and flat: the blade lives in the plane of flight (X) and up (Z), no width
	float MinZ = 1e9f, MaxZ = -1e9f, MaxX = -1e9f, MaxAbsY = 0.f;
	for (const FVector& V : Mesh.Vertices)
	{
		MinZ = FMath::Min(MinZ, static_cast<float>(V.Z));
		MaxZ = FMath::Max(MaxZ, static_cast<float>(V.Z));
		MaxX = FMath::Max(MaxX, static_cast<float>(V.X));
		MaxAbsY = FMath::Max(MaxAbsY, static_cast<float>(FMath::Abs(V.Y)));
	}
	TestTrue(TEXT("tip to tip = height"), FMath::IsNearlyEqual(MaxZ - MinZ, 220.f, 0.5f));
	TestTrue(TEXT("centred on the origin"), FMath::IsNearlyEqual(MaxZ, 110.f, 0.5f));
	TestTrue(TEXT("edge first: the middle bulges forward by depth"), FMath::IsNearlyEqual(MaxX, 55.f, 0.5f));
	TestTrue(TEXT("no width"), MaxAbsY < UE_KINDA_SMALL_NUMBER);

	// The middle step: leading edge at the front, trailing edge one thickness behind; the tips come to a point
	const int32 Mid = (Segments / 2) * Rows;
	const FVector& LeadMid = Mesh.Vertices[Mid];
	const FVector& TrailMid = Mesh.Vertices[Mid + 2];
	TestTrue(TEXT("middle thickness"), FMath::IsNearlyEqual(static_cast<float>(LeadMid.X - TrailMid.X), 28.f, 0.5f));
	TestTrue(TEXT("body between the edges"), Mesh.Vertices[Mid + 1].X < LeadMid.X && Mesh.Vertices[Mid + 1].X > TrailMid.X);
	TestTrue(TEXT("bottom tip is a point"), Mesh.Vertices[0].Equals(Mesh.Vertices[2], 0.5f));
	TestTrue(TEXT("leading edge whiter than the body"), Mesh.Colours[Mid].G > Mesh.Colours[Mid + 1].G);
	TestTrue(TEXT("trailing edge fainter than the body"), Mesh.Colours[Mid + 2].A < Mesh.Colours[Mid + 1].A);

	// Echoes trail behind, fainter
	const FS1SwordWaveMesh WithEchoes = S1SwordWave::BuildCrescent(Look, Segments, 2, 32.f);
	const int32 Strip = (Segments + 1) * Rows;
	TestEqual(TEXT("three strips"), WithEchoes.Vertices.Num(), Strip * 3);
	TestTrue(TEXT("echo sits behind"), WithEchoes.Vertices[Strip + Mid].X < WithEchoes.Vertices[Mid].X - 30.f);
	TestTrue(TEXT("echo is fainter"), WithEchoes.Colours[Strip + Mid].A < WithEchoes.Colours[Mid].A);
	return true;
}

#endif
