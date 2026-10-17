#include "pch.h"
#include "LonePath.h"
#include "CsvTable.h"

LonePathRules& LonePathRules::Get()
{
	static LonePathRules s_rules;
	return s_rules;
}

bool LonePathRules::Parse(const string& csv, OUT string& error)
{
	CsvTable table;
	vector<int32> c;
	if (Csv::Parse(csv, "lone_path.csv", OUT table, OUT error) == false
		|| Csv::FindColumns(table, "lone_path.csv", { "key", "value" }, OUT c, OUT error) == false)
		return false;

	for (size_t r = 0; r < table.rows.size(); r++)
	{
		const string& key = table.rows[r][c[0]];
		const string& value = table.rows[r][c[1]];
		const string where = "lone_path.csv:" + to_string(table.lineNumbers[r]) + " " + key;

		uint32 u = 0;
		float f = 0.f;
		const bool isUInt = Csv::ParseUInt(value, OUT u);
		const bool isFloat = Csv::ParseFloat(value, OUT f);

		auto uintIn = [&](uint32 lo, uint32 hi) { if (isUInt == false || u < lo || u > hi) { error = where + " must be an integer " + to_string(lo) + ".." + to_string(hi); return false; } return true; };
		auto floatIn = [&](float lo, float hi) { if (isFloat == false || f < lo || f > hi) { error = where + " must be a number " + to_string(lo) + ".." + to_string(hi); return false; } return true; };

		if (key == "roll_pct") { if (uintIn(0, 100) == false) return false; rollPct = u; }
		else if (key == "damage_reduce_per_piece") { if (floatIn(0.f, 0.25f) == false) return false; damageReducePerPiece = f; }
		else if (key == "cc_reduce_per_piece") { if (floatIn(0.f, 0.25f) == false) return false; ccReducePerPiece = f; }
		else if (key == "attack_per_piece") { if (floatIn(0.f, 0.25f) == false) return false; attackPerPiece = f; }
		else if (key == "max_pieces") { if (uintIn(1, 6) == false) return false; maxPieces = static_cast<int32>(u); }
		else if (key == "outnumber_radius_cm") { if (floatIn(100.f, 3000.f) == false) return false; outnumberRadiusCm = f; }
		else if (key == "outnumber_enemies") { if (uintIn(2, 10) == false) return false; outnumberEnemies = static_cast<int32>(u); }
		else
		{
			error = where + " is not a known key";
			return false;
		}
	}

	// 상한 - 착용 상한까지 쌓아도 완전 면역은 없다 (기획서: CC 저항 상한 40 % · 다구대일의 우위는 남는다)
	if (damageReducePerPiece * maxPieces > 0.5f || ccReducePerPiece * maxPieces > 0.4f)
	{
		error = "lone_path.csv: damage_reduce_per_piece x max_pieces must be <= 0.5 and cc_reduce_per_piece x max_pieces <= 0.4";
		return false;
	}
	return true;
}

bool LonePathRules::Load(const string& dataDir, OUT string& error)
{
	*this = LonePathRules();

	string text;
	if (Csv::ReadFile(dataDir + "\\lone_path.csv", OUT text) && Parse(text, OUT error) == false)
		return false;
	return true;
}

int32 LonePath::EffectivePieces(int32 worn, const LonePathRules& rules)
{
	return clamp(worn, 0, rules.maxPieces);
}

bool LonePath::Outnumbered(int32 nearbyEnemies, const LonePathRules& rules)
{
	return nearbyEnemies >= rules.outnumberEnemies;
}

float LonePath::DamageTakenMult(int32 worn, int32 nearbyEnemies, const LonePathRules& rules)
{
	if (Outnumbered(nearbyEnemies, rules) == false)
		return 1.f;
	return 1.f - rules.damageReducePerPiece * EffectivePieces(worn, rules);
}

float LonePath::CcDurationMult(int32 worn, int32 nearbyEnemies, const LonePathRules& rules)
{
	if (Outnumbered(nearbyEnemies, rules) == false)
		return 1.f;
	return 1.f - rules.ccReducePerPiece * EffectivePieces(worn, rules);
}

float LonePath::AttackBonus(int32 worn, const LonePathRules& rules)
{
	return rules.attackPerPiece * EffectivePieces(worn, rules);
}

bool LonePath::Rolls(uint32 roll, const LonePathRules& rules)
{
	return roll < rules.rollPct;
}

uint32 LonePath::RollOf(uint64 instanceId)
{
	uint64 z = instanceId + 0x9E3779B97F4A7C15ull + 0x4C6F6E65ull;		// "Lone" - 다른 해시와 겹치지 않게
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	z = z ^ (z >> 31);
	return static_cast<uint32>(z % 100);
}
