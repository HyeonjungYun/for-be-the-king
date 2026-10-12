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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1AnimWeaponKeyTest, "S1.Anim.WeaponKey_FromItemArchetype", GS1AnimTestFlags)
bool FS1AnimWeaponKeyTest::RunTest(const FString& Parameters)
{
	// 0 = unarmed → any. A real id needs the item subsystem (a game instance); without one it also falls back to any
	TestEqual(TEXT("no weapon"), FS1AnimCatalog::WeaponKeyOf(0), FName(TEXT("any")));
	TestEqual(TEXT("unknown id"), FS1AnimCatalog::WeaponKeyOf(999999), FName(TEXT("any")));
	return true;
}

#endif
