// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Game/Monster/S1MonsterCatalog.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * design/gdd/monster-ai.md — the client's reading of the server monster table.
 *
 * Run headless:
 *   UnrealEditor-Cmd.exe S1.uproject -ExecCmds="Automation RunTests S1.Monster; Quit" -nullrhi -unattended
 */
namespace
{
	constexpr EAutomationTestFlags GS1MonsterTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	const TCHAR* MonsterTypesHeader = TEXT("monster_type_id,name_key,tier,max_hp,attack_power,attack_speed,attack_range_cm,armor,magic_resist,resistance,move_speed_cms,radius_cm,aggro_radius_cm,leash_radius_cm,search_s,give_up_s,assist_radius_cm,displace_immune,skill_id,drop_tier,drop_chance_pct,drop_rolls\n");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1MonsterCatalogLiveTest, "S1.Monster.Catalog.ServerFile_ThreeTypes_SizesMatchTheGdd", GS1MonsterTestFlags)
bool FS1MonsterCatalogLiveTest::RunTest(const FString& Parameters)
{
	// The same file the server reads — ../Server/Data next to the project
	FS1MonsterCatalog Catalog;
	FString Error;
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../Server/Data")));
	TestTrue(*FString::Printf(TEXT("load %s: %s"), *Directory, *Error), Catalog.LoadFromDirectory(Directory, Error));
	TestEqual(TEXT("types"), Catalog.Num(), 3);

	const FS1MonsterTypeDef* Hound = Catalog.Find(1);
	const FS1MonsterTypeDef* Boss = Catalog.Find(3);
	if (TestNotNull(TEXT("hound"), Hound) && TestNotNull(TEXT("boss"), Boss))
	{
		TestEqual(TEXT("hound name key"), Hound->NameKey, FString(TEXT("monster.hound")));
		TestEqual(TEXT("hound radius"), Hound->RadiusCm, 40.f);
		TestEqual(TEXT("boss tier"), Boss->Tier, FName(TEXT("boss")));
		TestEqual(TEXT("boss radius"), Boss->RadiusCm, 110.f);
		TestEqual(TEXT("boss max hp"), Boss->MaxHp, 3000);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1MonsterCatalogParseTest, "S1.Monster.Catalog.BadRows_KeepNothing", GS1MonsterTestFlags)
bool FS1MonsterCatalogParseTest::RunTest(const FString& Parameters)
{
	const FString Good = TEXT("1,monster.hound,field,120,6,1.0,150,0,0,0,300,40,600,1800,2,6,1000,0,0,open,30,1\n");

	FS1MonsterCatalog Catalog;
	FString Error;
	TestTrue(TEXT("good row"), Catalog.Parse(FString(MonsterTypesHeader) + Good, Error));
	TestEqual(TEXT("one type"), Catalog.Num(), 1);

	TestFalse(TEXT("duplicate id"), Catalog.Parse(FString(MonsterTypesHeader) + Good + Good, Error));
	TestEqual(TEXT("nothing kept after a bad file"), Catalog.Num(), 0);
	TestFalse(TEXT("radius 0"), Catalog.Parse(FString(MonsterTypesHeader) + TEXT("1,monster.hound,field,120,6,1.0,150,0,0,0,300,0,600,1800,2,6,1000,0,0,open,30,1\n"), Error));
	TestFalse(TEXT("short row"), Catalog.Parse(FString(MonsterTypesHeader) + TEXT("1,monster.hound,field\n"), Error));
	TestFalse(TEXT("missing column"), Catalog.Parse(TEXT("monster_type_id,name_key\n1,x\n"), Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1MonsterScaleTest, "S1.Monster.Catalog.ScaleForRadius_PlayerIsOne_Clamped", GS1MonsterTestFlags)
bool FS1MonsterScaleTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("player capsule"), FS1MonsterCatalog::ScaleForRadius(42.f), 1.f);
	TestTrue(TEXT("boss 110 cm ≈ 2.62x"), FMath::IsNearlyEqual(FS1MonsterCatalog::ScaleForRadius(110.f), 110.f / 42.f, 0.001f));
	TestEqual(TEXT("clamped low"), FS1MonsterCatalog::ScaleForRadius(1.f), 0.5f);
	TestEqual(TEXT("clamped high"), FS1MonsterCatalog::ScaleForRadius(1000.f), 4.f);
	return true;
}

#endif
