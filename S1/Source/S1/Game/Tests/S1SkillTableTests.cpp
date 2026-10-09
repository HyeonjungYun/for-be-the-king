// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Game/S1SkillTable.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The client's reading of skills.csv + skill_effects.csv (2026-10-10 roster). Inline fixtures — no file I/O.
 *
 * Run headless:
 *   UnrealEditor-Cmd.exe S1.uproject -ExecCmds="Automation RunTests S1.Skill; Quit" -nullrhi -unattended
 */
namespace
{
	constexpr EAutomationTestFlags GS1SkillTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	const TCHAR* GSkillsCsv =
		TEXT("skill_id,name_key,power,kind,cast_ms,cooldown_ms,can_move,telegraph_ms,shape,aim,range_cm,width_cm,radius_cm,angle_deg,allowed_slot,projectile_speed_cms,pierce\r\n")
		TEXT("1003,skill.1003,ad,active,60,6000,0,0,circle_self,direction,400,0,0,0,0,0,0\r\n")
		TEXT("1004,skill.1004,ad,active,300,8000,0,0,cone,direction,300,0,300,90,0,0,0\r\n")
		TEXT("1007,skill.1007,ad,active,300,10000,0,0,single,target,400,0,0,0,0,0,0\r\n")
		TEXT("1009,skill.1009,hybrid,active,300,8000,0,0,projectile,direction,800,80,0,0,0,1600,1\r\n")
		TEXT("1034,skill.1034,ad,passive,0,60000,0,0,none,none,0,0,0,0,0,0,0\r\n")
		TEXT("1038,skill.1038,ap,recast,300,10000,0,0,single,target,300,0,0,0,0,0,0\r\n")
		TEXT("1075,skill.1075,ap,active,60,10000,0,0,circle_self,point,600,0,0,0,0,0,0\r\n");

	const TCHAR* GEffectsCsv =
		TEXT("skill_id,seq,effect,who,requires,ad_ratio,ap_ratio,hits,interval_ms,cc_type,duration_ms,magnitude,dist_cm,speed_cms,ignores_walls,stat,per_level,radius_cm,param\n")
		TEXT("1003,1,move,self,none,0,0,0,0,none,0,0,400,1200,0,none,0,0,dash\n")
		TEXT("1004,1,damage,target,none,1.2,0,1,0,none,0,0,0,0,0,none,0,0,none\n")
		TEXT("1007,1,damage,target,none,2.5,0,1,0,none,0,0,0,0,0,none,0,0,none\n")
		TEXT("1007,2,hard_cc,target,none,0,0,0,0,stun,1000,0,0,0,0,none,0,0,none\n")
		TEXT("1009,1,damage,target,none,1.0,1.0,1,0,none,0,0,0,0,0,none,0,0,none\n")
		TEXT("1034,1,lethal_save,self,none,0,0,0,0,none,60000,0,300,1200,0,none,0,0,none\n")
		TEXT("1038,1,damage,target,none,0,1.5,1,0,none,0,0,0,0,0,none,0,0,none\n")
		TEXT("1038,2,move,self,none,0,0,0,0,none,0,0,200,1000,0,none,0,0,back\n")
		TEXT("1038,3,move,self,recast,0,0,0,0,none,0,0,400,1200,0,none,0,0,dash\n")
		TEXT("1075,1,move,self,none,0,0,0,0,none,0,0,600,0,1,none,0,0,blink\n");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1SkillTableParseTest, "S1.Skill.Table.Parse_ReadsShapeAimAndMovement", GS1SkillTestFlags)
bool FS1SkillTableParseTest::RunTest(const FString& Parameters)
{
	FString Error;
	TestTrue(TEXT("fixture parses"), FS1SkillTable::Parse(GSkillsCsv, GEffectsCsv, Error));
	TestEqual(TEXT("count"), FS1SkillTable::Num(), 7);

	const FS1SkillDef* Dash = FS1SkillTable::Find(1003);
	if (TestNotNull(TEXT("1003"), Dash))
	{
		TestTrue(TEXT("dash moves"), Dash->HasMovement());
		TestEqual(TEXT("dash dist"), Dash->MoveDistCm, 400.f);
		TestEqual(TEXT("dash speed"), Dash->MoveSpeedCms, 1200.f);
		TestEqual(TEXT("dash kind"), Dash->MoveKind, FName(TEXT("dash")));
		TestTrue(TEXT("dash aims"), Dash->NeedsAiming());
		TestFalse(TEXT("dash is not hard cc"), Dash->bHasHardCc);
	}

	const FS1SkillDef* Cone = FS1SkillTable::Find(1004);
	if (TestNotNull(TEXT("1004"), Cone))
	{
		TestEqual(TEXT("cone shape"), static_cast<int32>(Cone->Shape), static_cast<int32>(ES1SkillShape::Cone));
		TestEqual(TEXT("cone angle"), Cone->AngleDeg, 90.f);
		TestEqual(TEXT("cone radius"), Cone->RadiusCm, 300.f);
	}

	const FS1SkillDef* Stun = FS1SkillTable::Find(1007);
	if (TestNotNull(TEXT("1007"), Stun))
	{
		TestTrue(TEXT("stun is hard cc"), Stun->bHasHardCc);
		TestFalse(TEXT("stun does not move"), Stun->HasMovement());
		TestEqual(TEXT("cast"), static_cast<int32>(Stun->CastMs), 300);
		TestEqual(TEXT("cooldown"), static_cast<int32>(Stun->CooldownMs), 10000);
	}

	const FS1SkillDef* Bolt = FS1SkillTable::Find(1009);
	if (TestNotNull(TEXT("1009"), Bolt))
	{
		TestEqual(TEXT("projectile shape"), static_cast<int32>(Bolt->Shape), static_cast<int32>(ES1SkillShape::Projectile));
		TestTrue(TEXT("pierce"), Bolt->bPierce);
		TestEqual(TEXT("speed"), Bolt->ProjectileSpeedCms, 1600.f);
		TestEqual(TEXT("hybrid"), static_cast<int32>(Bolt->Power), static_cast<int32>(ES1SkillPower::Hybrid));
	}

	const FS1SkillDef* Passive = FS1SkillTable::Find(1034);
	if (TestNotNull(TEXT("1034"), Passive))
	{
		TestEqual(TEXT("passive kind"), static_cast<int32>(Passive->Kind), static_cast<int32>(ES1SkillKind::Passive));
		TestFalse(TEXT("passive does not aim"), Passive->NeedsAiming());
		// lethal_save has dist/speed but it is not the key's own dash
		TestFalse(TEXT("passive has no cast movement"), Passive->HasMovement());
	}

	// A recast's first press steps back; the second-press dash is not the key's cast movement
	const FS1SkillDef* Recast = FS1SkillTable::Find(1038);
	if (TestNotNull(TEXT("1038"), Recast))
	{
		TestEqual(TEXT("recast kind"), static_cast<int32>(Recast->Kind), static_cast<int32>(ES1SkillKind::Recast));
		TestEqual(TEXT("first press moves back"), Recast->MoveKind, FName(TEXT("back")));
		TestEqual(TEXT("back dist"), Recast->MoveDistCm, 200.f);
		TestEqual(TEXT("second press dashes"), Recast->RecastMoveKind, FName(TEXT("dash")));
		TestEqual(TEXT("second press dist"), Recast->RecastMoveDistCm, 400.f);
		TestEqual(TEXT("second press speed"), Recast->RecastMoveSpeedCms, 1200.f);
	}

	// Skills without a recast have no second-press movement
	if (Dash != nullptr)
		TestEqual(TEXT("dash has no recast move"), Dash->RecastMoveDistCm, 0.f);

	// Blink: instant (speed 0) through walls
	const FS1SkillDef* Blink = FS1SkillTable::Find(1075);
	if (TestNotNull(TEXT("1075"), Blink))
	{
		TestTrue(TEXT("blink moves"), Blink->HasMovement());
		TestTrue(TEXT("blink ignores walls"), Blink->bMoveIgnoresWalls);
		TestEqual(TEXT("blink speed 0"), Blink->MoveSpeedCms, 0.f);
	}

	TestNull(TEXT("unknown id"), FS1SkillTable::Find(1001));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1SkillTableRejectTest, "S1.Skill.Table.Parse_RejectsBadRows", GS1SkillTestFlags)
bool FS1SkillTableRejectTest::RunTest(const FString& Parameters)
{
	FString Error;

	// Effect for a skill that is not in skills.csv
	TestFalse(TEXT("orphan effect"), FS1SkillTable::Parse(GSkillsCsv,
		TEXT("skill_id,seq,effect,who,requires,ad_ratio,ap_ratio,hits,interval_ms,cc_type,duration_ms,magnitude,dist_cm,speed_cms,ignores_walls,stat,per_level,radius_cm,param\n")
		TEXT("9999,1,damage,target,none,1,0,1,0,none,0,0,0,0,0,none,0,0,none\n"), Error));
	TestTrue(TEXT("names the line"), Error.Contains(TEXT("line 2")));

	// Unknown shape
	TestFalse(TEXT("unknown shape"), FS1SkillTable::Parse(
		TEXT("skill_id,name_key,power,kind,cast_ms,cooldown_ms,can_move,telegraph_ms,shape,aim,range_cm,width_cm,radius_cm,angle_deg,allowed_slot,projectile_speed_cms,pierce\n")
		TEXT("1,skill.1,ad,active,0,0,0,0,hexagon,none,0,0,0,0,0,0,0\n"), GEffectsCsv, Error));

	// Missing column
	TestFalse(TEXT("missing column"), FS1SkillTable::Parse(TEXT("skill_id,power\n1,ad\n"), GEffectsCsv, Error));

	// A failed parse keeps the previous table, so a good one afterwards still works
	TestTrue(TEXT("good parse after bad"), FS1SkillTable::Parse(GSkillsCsv, GEffectsCsv, Error));
	TestEqual(TEXT("count"), FS1SkillTable::Num(), 7);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1SkillTableRulesTest, "S1.Skill.Table.ParseRules_ReadsRecastWindow", GS1SkillTestFlags)
bool FS1SkillTableRulesTest::RunTest(const FString& Parameters)
{
	FString Error;

	TestTrue(TEXT("rules parse"), FS1SkillTable::ParseRules(TEXT("key,value\r\ndot_tick_ms,500\r\nrecast_window_ms,2000\r\n"), Error));
	TestEqual(TEXT("window"), static_cast<int32>(FS1SkillTable::RecastWindowMs()), 2000);

	// Missing key - the client would never open the window, so refuse the file
	TestFalse(TEXT("missing window"), FS1SkillTable::ParseRules(TEXT("key,value\r\ndot_tick_ms,500\r\n"), Error));
	TestTrue(TEXT("names the key"), Error.Contains(TEXT("recast_window_ms")));

	// Not a number
	TestFalse(TEXT("bad number"), FS1SkillTable::ParseRules(TEXT("key,value\r\nrecast_window_ms,soon\r\n"), Error));

	// A failed parse keeps the previous value
	TestEqual(TEXT("window kept"), static_cast<int32>(FS1SkillTable::RecastWindowMs()), 2000);
	return true;
}

#endif
