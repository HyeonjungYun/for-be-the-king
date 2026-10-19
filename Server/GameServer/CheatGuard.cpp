#include "pch.h"
#include "CheatGuard.h"
#include "CsvTable.h"

namespace
{
	// csv 키 = "weight_" + 이 이름
	const char* const VIOLATION_NAMES[] =
	{
		"move_speed", "move_wall", "move_bounds", "move_clock", "move_while_locked",
		"attack_range", "attack_wall", "attack_hidden",
		"skill_cooldown", "skill_range", "skill_wall", "skill_hidden",
		"loot_range", "loot_hidden", "action_while_locked",
	};
	static_assert(sizeof(VIOLATION_NAMES) / sizeof(VIOLATION_NAMES[0]) == static_cast<size_t>(Violation::Count), "violation names");
}

const char* ViolationName(Violation v)
{
	const size_t i = static_cast<size_t>(v);
	return i < static_cast<size_t>(Violation::Count) ? VIOLATION_NAMES[i] : "unknown";
}

CheatRules::CheatRules()
{
	// 기본 가중치 - 정상 클라에서도 생기는 거부(지연 · 경계)는 낮게, 정상 클라가 만들 수 없는 것은 높게
	auto set = [this](Violation v, float w) { weights[static_cast<size_t>(v)] = w; };
	set(Violation::MoveSpeed, 3.f);
	set(Violation::MoveWall, 4.f);
	set(Violation::MoveBounds, 10.f);
	set(Violation::MoveClock, 5.f);
	set(Violation::MoveWhileLocked, 0.5f);
	set(Violation::AttackRange, 0.5f);
	set(Violation::AttackWall, 1.f);
	set(Violation::AttackHidden, 3.f);
	set(Violation::SkillCooldown, 2.f);
	set(Violation::SkillRange, 1.f);
	set(Violation::SkillWall, 1.f);
	set(Violation::SkillHidden, 3.f);
	set(Violation::LootRange, 2.f);
	set(Violation::LootHidden, 4.f);
	set(Violation::ActionWhileLocked, 1.f);
}

CheatRules& CheatRules::Get()
{
	static CheatRules s_rules;
	return s_rules;
}

bool CheatRules::Parse(const string& csv, OUT string& error)
{
	CsvTable table;
	vector<int32> c;
	if (Csv::Parse(csv, "anticheat.csv", OUT table, OUT error) == false
		|| Csv::FindColumns(table, "anticheat.csv", { "key", "value" }, OUT c, OUT error) == false)
		return false;

	for (size_t r = 0; r < table.rows.size(); r++)
	{
		const string& key = table.rows[r][c[0]];
		const string& value = table.rows[r][c[1]];
		const string where = "anticheat.csv:" + to_string(table.lineNumbers[r]) + " " + key;

		uint32 u = 0;
		float f = 0.f;
		const bool isUInt = Csv::ParseUInt(value, OUT u);
		const bool isFloat = Csv::ParseFloat(value, OUT f);

		auto uintIn = [&](uint32 lo, uint32 hi) { if (isUInt == false || u < lo || u > hi) { error = where + " must be an integer " + to_string(lo) + ".." + to_string(hi); return false; } return true; };
		auto floatIn = [&](float lo, float hi) { if (isFloat == false || f < lo || f > hi) { error = where + " must be a number " + to_string(lo) + ".." + to_string(hi); return false; } return true; };

		if (key.rfind("weight_", 0) == 0)
		{
			const string name = key.substr(7);
			size_t found = static_cast<size_t>(Violation::Count);
			for (size_t i = 0; i < static_cast<size_t>(Violation::Count); i++)
				if (name == VIOLATION_NAMES[i])
					found = i;
			if (found == static_cast<size_t>(Violation::Count))
			{
				error = where + " is not a known violation";
				return false;
			}
			if (floatIn(0.f, 100.f) == false)
				return false;
			weights[found] = f;
		}
		else if (key == "decay_per_sec") { if (floatIn(0.f, 100.f) == false) return false; decayPerSec = f; }
		else if (key == "warn_score") { if (floatIn(1.f, 10000.f) == false) return false; warnScore = f; }
		else if (key == "kick_score") { if (floatIn(1.f, 10000.f) == false) return false; kickScore = f; }
		else if (key == "strikes_for_ban") { if (uintIn(1, 100) == false) return false; strikesForBan = u; }
		else if (key == "ban_window_hours") { if (uintIn(1, 24 * 30) == false) return false; banWindowHours = u; }
		else if (key == "ban_minutes") { if (uintIn(0, 60 * 24 * 365) == false) return false; banMinutes = u; }
		else if (key == "enabled") { if (uintIn(0, 1) == false) return false; enabled = (u == 1); }
		else
		{
			error = where + " is not a known key";
			return false;
		}
	}

	if (warnScore >= kickScore)
	{
		error = "anticheat.csv: warn_score must be below kick_score";
		return false;
	}
	return true;
}

bool CheatRules::Load(const string& dataDir, OUT string& error)
{
	*this = CheatRules();

	string text;
	if (Csv::ReadFile(dataDir + "\\anticheat.csv", OUT text) && Parse(text, OUT error) == false)
		return false;
	return true;
}

void CheatGuard::Decay(uint64 nowUs, const CheatRules& rules)
{
	if (_lastUs != 0 && nowUs > _lastUs)
	{
		const float sec = static_cast<float>(nowUs - _lastUs) / 1'000'000.f;
		_score = max(0.f, _score - rules.decayPerSec * sec);
	}
	_lastUs = max(_lastUs, nowUs);

	// 경고 기준 아래로 내려오면 다음에 넘을 때 다시 경고한다
	if (_score < rules.warnScore)
		_warned = false;
}

CheatAction CheatGuard::Report(Violation v, uint64 nowUs, const CheatRules& rules)
{
	const size_t i = static_cast<size_t>(v);
	if (i >= static_cast<size_t>(Violation::Count))
		return CheatAction::None;

	Decay(nowUs, rules);
	_counts[i]++;
	_score += rules.weights[i];

	if (_kicked)
		return CheatAction::None;

	if (rules.enabled && _score >= rules.kickScore)
	{
		_kicked = true;
		return CheatAction::Kick;
	}
	if (_warned == false && _score >= rules.warnScore)
	{
		_warned = true;
		return CheatAction::Warn;
	}
	return CheatAction::None;
}

float CheatGuard::Score(uint64 nowUs, const CheatRules& rules)
{
	Decay(nowUs, rules);
	return _score;
}

uint32 CheatGuard::Total() const
{
	uint32 total = 0;
	for (uint32 n : _counts)
		total += n;
	return total;
}

string CheatGuard::Summary(size_t n) const
{
	vector<pair<uint32, size_t>> order;
	for (size_t i = 0; i < static_cast<size_t>(Violation::Count); i++)
		if (_counts[i] > 0)
			order.push_back({ _counts[i], i });
	sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });

	string out;
	for (size_t k = 0; k < order.size() && k < n; k++)
	{
		if (out.empty() == false)
			out += ' ';
		out += VIOLATION_NAMES[order[k].second];
		out += ':';
		out += to_string(order[k].first);
	}
	return out.empty() ? "-" : out;
}

uint64 CheatBan::BanUntil(const vector<uint64>& strikeUnixSecs, uint64 nowUnixSec, const CheatRules& rules)
{
	if (rules.banMinutes == 0 || rules.strikesForBan == 0)
		return 0;

	const uint64 window = static_cast<uint64>(rules.banWindowHours) * 3600;
	uint32 inWindow = 0;
	for (uint64 t : strikeUnixSecs)
		if (t <= nowUnixSec && nowUnixSec - t <= window)
			inWindow++;

	if (inWindow < rules.strikesForBan)
		return 0;
	return nowUnixSec + static_cast<uint64>(rules.banMinutes) * 60;
}
