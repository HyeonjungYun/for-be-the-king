// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Game/Item/S1ItemCatalog.h"
#include "Game/Item/S1ItemLogic.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Item catalog parsing and the detail-pane rules built on it — design/ux/item-ui.md AC-UI-01.
 *
 * The CSVs are written inline, not read from Server/Data, so the tests do no file I/O and do
 * not break when the roster is tuned.
 */
namespace
{
	constexpr EAutomationTestFlags GS1CatalogTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	const TCHAR* GTypesCsv =
		TEXT("id,name_key,equip_slot,archetype,damage_type,grade_mode,attack_range_cm,skill_pool_id\r\n")
		TEXT("101,item.101,1,heavy_weapon,physical,rolled,175,weapon\r\n")
		TEXT("102,item.102,1,light_weapon,physical,rolled,175,weapon\r\n")
		TEXT("\r\n")
		TEXT("301,item.301,3,helmet_scout,none,rolled,0,helmet\r\n")
		TEXT("401,item.401,4,armor_heavy,none,rolled,0,armor\r\n");

	const TCHAR* GStatsCsv =
		TEXT("id,stat,base_stat,flat_floor\n")
		TEXT("101,attack_power,4.7059,0\n")
		TEXT("102,attack_power,1.4706,0\n")
		TEXT("102,crit_chance,0.0735,0\n")
		TEXT("401,max_hp,50,0\n")
		TEXT("401,armor,6,0\n");

	const float GGradeMultipliers[] = { 1.0f, 1.3f, 1.7f, 2.2f };

	FS1ItemView MakeItem(uint32 TypeId, std::initializer_list<uint32> SkillIds)
	{
		FS1ItemView Item;
		Item.InstanceId = 1;
		Item.ItemTypeId = TypeId;
		Item.Grade = ES1ItemGrade::Common;
		for (uint32 SkillId : SkillIds)
			Item.Skills.Add(FS1ItemSkill{ 0, SkillId });
		return Item;
	}

	FS1ItemCatalog MakeCatalog(FAutomationTestBase& Test)
	{
		FS1ItemCatalog Catalog;
		FString Error;
		Test.TestTrue(TEXT("fixture parses"), Catalog.Parse(GTypesCsv, GStatsCsv, Error));
		return Catalog;
	}
}

// ── Grade multipliers (item_grades.csv — shared with the server's ItemTable, 2026-10-07) ──

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1CatalogGradesTest,
	"S1.Item.Catalog.ParseGrades_AllFourGrades_SameFileAsServer", GS1CatalogTestFlags)
bool FS1CatalogGradesTest::RunTest(const FString& Parameters)
{
	FS1ItemCatalog Catalog;
	FString Error;

	TestTrue(TEXT("parses"), Catalog.ParseGrades(TEXT("grade,multiplier\r\n1,1.0\r\n2,1.3\r\n3,1.7\r\n4,2.2\r\n"), Error));
	if (TestEqual(TEXT("four grades"), Catalog.GetGradeMultipliers().Num(), 4))
	{
		TestEqual(TEXT("common"), Catalog.GetGradeMultipliers()[0], 1.0f);
		TestEqual(TEXT("legendary"), Catalog.GetGradeMultipliers()[3], 2.2f);
	}

	// A missing grade would silently make that grade x1.0 — rejected, and nothing is kept
	struct FCase { const TCHAR* What; const TCHAR* Csv; };
	const FCase Cases[] =
	{
		{ TEXT("grade 4 missing"),		TEXT("grade,multiplier\n1,1.0\n2,1.3\n3,1.7\n") },
		{ TEXT("grade twice"),			TEXT("grade,multiplier\n1,1.0\n2,1.3\n2,1.4\n3,1.7\n4,2.2\n") },
		{ TEXT("grade 5"),				TEXT("grade,multiplier\n1,1.0\n2,1.3\n3,1.7\n4,2.2\n5,3.0\n") },
		{ TEXT("multiplier 0"),			TEXT("grade,multiplier\n1,0\n2,1.3\n3,1.7\n4,2.2\n") },
		{ TEXT("missing column"),		TEXT("grade\n1\n2\n3\n4\n") },
	};

	for (const FCase& Case : Cases)
	{
		TestFalse(FString::Printf(TEXT("%s - rejected"), Case.What), Catalog.ParseGrades(Case.Csv, Error));
		TestEqual(FString::Printf(TEXT("%s - nothing kept"), Case.What), Catalog.GetGradeMultipliers().Num(), 0);
	}
	return true;
}

// ── Catalog parsing ──────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1CatalogParseValidTest,
	"S1.Item.Catalog.Parse_ValidFiles_KeepsEveryTypeAndStat", GS1CatalogTestFlags)
bool FS1CatalogParseValidTest::RunTest(const FString& Parameters)
{
	FS1ItemCatalog Catalog;
	FString Error;

	TestTrue(TEXT("parses (CRLF, LF and a blank line mixed)"), Catalog.Parse(GTypesCsv, GStatsCsv, Error));
	TestEqual(TEXT("four types"), Catalog.Num(), 4);

	const FS1ItemTypeDef* Weapon = Catalog.Find(102);
	if (TestNotNull(TEXT("102 exists"), Weapon))
	{
		TestEqual(TEXT("slot"), Weapon->EquipSlot, 1);
		TestTrue(TEXT("is a weapon"), Weapon->IsWeapon());
		TestEqual(TEXT("archetype"), Weapon->Archetype, FName(TEXT("light_weapon")));
		TestEqual(TEXT("name key"), Weapon->NameKey, FString(TEXT("item.102")));
		TestEqual(TEXT("range"), Weapon->AttackRangeCm, 175.f);
		TestEqual(TEXT("two stats"), Weapon->Stats.Num(), 2);
	}

	const FS1ItemTypeDef* Helmet = Catalog.Find(301);
	if (TestNotNull(TEXT("301 exists"), Helmet))
		TestEqual(TEXT("a type with no stat rows has no stats"), Helmet->Stats.Num(), 0);

	TestNull(TEXT("unknown id is null"), Catalog.Find(999));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1CatalogParseInvalidTest,
	"S1.Item.Catalog.Parse_BadRows_FailsAndKeepsNothing", GS1CatalogTestFlags)
bool FS1CatalogParseInvalidTest::RunTest(const FString& Parameters)
{
	const FString Header = TEXT("id,name_key,equip_slot,archetype,damage_type,grade_mode,attack_range_cm,skill_pool_id\n");
	const FString NoStats = TEXT("id,stat,base_stat,flat_floor\n");

	struct FCase { const TCHAR* What; FString Types; FString Stats; };
	const FCase Cases[] =
	{
		{ TEXT("slot 2 is a key, not a part"),	Header + TEXT("101,item.101,2,heavy_weapon,physical,rolled,175,weapon\n"), NoStats },
		{ TEXT("slot 9 does not exist"),		Header + TEXT("101,item.101,9,heavy_weapon,physical,rolled,175,weapon\n"), NoStats },
		{ TEXT("duplicate id"),					Header + TEXT("101,item.101,1,a,physical,rolled,175,weapon\n101,item.101,1,a,physical,rolled,175,weapon\n"), NoStats },
		{ TEXT("id is not a number"),			Header + TEXT("abc,item.101,1,a,physical,rolled,175,weapon\n"), NoStats },
		{ TEXT("empty name key"),				Header + TEXT("101,,1,a,physical,rolled,175,weapon\n"), NoStats },
		{ TEXT("short row"),					Header + TEXT("101,item.101,1\n"), NoStats },
		{ TEXT("missing column"),				FString(TEXT("id,name_key\n101,item.101\n")), NoStats },
		{ TEXT("stat for an unknown id"),		Header + TEXT("101,item.101,1,a,physical,rolled,175,weapon\n"), NoStats + TEXT("999,armor,1,0\n") },
		{ TEXT("stat value is not a number"),	Header + TEXT("101,item.101,1,a,physical,rolled,175,weapon\n"), NoStats + TEXT("101,armor,x,0\n") },
		{ TEXT("same stat twice"),				Header + TEXT("101,item.101,1,a,physical,rolled,175,weapon\n"), NoStats + TEXT("101,armor,1,0\n101,armor,2,0\n") },
	};

	for (const FCase& Case : Cases)
	{
		FS1ItemCatalog Catalog;
		FString Error;

		// Arrange a good catalog first, so "keeps nothing" means the old contents survive.
		Catalog.Parse(GTypesCsv, GStatsCsv, Error);

		const bool bParsed = Catalog.Parse(Case.Types, Case.Stats, Error);
		TestFalse(FString::Printf(TEXT("%s - rejected"), Case.What), bParsed);
		TestFalse(FString::Printf(TEXT("%s - says why"), Case.What), Error.IsEmpty());
		TestEqual(FString::Printf(TEXT("%s - previous contents kept"), Case.What), Catalog.Num(), 4);
	}

	return true;
}

// ── AC-UI-01 — which keys an item uses, and which change on a swap ───────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1KeysForTest,
	"S1.Item.Logic.KeysFor_PartAndOrder_PicksTheKey", GS1CatalogTestFlags)
bool FS1KeysForTest::RunTest(const FString& Parameters)
{
	using S1ItemLogic::KeysFor;
	const FS1ItemCatalog Catalog = MakeCatalog(*this);

	{
		const TArray<FS1KeySkill> Keys = KeysFor(*Catalog.Find(102), MakeItem(102, { 3001, 2001 }));
		if (TestEqual(TEXT("weapon with two skills uses two keys"), Keys.Num(), 2))
		{
			TestEqual(TEXT("first skill -> Shift"), Keys[0].KeySlot, 1);
			TestEqual(TEXT("second skill -> Q"), Keys[1].KeySlot, 2);
			TestEqual(TEXT("Q holds the second skill"), static_cast<int64>(Keys[1].SkillId), static_cast<int64>(2001));
		}
	}
	{
		const TArray<FS1KeySkill> Keys = KeysFor(*Catalog.Find(401), MakeItem(401, { 3001 }));
		if (TestEqual(TEXT("armor uses one key"), Keys.Num(), 1))
			TestEqual(TEXT("armor -> R"), Keys[0].KeySlot, 4);
	}
	{
		TestEqual(TEXT("an item with no skill uses no key"), KeysFor(*Catalog.Find(301), MakeItem(301, {})).Num(), 0);
		TestEqual(TEXT("a zero skill id is not a key"), KeysFor(*Catalog.Find(301), MakeItem(301, { 0 })).Num(), 0);
	}
	{
		// A second skill on a non-weapon has nowhere to go.
		const TArray<FS1KeySkill> Keys = KeysFor(*Catalog.Find(401), MakeItem(401, { 3001, 2001 }));
		TestEqual(TEXT("non-weapon keeps only its own key"), Keys.Num(), 1);
	}
	{
		// Once the server fills SkillInfo.slot, its answer wins.
		FS1ItemView Item = MakeItem(401, {});
		Item.Skills.Add(FS1ItemSkill{ 6, 3001 });
		const TArray<FS1KeySkill> Keys = KeysFor(*Catalog.Find(401), Item);
		if (TestEqual(TEXT("one key"), Keys.Num(), 1))
			TestEqual(TEXT("server slot wins"), Keys[0].KeySlot, 6);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1KeyChangesTest,
	"S1.Item.Logic.KeyChanges_SwapInSamePart_ListsOnlyKeysThatDiffer", GS1CatalogTestFlags)
bool FS1KeyChangesTest::RunTest(const FString& Parameters)
{
	using S1ItemLogic::KeyChanges;
	const FS1ItemCatalog Catalog = MakeCatalog(*this);
	const FS1ItemTypeDef& Weapon = *Catalog.Find(101);
	const FS1ItemTypeDef& Helmet = *Catalog.Find(301);

	{
		// Two-key weapon worn, one-key weapon in hand: Shift changes, Q goes empty.
		const FS1ItemView Worn = MakeItem(102, { 1001, 2001 });
		const FS1ItemView Candidate = MakeItem(101, { 3001 });
		const TArray<FS1KeyChange> Changes = KeyChanges(Weapon, Candidate, &Worn);
		if (TestEqual(TEXT("both weapon keys change"), Changes.Num(), 2))
		{
			TestEqual(TEXT("Shift first"), Changes[0].KeySlot, 1);
			TestEqual(TEXT("Shift becomes 3001"), static_cast<int64>(Changes[0].After), static_cast<int64>(3001));
			TestEqual(TEXT("Q second"), Changes[1].KeySlot, 2);
			TestEqual(TEXT("Q goes empty"), static_cast<int64>(Changes[1].After), static_cast<int64>(0));
			TestEqual(TEXT("Q held 2001"), static_cast<int64>(Changes[1].Before), static_cast<int64>(2001));
		}
	}
	{
		const FS1ItemView Worn = MakeItem(101, { 1001 });
		const FS1ItemView Candidate = MakeItem(102, { 1001 });
		TestEqual(TEXT("same skill on the same key is not a change"), KeyChanges(Weapon, Candidate, &Worn).Num(), 0);
	}
	{
		const TArray<FS1KeyChange> Changes = KeyChanges(Weapon, MakeItem(101, { 1001 }), nullptr);
		if (TestEqual(TEXT("nothing worn: only the filled key changes"), Changes.Num(), 1))
			TestEqual(TEXT("Shift from empty"), static_cast<int64>(Changes[0].Before), static_cast<int64>(0));
	}
	{
		// The MVP helmets roll no skill. Swapping one in empties E.
		const FS1ItemView Worn = MakeItem(301, { 3001 });
		const TArray<FS1KeyChange> Changes = KeyChanges(Helmet, MakeItem(301, {}), &Worn);
		if (TestEqual(TEXT("helmet key changes"), Changes.Num(), 1))
		{
			TestEqual(TEXT("on E"), Changes[0].KeySlot, 3);
			TestEqual(TEXT("goes empty"), static_cast<int64>(Changes[0].After), static_cast<int64>(0));
		}
	}

	return true;
}

// ── Stats ────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1StatValueTest,
	"S1.Item.Logic.StatValue_GradeAndFloor_MatchesGddD2b", GS1CatalogTestFlags)
bool FS1StatValueTest::RunTest(const FString& Parameters)
{
	using S1ItemLogic::StatValue;
	const TArrayView<const float> GM = MakeArrayView(GGradeMultipliers);

	FS1ItemStat Heavy;
	Heavy.Stat = TEXT("attack_power");
	Heavy.BaseStat = 4.7059f;

	// GDD MVP operating point: heavy weapon common +4.71, rare +6.12.
	TestEqual(TEXT("common = base"), StatValue(Heavy, ES1ItemGrade::Common, GM), 4.7059f, 0.0001f);
	TestEqual(TEXT("rare = base x 1.3"), StatValue(Heavy, ES1ItemGrade::Rare, GM), 6.11767f, 0.0001f);
	TestEqual(TEXT("legendary = base x 2.2"), StatValue(Heavy, ES1ItemGrade::Legendary, GM), 10.35298f, 0.0001f);

	FS1ItemStat Floored = Heavy;
	Floored.FlatFloor = 0.1f;
	TestEqual(TEXT("flat floor is added unscaled"), StatValue(Floored, ES1ItemGrade::Rare, GM), 6.21767f, 0.0001f);

	TestEqual(TEXT("missing multiplier counts as x1.0"), StatValue(Heavy, ES1ItemGrade::Rare, TArrayView<const float>()), 4.7059f, 0.0001f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1MoveSpeedBonusTest,
	"S1.Item.Logic.MoveSpeedBonus_PerItemCap_MatchesServerEquipStats", GS1CatalogTestFlags)
bool FS1MoveSpeedBonusTest::RunTest(const FString& Parameters)
{
	using S1ItemLogic::MoveSpeedBonus;
	const TArrayView<const float> GM = MakeArrayView(GGradeMultipliers);

	// Sprint boots (501): percent_ms_bonus 0.0441 — the real Server/Data value
	FS1ItemTypeDef Boots;
	Boots.Id = 501;
	Boots.EquipSlot = 5;
	Boots.Stats.Add(FS1ItemStat{ TEXT("percent_ms_bonus"), 0.0441f, 0.f });
	Boots.Stats.Add(FS1ItemStat{ TEXT("resistance"), 0.5f, 0.f });		// other stats never leak into the speed

	TestEqual(TEXT("common"), MoveSpeedBonus(Boots, ES1ItemGrade::Common, GM), 0.0441f, 0.0001f);
	TestEqual(TEXT("legendary x2.2"), MoveSpeedBonus(Boots, ES1ItemGrade::Legendary, GM), 0.09702f, 0.0001f);

	// The server caps each item at 0.15 (EquipStats::MS_BONUS_PER_ITEM_MAX) — a faster client is a rejected move
	FS1ItemTypeDef Fast = Boots;
	Fast.Stats[0].BaseStat = 0.2f;
	TestEqual(TEXT("capped per item"), MoveSpeedBonus(Fast, ES1ItemGrade::Legendary, GM), S1ItemLogic::MoveSpeedBonusPerItemMax, 0.0001f);

	FS1ItemTypeDef Weapon;
	Weapon.Id = 101;
	Weapon.EquipSlot = 1;
	Weapon.Stats.Add(FS1ItemStat{ TEXT("attack_power"), 4.7059f, 0.f });
	TestEqual(TEXT("no ms stat = 0"), MoveSpeedBonus(Weapon, ES1ItemGrade::Legendary, GM), 0.f, 0.0001f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1StatDeltasTest,
	"S1.Item.Logic.StatDeltas_TwoWeapons_ShowsGainsAndLosses", GS1CatalogTestFlags)
bool FS1StatDeltasTest::RunTest(const FString& Parameters)
{
	using S1ItemLogic::StatDeltas;
	const FS1ItemCatalog Catalog = MakeCatalog(*this);
	const TArrayView<const float> GM = MakeArrayView(GGradeMultipliers);

	// Heavy (common) in hand, light (common) worn: attack up, crit chance lost.
	const TArray<FS1StatDelta> Lines = StatDeltas(*Catalog.Find(101), ES1ItemGrade::Common,
		Catalog.Find(102), ES1ItemGrade::Common, GM);

	if (TestEqual(TEXT("attack + crit chance"), Lines.Num(), 2))
	{
		TestEqual(TEXT("candidate stat first"), Lines[0].Stat, FName(TEXT("attack_power")));
		TestEqual(TEXT("attack before"), Lines[0].Before, 1.4706f, 0.0001f);
		TestEqual(TEXT("attack after"), Lines[0].After, 4.7059f, 0.0001f);
		TestEqual(TEXT("worn-only stat last"), Lines[1].Stat, FName(TEXT("crit_chance")));
		TestEqual(TEXT("crit chance lost"), Lines[1].Delta(), -0.0735f, 0.0001f);
	}

	const TArray<FS1StatDelta> Bare = StatDeltas(*Catalog.Find(401), ES1ItemGrade::Rare, nullptr, ES1ItemGrade::None, GM);
	if (TestEqual(TEXT("nothing worn: every stat is a gain"), Bare.Num(), 2))
		TestEqual(TEXT("rare armor hp"), Bare[0].After, 65.f, 0.0001f);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
