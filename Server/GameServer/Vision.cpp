#include "pch.h"
#include "Vision.h"
#include "CsvTable.h"
#include <cmath>

namespace
{
	VisionRules		s_rules;
	atomic<bool>	s_enabled = true;

	bool ReadKey(const unordered_map<string, string>& kv, const char* key, OUT float& out, OUT string& error)
	{
		auto it = kv.find(key);
		if (it == kv.end() || Csv::ParseFloat(it->second, OUT out) == false)
		{
			error = string("vision_rules.csv is missing or has a bad '") + key + "'";
			return false;
		}
		return true;
	}

	bool ReadKey(const unordered_map<string, string>& kv, const char* key, OUT uint32& out, OUT string& error)
	{
		auto it = kv.find(key);
		if (it == kv.end() || Csv::ParseUInt(it->second, OUT out) == false)
		{
			error = string("vision_rules.csv is missing or has a bad '") + key + "'";
			return false;
		}
		return true;
	}
}

bool Vision::Load(const string& dataDir, OUT string& error)
{
	string text;
	if (Csv::ReadFile(dataDir + "\\vision_rules.csv", OUT text) == false)
	{
		error = "cannot open vision_rules.csv";
		return false;
	}
	return Parse(text, OUT error);
}

bool Vision::Parse(const string& csv, OUT string& error)
{
	CsvTable table;
	vector<int32> c;
	if (Csv::Parse(csv, "vision_rules.csv", OUT table, OUT error) == false
		|| Csv::FindColumns(table, "vision_rules.csv", { "key", "value" }, OUT c, OUT error) == false)
		return false;

	unordered_map<string, string> kv;
	for (const vector<string>& row : table.rows)
		kv[row[c[0]]] = row[c[1]];

	VisionRules rules;
	if (ReadKey(kv, "vision_radius_cm", OUT rules.radiusCm, OUT error) == false
		|| ReadKey(kv, "vision_grace_ms", OUT rules.graceMs, OUT error) == false
		|| ReadKey(kv, "vision_min_radius_cm", OUT rules.minRadiusCm, OUT error) == false)
		return false;

	// 반경이 0 이면 아무도 안 보이고, 층(280 m)보다 크면 FoW 가 무의미하다. 유예가 길면 숨은 사람이 오래 남는다
	if (rules.radiusCm <= 0.f || rules.radiusCm > 28000.f || rules.minRadiusCm <= 0.f || rules.minRadiusCm > rules.radiusCm
		|| rules.graceMs > 2000)
	{
		error = "vision_rules.csv has a value out of range (radius 0 < r <= 28000, 0 < min <= radius, grace <= 2000 ms)";
		return false;
	}

	s_rules = rules;
	return true;
}

const VisionRules& Vision::Rules()
{
	return s_rules;
}

void Vision::SetEnabled(bool on)
{
	s_enabled.store(on);
}

bool Vision::Enabled()
{
	return s_enabled.load();
}

bool Vision::LineOfSight(const WallMap& walls, float ax, float ay, float bx, float by)
{
	return walls.SightBlocked(ax, ay, bx, by) == false;
}

bool Vision::SourceSees(const WallMap& walls, const Source& src, float x, float y)
{
	const float dx = x - src.x;
	const float dy = y - src.y;
	if (dx * dx + dy * dy > src.radiusCm * src.radiusCm)
		return false;

	return src.needsLos == false || LineOfSight(walls, src.x, src.y, x, y);
}

bool Vision::AnySees(const WallMap& walls, const vector<Source>& sources, float x, float y, bool stealthed)
{
	for (const Source& src : sources)
	{
		if (stealthed && src.seesStealth == false)
			continue;
		if (SourceSees(walls, src, x, y))
			return true;
	}
	return false;
}

bool Vision::SegmentNearCircle(float ax, float ay, float bx, float by, float cx, float cy, float r)
{
	const double dx = static_cast<double>(bx) - ax;
	const double dy = static_cast<double>(by) - ay;
	const double len2 = dx * dx + dy * dy;

	double t = 0.0;
	if (len2 > 0.0)
	{
		t = ((static_cast<double>(cx) - ax) * dx + (static_cast<double>(cy) - ay) * dy) / len2;
		t = (std::max)(0.0, (std::min)(1.0, t));
	}

	const double px = ax + dx * t - cx;
	const double py = ay + dy * t - cy;
	return px * px + py * py <= static_cast<double>(r) * r;
}
