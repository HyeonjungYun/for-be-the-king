// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Game/Anim/S1AnimCatalog.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/ConfigCacheIni.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Animation catalog (design/quick-specs/animation-2026-10-12.md § AC). The clip table must resolve for every weapon and every
 * server motion, and every asset it names must exist in this project (the Fab packs are untracked — if they are missing this
 * fails loudly instead of leaving characters in the template pose).
 */
namespace
{
	constexpr EAutomationTestFlags GS1AnimTestFlags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
	const TCHAR* GWeapons[] = { TEXT("sword"), TEXT("hammer"), TEXT("spear"), TEXT("dagger"), TEXT("bow"), TEXT("knuckle"), TEXT("staff"), TEXT("monster"), TEXT("any") };
	const TCHAR* GCoreKeys[] = { TEXT("idle"), TEXT("run"), TEXT("hit"), TEXT("death"), TEXT("attack"), TEXT("attack2") };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1AnimCatalogLoadTest, "S1.Anim.Catalog_LoadsEveryWeaponAndMotion", GS1AnimTestFlags)
bool FS1AnimCatalogLoadTest::RunTest(const FString& Parameters)
{
	FS1AnimCatalog& Catalog = FS1AnimCatalog::Get();
	TestTrue(TEXT("catalog loaded"), Catalog.IsLoaded());
	TestTrue(TEXT("at least 300 clips (anim_clips.csv)"), Catalog.Num() >= 300);
	TestTrue(TEXT("motions.csv has the vocabulary (>= 20)"), Catalog.Motions().Num() >= 20);

	// AC-AN-01 every weapon has the core clips, idle/run loop, attack strikes inside the clip
	for (const TCHAR* Weapon : GWeapons)
	{
		for (const TCHAR* Key : GCoreKeys)
		{
			const FS1AnimClip* Clip = Catalog.Find(Key, FName(Weapon));
			if (TestNotNull(*FString::Printf(TEXT("%s/%s"), Key, Weapon), Clip) == false)
				continue;
			TestEqual(*FString::Printf(TEXT("%s/%s is weapon-specific or any"), Key, Weapon), Clip->Weapon == FName(Weapon) || Clip->Weapon == FName(TEXT("any")), true);
			if (FCString::Strcmp(Key, TEXT("idle")) == 0 || FCString::Strcmp(Key, TEXT("run")) == 0)
				TestTrue(*FString::Printf(TEXT("%s/%s loops"), Key, Weapon), Clip->bLoop);
			if (FCString::Strcmp(Key, TEXT("attack")) == 0)
				TestTrue(TEXT("attack strikes inside the clip"), Clip->HitPct > 0 && Clip->HitPct < 100);
		}
	}

	// AC-AN-02 every motion resolves for every weapon (m_none → null by design)
	for (const TPair<uint32, FString>& Motion : Catalog.Motions())
	{
		for (const TCHAR* Weapon : GWeapons)
		{
			const FS1AnimClip* Clip = Catalog.FindMotion(Motion.Key, FName(Weapon));
			if (Motion.Value == TEXT("m_none"))
				TestNull(TEXT("m_none has no clip"), Clip);
			else
				TestNotNull(*FString::Printf(TEXT("motion %u %s / %s"), Motion.Key, *Motion.Value, Weapon), Clip);
		}
	}
	TestNull(TEXT("unknown motion id → no clip"), Catalog.FindMotion(9999u, FName(TEXT("sword"))));

	// AC-AN-03 every asset the table names exists on disk (the packs)
	int32 Missing = 0;
	for (const TPair<FString, FS1AnimClip>& Pair : Catalog.All())
	{
		const FString Package = Pair.Value.Asset.GetLongPackageName();
		if (FPackageName::DoesPackageExist(Package) == false)
		{
			Missing++;
			if (Missing <= 5)
				AddError(FString::Printf(TEXT("missing asset %s for %s"), *Package, *Pair.Key));
		}
	}
	TestEqual(TEXT("missing assets"), Missing, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1AnimCatalogParseTest, "S1.Anim.Catalog_RejectsBadRowsAndFallsBack", GS1AnimTestFlags)
bool FS1AnimCatalogParseTest::RunTest(const FString& Parameters)
{
	FS1AnimCatalog Catalog;
	FString Error;
	const FString Header = TEXT("key,weapon,asset,rate,loop,hit_pct\n");

	TestTrue(TEXT("good rows"), Catalog.ParseClips(Header + TEXT("attack,any,/Game/A/B,1,0,40\nattack,sword,/Game/A/C,1,0,50\nidle,sword,/Game/A/D,1,1,0\n"), Error));
	TestEqual(TEXT("3 clips"), Catalog.Num(), 3);
	// AC-AN-04 fallback chain: (key, weapon) → (key, any) → (attack, weapon) → (attack, any)
	TestEqual(TEXT("exact"), Catalog.Find(TEXT("attack"), FName(TEXT("sword")))->Asset.ToString(), FString(TEXT("/Game/A/C")));
	TestEqual(TEXT("any weapon"), Catalog.Find(TEXT("attack"), FName(TEXT("bow")))->Asset.ToString(), FString(TEXT("/Game/A/B")));
	TestEqual(TEXT("missing key → the weapon's attack"), Catalog.Find(TEXT("m_dash"), FName(TEXT("sword")))->Asset.ToString(), FString(TEXT("/Game/A/C")));
	TestEqual(TEXT("missing key, other weapon → attack/any"), Catalog.Find(TEXT("m_dash"), FName(TEXT("bow")))->Asset.ToString(), FString(TEXT("/Game/A/B")));
	TestEqual(TEXT("idle/bow → no idle at all → attack/any"), Catalog.Find(TEXT("idle"), FName(TEXT("bow")))->Asset.ToString(), FString(TEXT("/Game/A/B")));

	TestFalse(TEXT("duplicate row rejected"), Catalog.ParseClips(Header + TEXT("attack,any,/Game/A/B,1,0,40\nattack,any,/Game/A/C,1,0,40\n"), Error));
	TestFalse(TEXT("hit_pct 101 rejected"), Catalog.ParseClips(Header + TEXT("attack,any,/Game/A/B,1,0,101\n"), Error));
	TestTrue(TEXT("hit_pct 100 accepted (boundary)"), Catalog.ParseClips(Header + TEXT("attack,any,/Game/A/B,1,0,100\n"), Error));
	TestTrue(TEXT("hit_pct 0 accepted (boundary)"), Catalog.ParseClips(Header + TEXT("attack,any,/Game/A/B,1,0,0\n"), Error));
	TestFalse(TEXT("hit_pct -1 rejected"), Catalog.ParseClips(Header + TEXT("attack,any,/Game/A/B,1,0,-1\n"), Error));
	TestFalse(TEXT("rate 0 rejected"), Catalog.ParseClips(Header + TEXT("attack,any,/Game/A/B,0,0,40\n"), Error));
	TestFalse(TEXT("missing column rejected"), Catalog.ParseClips(TEXT("key,weapon,asset\nattack,any,/Game/A/B\n"), Error));

	TestTrue(TEXT("motions"), Catalog.ParseMotions(TEXT("motion_id,key\n1,m_none\n2,m_attack\n"), Error));
	TestEqual(TEXT("2 motions"), Catalog.Motions().Num(), 2);
	TestFalse(TEXT("duplicate motion id rejected"), Catalog.ParseMotions(TEXT("motion_id,key\n1,m_none\n1,m_attack\n"), Error));
	TestFalse(TEXT("motion id 0 rejected"), Catalog.ParseMotions(TEXT("motion_id,key\n0,m_none\n"), Error));
	return true;
}

// 2026-10-15 8-direction locomotion: speed_cms column (optional), exact lookup without the attack fallback
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1AnimLocoTableTest, "S1.Anim.Loco.SpeedColumnAndExactLookup", GS1AnimTestFlags)
bool FS1AnimLocoTableTest::RunTest(const FString& Parameters)
{
	// Arrange
	FS1AnimCatalog Catalog;
	FString Error;
	const FString Header = TEXT("key,weapon,asset,rate,loop,hit_pct,speed_cms\n");

	// Act / Assert - the column is read, empty = 0, negative rejected, the old 6-column header still loads
	TestTrue(TEXT("rows with speed"), Catalog.ParseClips(Header + TEXT("attack,sword,/Game/A/A,1,0,40,\nrun_f,sword,/Game/A/F,1,1,40,522.5\n"), Error));
	TestEqual(TEXT("speed read"), Catalog.FindExact(TEXT("run_f"), FName(TEXT("sword")))->SpeedCms, 522.5f);
	TestEqual(TEXT("empty speed = 0"), Catalog.FindExact(TEXT("attack"), FName(TEXT("sword")))->SpeedCms, 0.f);
	TestNull(TEXT("exact lookup has no attack fallback (bow has no run_f)"), Catalog.FindExact(TEXT("run_f"), FName(TEXT("bow"))));
	TestNull(TEXT("exact lookup: unknown key on sword"), Catalog.FindExact(TEXT("run_fl"), FName(TEXT("sword"))));
	TestNotNull(TEXT("Find still falls back to attack"), Catalog.Find(TEXT("run_fl"), FName(TEXT("sword"))));
	TestFalse(TEXT("negative speed rejected"), Catalog.ParseClips(Header + TEXT("run_f,sword,/Game/A/F,1,1,40,-1\n"), Error));
	TestTrue(TEXT("6-column table still loads"), Catalog.ParseClips(TEXT("key,weapon,asset,rate,loop,hit_pct\nidle,any,/Game/A/I,1,1,0\n"), Error));
	return true;
}

// 2026-10-15 per-skill clips: start_pct (optional) skips a windup - it may not pass the strike
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1AnimStartPctTest, "S1.Anim.Catalog_StartPctColumn", GS1AnimTestFlags)
bool FS1AnimStartPctTest::RunTest(const FString& Parameters)
{
	// Arrange
	FS1AnimCatalog Catalog;
	FString Error;
	const FString Header = TEXT("key,weapon,asset,rate,loop,hit_pct,speed_cms,start_pct\n");

	// Act / Assert
	TestTrue(TEXT("rows with start"), Catalog.ParseClips(Header + TEXT("m_sk_1,sword,/Game/A/A,3,0,36,0,20\nattack,sword,/Game/A/B,1,0,40,0,\n"), Error));
	TestEqual(TEXT("start read"), Catalog.FindExact(TEXT("m_sk_1"), FName(TEXT("sword")))->StartPct, 20);
	TestEqual(TEXT("empty start = 0"), Catalog.FindExact(TEXT("attack"), FName(TEXT("sword")))->StartPct, 0);
	TestTrue(TEXT("start = hit accepted (boundary)"), Catalog.ParseClips(Header + TEXT("m_sk_1,sword,/Game/A/A,1,0,36,0,36\n"), Error));
	TestFalse(TEXT("start past the strike rejected"), Catalog.ParseClips(Header + TEXT("m_sk_1,sword,/Game/A/A,1,0,36,0,37\n"), Error));
	TestFalse(TEXT("negative start rejected"), Catalog.ParseClips(Header + TEXT("m_sk_1,sword,/Game/A/A,1,0,36,0,-1\n"), Error));
	return true;
}

// The shipped table: every player weapon has the full 16-clip set - a missing key would silently drop that weapon back to the
// old forward-only run
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1AnimLocoSetTest, "S1.Anim.Loco.EveryWeaponHasTheEightDirectionSet", GS1AnimTestFlags)
bool FS1AnimLocoSetTest::RunTest(const FString& Parameters)
{
	// Arrange
	FS1AnimCatalog& Catalog = FS1AnimCatalog::Get();
	const TCHAR* Weapons[] = { TEXT("sword"), TEXT("hammer"), TEXT("spear"), TEXT("dagger"), TEXT("bow"), TEXT("knuckle"), TEXT("staff") };
	const TCHAR* Gaits[] = { TEXT("walk_"), TEXT("run_") };

	// Act / Assert
	for (const TCHAR* Weapon : Weapons)
	{
		for (const TCHAR* Gait : Gaits)
		{
			for (int32 Sector = 0; Sector < 8; Sector++)
			{
				const FString Key = FString(Gait) + S1LocoLogic::SectorSuffix(Sector);
				const FS1AnimClip* Clip = Catalog.FindExact(Key, FName(Weapon));
				if (TestNotNull(*FString::Printf(TEXT("%s/%s"), *Key, Weapon), Clip) == false)
					continue;
				TestTrue(*FString::Printf(TEXT("%s/%s loops"), *Key, Weapon), Clip->bLoop);
				TestTrue(*FString::Printf(TEXT("%s/%s has a measured speed"), *Key, Weapon), Clip->SpeedCms > 0.f);
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1AnimLocoSectorTest, "S1.Anim.Loco.DirectionSectorGaitAndPhase", GS1AnimTestFlags)
bool FS1AnimLocoSectorTest::RunTest(const FString& Parameters)
{
	using namespace S1LocoLogic;
	// Sector centres: 0 F · 45 FR · 90 R · 135 BR · ±180 B · -135 BL · -90 L · -45 FL (UE yaw: + = to the right)
	TestEqual(TEXT("0 → F"), DirectionSector(0.f, -1, 10.f), 0);
	TestEqual(TEXT("45 → FR"), DirectionSector(45.f, -1, 10.f), 1);
	TestEqual(TEXT("90 → R"), DirectionSector(90.f, -1, 10.f), 2);
	TestEqual(TEXT("135 → BR"), DirectionSector(135.f, -1, 10.f), 3);
	TestEqual(TEXT("180 → B"), DirectionSector(180.f, -1, 10.f), 4);
	TestEqual(TEXT("-180 → B"), DirectionSector(-180.f, -1, 10.f), 4);
	TestEqual(TEXT("-135 → BL"), DirectionSector(-135.f, -1, 10.f), 5);
	TestEqual(TEXT("-90 → L"), DirectionSector(-90.f, -1, 10.f), 6);
	TestEqual(TEXT("-45 → FL"), DirectionSector(-45.f, -1, 10.f), 7);
	TestEqual(TEXT("wraps: 315 = -45 → FL"), DirectionSector(315.f, -1, 10.f), 7);
	// Hysteresis: F is kept up to 22.5 + 10 = 32.5°, then FR
	TestEqual(TEXT("30° stays F"), DirectionSector(30.f, 0, 10.f), 0);
	TestEqual(TEXT("33° leaves F → FR"), DirectionSector(33.f, 0, 10.f), 1);
	TestEqual(TEXT("without a previous sector 30° is FR"), DirectionSector(30.f, -1, 10.f), 1);
	TestEqual(TEXT("B kept across the ±180 seam"), DirectionSector(-170.f, 4, 10.f), 4);
	TestEqual(TEXT("suffix 0"), FString(SectorSuffix(0)), FString(TEXT("f")));
	TestEqual(TEXT("suffix 7"), FString(SectorSuffix(7)), FString(TEXT("fl")));
	TestEqual(TEXT("suffix wraps -1 → fl"), FString(SectorSuffix(-1)), FString(TEXT("fl")));

	// Gait hysteresis: enter run at 260, leave below 230
	TestFalse(TEXT("walker at 259 walks"), IsRunGait(259.f, false, 260.f, 230.f));
	TestTrue(TEXT("walker at 260 runs"), IsRunGait(260.f, false, 260.f, 230.f));
	TestTrue(TEXT("runner at 230 keeps running"), IsRunGait(230.f, true, 260.f, 230.f));
	TestFalse(TEXT("runner at 229 walks"), IsRunGait(229.f, true, 260.f, 230.f));

	// Step phase carries over a looping montage position
	TestEqual(TEXT("half way"), LoopPhase(0.4f, 0.8f), 0.5f);
	TestTrue(TEXT("after 3 loops + quarter"), FMath::IsNearlyEqual(LoopPhase(2.6f, 0.8f), 0.25f, 1e-4f));
	TestEqual(TEXT("zero length → 0"), LoopPhase(1.f, 0.f), 0.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1AnimWeaponKeyTest, "S1.Anim.WeaponKey_FromItemArchetype", GS1AnimTestFlags)
bool FS1AnimWeaponKeyTest::RunTest(const FString& Parameters)
{
	// 0 = unarmed → any. A real id needs the item subsystem (a game instance); without one it also falls back to any
	TestEqual(TEXT("no weapon"), FS1AnimCatalog::WeaponKeyOf(0), FName(TEXT("any")));
	TestEqual(TEXT("unknown id"), FS1AnimCatalog::WeaponKeyOf(999999), FName(TEXT("any")));
	return true;
}

#endif
