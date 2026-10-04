#include "pch.h"
#include "EntryPointTable.h"
#include "CsvTable.h"

vector<EntryPoint> EntryPointTable::s_points;

bool EntryPointTable::Load(const string& dataDir, uint32 floorCount, OUT string& error)
{
	string text;
	if (Csv::ReadFile(dataDir + "\\entry_points.csv", OUT text) == false)
	{
		error = "cannot open entry_points.csv";
		s_points.clear();
		return false;
	}

	return Parse(text, floorCount, OUT error);
}

bool EntryPointTable::Parse(const string& csv, uint32 floorCount, OUT string& error)
{
	s_points.clear();

	vector<EntryPoint> points;
	unordered_set<uint32> ids;
	vector<uint32> perFloor(floorCount, 0);

	CsvTable table;
	vector<int32> c;

	if (Csv::Parse(csv, "entry_points.csv", OUT table, OUT error) == false
		|| Csv::FindColumns(table, "entry_points.csv", { "entry_id", "floor_id", "x", "y", "z", "yaw" }, OUT c, OUT error) == false)
		return false;

	for (size_t r = 0; r < table.rows.size(); r++)
	{
		const vector<string>& row = table.rows[r];
		const string where = "entry_points.csv:" + to_string(table.lineNumbers[r]);

		EntryPoint p;

		// 0 은 "무작위" 라는 뜻이라 지점 id 로 쓸 수 없다 (C_ENTER_GAME.entry_point_id)
		if (Csv::ParseUInt(row[c[0]], OUT p.entryId) == false || p.entryId == 0)
		{
			error = where + " entry_id must be a positive number (0 means random)";
			return false;
		}

		if (ids.insert(p.entryId).second == false)
		{
			error = where + " duplicate entry_id " + to_string(p.entryId);
			return false;
		}

		if (Csv::ParseUInt(row[c[1]], OUT p.floorId) == false || p.floorId >= floorCount)
		{
			error = where + " floor_id must be 0.." + to_string(floorCount - 1);
			return false;
		}

		if (Csv::ParseFloat(row[c[2]], OUT p.x) == false || Csv::ParseFloat(row[c[3]], OUT p.y) == false
			|| Csv::ParseFloat(row[c[4]], OUT p.z) == false || Csv::ParseFloat(row[c[5]], OUT p.yaw) == false)
		{
			error = where + " x, y, z and yaw must be numbers";
			return false;
		}

		perFloor[p.floorId]++;
		points.push_back(p);
	}

	// 지점이 없는 층에는 들어갈 곳이 없다 - [무작위]도 고를 것이 없다
	for (uint32 floor = 0; floor < floorCount; floor++)
	{
		if (perFloor[floor] == 0)
		{
			error = "entry_points.csv has no entry point on floor " + to_string(floor);
			return false;
		}
	}

	s_points = move(points);
	return true;
}

const EntryPoint* EntryPointTable::Resolve(uint32 floorId, uint32 requestedId, mt19937& rng, OUT bool& wasRandom)
{
	wasRandom = false;

	vector<const EntryPoint*> onFloor;
	for (const EntryPoint& p : s_points)
	{
		if (p.floorId != floorId)
			continue;

		if (requestedId != 0 && p.entryId == requestedId)
			return &p;

		onFloor.push_back(&p);
	}

	if (onFloor.empty())
		return nullptr;

	wasRandom = true;
	uniform_int_distribution<size_t> pick(0, onFloor.size() - 1);
	return onFloor[pick(rng)];
}
