// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/S1SkillTable.h"
#include "Misc/DefaultValueHelper.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	/** A CSV split into cells. No quoting — the skill files never need commas inside a value (names live in the String Table). */
	struct FSkillCsv
	{
		TArray<FString>			Header;
		TArray<TArray<FString>>	Rows;
		TArray<int32>			LineNumbers;

		int32 Column(const TCHAR* Name) const { return Header.IndexOfByKey(FString(Name)); }
	};

	FSkillCsv ReadSkillCsv(const FString& Text)
	{
		FSkillCsv Out;

		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, /*InCullEmpty*/ false);

		for (int32 i = 0; i < Lines.Num(); i++)
		{
			if (Lines[i].TrimStartAndEnd().IsEmpty())
				continue;

			TArray<FString> Cells;
			Lines[i].ParseIntoArray(Cells, TEXT(","), /*InCullEmpty*/ false);
			for (FString& Cell : Cells)
				Cell.TrimStartAndEndInline();

			if (Out.Header.Num() == 0)
			{
				Out.Header = MoveTemp(Cells);
				continue;
			}

			Out.Rows.Add(MoveTemp(Cells));
			Out.LineNumbers.Add(i + 1);
		}

		return Out;
	}

	bool ParseKind(const FString& S, ES1SkillKind& Out)
	{
		if (S == TEXT("active"))		{ Out = ES1SkillKind::Active; return true; }
		if (S == TEXT("passive"))		{ Out = ES1SkillKind::Passive; return true; }
		if (S == TEXT("hold"))			{ Out = ES1SkillKind::Hold; return true; }
		if (S == TEXT("recast"))		{ Out = ES1SkillKind::Recast; return true; }
		return false;
	}

	bool ParseShape(const FString& S, ES1SkillShape& Out)
	{
		if (S == TEXT("none"))			{ Out = ES1SkillShape::None; return true; }
		if (S == TEXT("single"))		{ Out = ES1SkillShape::Single; return true; }
		if (S == TEXT("line"))			{ Out = ES1SkillShape::Line; return true; }
		if (S == TEXT("cone"))			{ Out = ES1SkillShape::Cone; return true; }
		if (S == TEXT("circle_self"))	{ Out = ES1SkillShape::CircleSelf; return true; }
		if (S == TEXT("circle_point"))	{ Out = ES1SkillShape::CirclePoint; return true; }
		if (S == TEXT("projectile"))	{ Out = ES1SkillShape::Projectile; return true; }
		return false;
	}

	bool ParseAim(const FString& S, ES1SkillAimType& Out)
	{
		if (S == TEXT("none"))			{ Out = ES1SkillAimType::None; return true; }
		if (S == TEXT("self_area"))		{ Out = ES1SkillAimType::SelfArea; return true; }
		if (S == TEXT("target"))		{ Out = ES1SkillAimType::Target; return true; }
		if (S == TEXT("direction"))		{ Out = ES1SkillAimType::Direction; return true; }
		if (S == TEXT("point"))			{ Out = ES1SkillAimType::Point; return true; }
		return false;
	}

	bool ParsePower(const FString& S, ES1SkillPower& Out)
	{
		if (S == TEXT("none"))			{ Out = ES1SkillPower::None; return true; }
		if (S == TEXT("ad"))			{ Out = ES1SkillPower::Ad; return true; }
		if (S == TEXT("ap"))			{ Out = ES1SkillPower::Ap; return true; }
		if (S == TEXT("hybrid"))		{ Out = ES1SkillPower::Hybrid; return true; }
		return false;
	}

	// The effect kinds that make a skill "hard CC" — the same list as the server's IsHardCcEffect
	bool IsHardCc(const FString& Effect, const FString& CcType)
	{
		if (Effect == TEXT("knockback") || Effect == TEXT("pull") || Effect == TEXT("petrify_window") || Effect == TEXT("throw"))
			return true;
		if (Effect == TEXT("hard_cc"))
			return CcType == TEXT("stun") || CcType == TEXT("root") || CcType == TEXT("launch") || CcType == TEXT("knockback");
		if (Effect == TEXT("hold"))
			return CcType == TEXT("root");
		return false;
	}
}

TMap<uint32, FS1SkillDef>& FS1SkillTable::Defs()
{
	// Static data with no owner — no dependency on GameInstance startup order
	static TMap<uint32, FS1SkillDef> Table;
	return Table;
}

uint32& FS1SkillTable::RecastWindow()
{
	static uint32 WindowMs = 0;
	return WindowMs;
}

uint32 FS1SkillTable::RecastWindowMs()
{
	return RecastWindow();
}

bool FS1SkillTable::LoadFromDirectory(const FString& Directory, FString& OutError)
{
	FString SkillsCsv;
	FString EffectsCsv;
	FString RulesCsv;

	const FString SkillsPath = FPaths::Combine(Directory, TEXT("skills.csv"));
	const FString EffectsPath = FPaths::Combine(Directory, TEXT("skill_effects.csv"));
	const FString RulesPath = FPaths::Combine(Directory, TEXT("skill_rules.csv"));

	if (FFileHelper::LoadFileToString(SkillsCsv, *SkillsPath) == false)
	{
		OutError = FString::Printf(TEXT("cannot read %s"), *SkillsPath);
		return false;
	}
	if (FFileHelper::LoadFileToString(EffectsCsv, *EffectsPath) == false)
	{
		OutError = FString::Printf(TEXT("cannot read %s"), *EffectsPath);
		return false;
	}
	if (FFileHelper::LoadFileToString(RulesCsv, *RulesPath) == false)
	{
		OutError = FString::Printf(TEXT("cannot read %s"), *RulesPath);
		return false;
	}

	return Parse(SkillsCsv, EffectsCsv, OutError) && ParseRules(RulesCsv, OutError);
}

bool FS1SkillTable::ParseRules(const FString& RulesCsv, FString& OutError)
{
	const FSkillCsv Csv = ReadSkillCsv(RulesCsv);
	const int32 ColKey = Csv.Column(TEXT("key"));
	const int32 ColValue = Csv.Column(TEXT("value"));

	if (ColKey == INDEX_NONE || ColValue == INDEX_NONE)
	{
		OutError = TEXT("skill_rules.csv: header needs key,value");
		return false;
	}

	int32 WindowMs = -1;
	for (int32 r = 0; r < Csv.Rows.Num(); r++)
	{
		const TArray<FString>& Cells = Csv.Rows[r];
		if (Cells.Num() < 2 || Cells[ColKey] != TEXT("recast_window_ms"))
			continue;

		if (FDefaultValueHelper::ParseInt(Cells[ColValue], WindowMs) == false || WindowMs < 0)
		{
			OutError = FString::Printf(TEXT("skill_rules.csv line %d: recast_window_ms is not a number >= 0"), Csv.LineNumbers[r]);
			return false;
		}
	}

	if (WindowMs < 0)
	{
		OutError = TEXT("skill_rules.csv: missing recast_window_ms");
		return false;
	}

	RecastWindow() = static_cast<uint32>(WindowMs);
	return true;
}

bool FS1SkillTable::Parse(const FString& SkillsCsv, const FString& EffectsCsv, FString& OutError)
{
	TMap<uint32, FS1SkillDef> Parsed;

	// ── skills.csv ──
	{
		const FSkillCsv Csv = ReadSkillCsv(SkillsCsv);

		const int32 ColId		= Csv.Column(TEXT("skill_id"));
		const int32 ColPower	= Csv.Column(TEXT("power"));
		const int32 ColKind		= Csv.Column(TEXT("kind"));
		const int32 ColCast		= Csv.Column(TEXT("cast_ms"));
		const int32 ColCooldown	= Csv.Column(TEXT("cooldown_ms"));
		const int32 ColCanMove	= Csv.Column(TEXT("can_move"));
		const int32 ColShape	= Csv.Column(TEXT("shape"));
		const int32 ColAim		= Csv.Column(TEXT("aim"));
		const int32 ColRange	= Csv.Column(TEXT("range_cm"));
		const int32 ColWidth	= Csv.Column(TEXT("width_cm"));
		const int32 ColRadius	= Csv.Column(TEXT("radius_cm"));
		const int32 ColAngle	= Csv.Column(TEXT("angle_deg"));
		const int32 ColProjSpd	= Csv.Column(TEXT("projectile_speed_cms"));
		const int32 ColPierce	= Csv.Column(TEXT("pierce"));

		for (int32 Col : { ColId, ColPower, ColKind, ColCast, ColCooldown, ColCanMove, ColShape, ColAim, ColRange, ColWidth, ColRadius, ColAngle, ColProjSpd, ColPierce })
		{
			if (Col == INDEX_NONE)
			{
				OutError = TEXT("skills.csv: header is missing a required column");
				return false;
			}
		}

		for (int32 r = 0; r < Csv.Rows.Num(); r++)
		{
			const TArray<FString>& Cells = Csv.Rows[r];
			const int32 Line = Csv.LineNumbers[r];

			if (Cells.Num() < Csv.Header.Num())
			{
				OutError = FString::Printf(TEXT("skills.csv line %d: %d cells, header has %d"), Line, Cells.Num(), Csv.Header.Num());
				return false;
			}

			int32 Id = 0, Cast = 0, Cooldown = 0, CanMove = 0, Pierce = 0;
			FS1SkillDef Def;

			if (FDefaultValueHelper::ParseInt(Cells[ColId], Id) == false || Id <= 0)
			{
				OutError = FString::Printf(TEXT("skills.csv line %d: skill_id '%s' is not a positive integer"), Line, *Cells[ColId]);
				return false;
			}
			if (Parsed.Contains(static_cast<uint32>(Id)))
			{
				OutError = FString::Printf(TEXT("skills.csv line %d: skill_id %d appears twice"), Line, Id);
				return false;
			}
			if (ParsePower(Cells[ColPower], Def.Power) == false || ParseKind(Cells[ColKind], Def.Kind) == false
				|| ParseShape(Cells[ColShape], Def.Shape) == false || ParseAim(Cells[ColAim], Def.AimType) == false)
			{
				OutError = FString::Printf(TEXT("skills.csv line %d: unknown power / kind / shape / aim"), Line);
				return false;
			}
			if (FDefaultValueHelper::ParseInt(Cells[ColCast], Cast) == false || FDefaultValueHelper::ParseInt(Cells[ColCooldown], Cooldown) == false
				|| FDefaultValueHelper::ParseInt(Cells[ColCanMove], CanMove) == false || FDefaultValueHelper::ParseInt(Cells[ColPierce], Pierce) == false
				|| FDefaultValueHelper::ParseFloat(Cells[ColRange], Def.RangeCm) == false || FDefaultValueHelper::ParseFloat(Cells[ColWidth], Def.WidthCm) == false
				|| FDefaultValueHelper::ParseFloat(Cells[ColRadius], Def.RadiusCm) == false || FDefaultValueHelper::ParseFloat(Cells[ColAngle], Def.AngleDeg) == false
				|| FDefaultValueHelper::ParseFloat(Cells[ColProjSpd], Def.ProjectileSpeedCms) == false
				|| Cast < 0 || Cooldown < 0 || Def.RangeCm < 0.f || Def.WidthCm < 0.f || Def.RadiusCm < 0.f || Def.AngleDeg < 0.f)
			{
				OutError = FString::Printf(TEXT("skills.csv line %d: a numeric column is not a number >= 0"), Line);
				return false;
			}

			Def.SkillId = static_cast<uint32>(Id);
			Def.EffectId = Def.SkillId;
			Def.CastMs = static_cast<uint32>(Cast);
			Def.CooldownMs = static_cast<uint32>(Cooldown);
			Def.bCanMoveWhileCasting = (CanMove != 0);
			Def.bPierce = (Pierce != 0);

			Parsed.Add(Def.SkillId, MoveTemp(Def));
		}
	}

	if (Parsed.Num() == 0)
	{
		OutError = TEXT("skills.csv has no rows");
		return false;
	}

	// ── skill_effects.csv — only the movement the client runs itself, and whether a key is hard CC ──
	{
		const FSkillCsv Csv = ReadSkillCsv(EffectsCsv);

		const int32 ColId		= Csv.Column(TEXT("skill_id"));
		const int32 ColEffect	= Csv.Column(TEXT("effect"));
		const int32 ColWho		= Csv.Column(TEXT("who"));
		const int32 ColRequires	= Csv.Column(TEXT("requires"));
		const int32 ColCc		= Csv.Column(TEXT("cc_type"));
		const int32 ColDist		= Csv.Column(TEXT("dist_cm"));
		const int32 ColSpeed	= Csv.Column(TEXT("speed_cms"));
		const int32 ColWalls	= Csv.Column(TEXT("ignores_walls"));
		const int32 ColParam	= Csv.Column(TEXT("param"));

		for (int32 Col : { ColId, ColEffect, ColWho, ColRequires, ColCc, ColDist, ColSpeed, ColWalls, ColParam })
		{
			if (Col == INDEX_NONE)
			{
				OutError = TEXT("skill_effects.csv: header is missing a required column");
				return false;
			}
		}

		for (int32 r = 0; r < Csv.Rows.Num(); r++)
		{
			const TArray<FString>& Cells = Csv.Rows[r];
			const int32 Line = Csv.LineNumbers[r];

			if (Cells.Num() < Csv.Header.Num())
			{
				OutError = FString::Printf(TEXT("skill_effects.csv line %d: %d cells, header has %d"), Line, Cells.Num(), Csv.Header.Num());
				return false;
			}

			int32 Id = 0;
			if (FDefaultValueHelper::ParseInt(Cells[ColId], Id) == false || Id <= 0)
			{
				OutError = FString::Printf(TEXT("skill_effects.csv line %d: skill_id '%s' is not a positive integer"), Line, *Cells[ColId]);
				return false;
			}

			FS1SkillDef* Def = Parsed.Find(static_cast<uint32>(Id));
			if (Def == nullptr)
			{
				OutError = FString::Printf(TEXT("skill_effects.csv line %d: skill %d is not in skills.csv"), Line, Id);
				return false;
			}

			const FString& Effect = Cells[ColEffect];
			if (IsHardCc(Effect, Cells[ColCc]))
				Def->bHasHardCc = true;

			// The first movement that runs on the first press. A recast's second dash is not the key's cast.
			if (Effect == TEXT("move") && Cells[ColWho] == TEXT("self") && Cells[ColRequires] == TEXT("none") && Def->MoveDistCm <= 0.f)
			{
				int32 Walls = 0;
				if (FDefaultValueHelper::ParseFloat(Cells[ColDist], Def->MoveDistCm) == false
					|| FDefaultValueHelper::ParseFloat(Cells[ColSpeed], Def->MoveSpeedCms) == false
					|| FDefaultValueHelper::ParseInt(Cells[ColWalls], Walls) == false)
				{
					OutError = FString::Printf(TEXT("skill_effects.csv line %d: move dist_cm / speed_cms / ignores_walls are not numbers"), Line);
					return false;
				}
				Def->bMoveIgnoresWalls = (Walls != 0);
				Def->MoveKind = FName(*Cells[ColParam]);
			}

			// The recast's second press - the client runs that dash too, toward the target the first press locked
			if (Effect == TEXT("move") && Cells[ColWho] == TEXT("self") && Cells[ColRequires] == TEXT("recast") && Def->RecastMoveDistCm <= 0.f)
			{
				if (FDefaultValueHelper::ParseFloat(Cells[ColDist], Def->RecastMoveDistCm) == false
					|| FDefaultValueHelper::ParseFloat(Cells[ColSpeed], Def->RecastMoveSpeedCms) == false)
				{
					OutError = FString::Printf(TEXT("skill_effects.csv line %d: recast move dist_cm / speed_cms are not numbers"), Line);
					return false;
				}
				Def->RecastMoveKind = FName(*Cells[ColParam]);
			}
		}
	}

	Defs() = MoveTemp(Parsed);
	return true;
}

const FS1SkillDef* FS1SkillTable::Find(uint32 SkillId)
{
	return Defs().Find(SkillId);
}

int32 FS1SkillTable::Num()
{
	return Defs().Num();
}
