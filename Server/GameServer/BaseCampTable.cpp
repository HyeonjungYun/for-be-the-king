#include "pch.h"
#include "BaseCampTable.h"
#include "CsvTable.h"
#include <cmath>

bool					BaseCampTable::s_loaded = false;
WallMap					BaseCampTable::s_walls;
BaseCampPoint			BaseCampTable::s_area;
BaseCampPoint			BaseCampTable::s_spawn;
BaseCampPoint			BaseCampTable::s_dummy;
uint32					BaseCampTable::s_dummyTypeId = 0;
vector<BaseCampPoint>	BaseCampTable::s_npcs;

namespace
{
	bool InsideArea(const BaseCampPoint& area, float x, float y, float margin)
	{
		return std::fabs(x - area.x) <= area.halfX - margin && std::fabs(y - area.y) <= area.halfY - margin;
	}
}

bool BaseCampTable::Load(const string& dataDir, OUT string& error)
{
	s_loaded = false;

	string text;
	if (Csv::ReadFile(dataDir + "\\basecamp.csv", OUT text) == false)
		return true;		// 파일 없음 = 맵 없음 (예전 UI 베이스캠프)
	return Parse(text, OUT error);
}

bool BaseCampTable::Parse(const string& csv, OUT string& error)
{
	s_loaded = false;
	s_walls = WallMap();
	s_area = s_spawn = s_dummy = BaseCampPoint();
	s_dummyTypeId = 0;
	s_npcs.clear();

	CsvTable table;
	vector<int32> c;
	if (Csv::Parse(csv, "basecamp.csv", OUT table, OUT error) == false
		|| Csv::FindColumns(table, "basecamp.csv", { "kind", "id", "x", "y", "yaw", "half_x", "half_y" }, OUT c, OUT error) == false)
		return false;

	bool hasArea = false, hasSpawn = false, hasDummy = false;
	vector<BaseCampPoint> props;

	for (size_t r = 0; r < table.rows.size(); r++)
	{
		const vector<string>& row = table.rows[r];
		const string where = "basecamp.csv:" + to_string(table.lineNumbers[r]);
		const string& kind = row[c[0]];

		BaseCampPoint p;
		p.id = row[c[1]];
		if (Csv::ParseFloat(row[c[2]], OUT p.x) == false || Csv::ParseFloat(row[c[3]], OUT p.y) == false || Csv::ParseFloat(row[c[4]], OUT p.yaw) == false
			|| Csv::ParseFloat(row[c[5]], OUT p.halfX) == false || Csv::ParseFloat(row[c[6]], OUT p.halfY) == false)
		{
			error = where + " x, y, yaw, half_x, half_y must be numbers";
			return false;
		}
		if (FloorBounds::Contains(p.x, p.y) == false || p.halfX < 0.f || p.halfY < 0.f)
		{
			error = where + " must be inside the floor square and half sizes must not be negative";
			return false;
		}

		if (kind == "area")
		{
			if (hasArea || p.halfX < 500.f || p.halfY < 500.f || p.halfX > 5000.f || p.halfY > 5000.f)
			{
				error = where + " exactly one area, half sizes 500..5000 cm";
				return false;
			}
			if (FloorBounds::Contains(p.x - p.halfX, p.y - p.halfY) == false || FloorBounds::Contains(p.x + p.halfX, p.y + p.halfY) == false)
			{
				error = where + " the area must be inside the floor square";
				return false;
			}
			s_area = p;
			hasArea = true;
		}
		else if (kind == "spawn")
		{
			if (hasSpawn) { error = where + " only one spawn"; return false; }
			s_spawn = p;
			hasSpawn = true;
		}
		else if (kind == "dummy")
		{
			uint32 typeId = 0;
			if (hasDummy || Csv::ParseUInt(p.id, OUT typeId) == false || typeId == 0)
			{
				error = where + " only one dummy, its id is a monster_type_id";
				return false;
			}
			s_dummy = p;
			s_dummyTypeId = typeId;
			hasDummy = true;
		}
		else if (kind == "npc")
		{
			// trade = 매입 행수 (2026-10-17 경제) - 없어도 된다 (전리품을 팔 곳이 없을 뿐)
			if (p.id != "stash" && p.id != "enhance" && p.id != "gate" && p.id != "trade")
			{
				error = where + " npc id must be stash, enhance, gate or trade";
				return false;
			}
			for (const BaseCampPoint& n : s_npcs)
			{
				if (n.id == p.id)
				{
					error = where + " duplicate npc " + p.id;
					return false;
				}
			}
			if (p.halfX < 20.f || p.halfY < 20.f)
			{
				error = where + " npc body half sizes must be at least 20 cm";
				return false;
			}
			s_npcs.push_back(p);
		}
		else if (kind == "prop")
		{
			if (p.halfX < 10.f || p.halfY < 10.f)
			{
				error = where + " prop half sizes must be at least 10 cm";
				return false;
			}
			props.push_back(p);
		}
		else
		{
			error = where + " unknown kind '" + kind + "' (area / spawn / dummy / npc / prop)";
			return false;
		}
	}

	auto hasNpc = [&](const char* id) { return any_of(s_npcs.begin(), s_npcs.end(), [&](const BaseCampPoint& n) { return n.id == id; }); };
	if (hasArea == false || hasSpawn == false || hasNpc("stash") == false || hasNpc("enhance") == false || hasNpc("gate") == false)
	{
		error = "basecamp.csv needs one area, one spawn and the three npcs (stash, enhance, gate)";
		return false;
	}

	// 서는 곳 · 허수아비 · NPC 는 바닥 안 (가장자리 1 m 안쪽)
	auto inside = [&](const BaseCampPoint& p, const char* what)
		{
			if (InsideArea(s_area, p.x, p.y, 100.f))
				return true;
			error = string("basecamp.csv ") + what + " '" + p.id + "' must be inside the area, 1 m from its edge";
			return false;
		};
	if (inside(s_spawn, "spawn") == false || (hasDummy && inside(s_dummy, "dummy") == false))
		return false;
	for (const BaseCampPoint& n : s_npcs)
		if (inside(n, "npc") == false)
			return false;

	// 벽 - NPC 와 장식은 기둥(Pillar) 상자. 바닥 네모가 해안선이다 (밖으로 못 나간다)
	vector<WallBox> boxes;
	auto addBox = [&](const BaseCampPoint& p)
		{
			WallBox w;
			w.kind = WallKind::Pillar;
			w.cx = p.x;
			w.cy = p.y;
			w.halfLength = p.halfX;
			w.halfThickness = p.halfY;
			w.yawDeg = p.yaw;
			boxes.push_back(w);
		};
	for (const BaseCampPoint& n : s_npcs)
		addBox(n);
	for (const BaseCampPoint& p : props)
		addBox(p);

	if (s_walls.Build(boxes, OUT error) == false)
		return false;

	vector<OutlinePoint> outline = {
		{ s_area.x - s_area.halfX, s_area.y - s_area.halfY },
		{ s_area.x + s_area.halfX, s_area.y - s_area.halfY },
		{ s_area.x + s_area.halfX, s_area.y + s_area.halfY },
		{ s_area.x - s_area.halfX, s_area.y + s_area.halfY },
	};
	if (s_walls.SetOutline(outline, OUT error) == false)
		return false;

	// 서는 곳이 장애물 안이면 들어오자마자 갇힌다
	if (s_walls.Overlaps(s_spawn.x, s_spawn.y, 50.f))
	{
		error = "basecamp.csv spawn overlaps an npc or a prop";
		return false;
	}

	s_loaded = true;
	return true;
}

bool BaseCampTable::CheckClearOf(const WallMap& floor, OUT string& error)
{
	if (s_loaded == false)
		return true;

	const BaseCampPoint& a = s_area;
	const float xs[3] = { a.x - a.halfX, a.x, a.x + a.halfX };
	const float ys[3] = { a.y - a.halfY, a.y, a.y + a.halfY };
	for (float x : xs)
	{
		for (float y : ys)
		{
			// 해안선이 있는 층만 - 해안선 없는 층은 사각형 전체가 층이라 겹칠 수밖에 없다 (그땐 검사하지 않는다)
			if (floor.HasOutline() && floor.InsideFloor(x, y))
			{
				error = "basecamp.csv area overlaps the island (" + to_string(static_cast<int32>(x)) + "," + to_string(static_cast<int32>(y)) + ")";
				return false;
			}
		}
	}
	return true;
}
