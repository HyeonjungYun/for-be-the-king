#include "pch.h"
#include "TowerTable.h"
#include "CsvTable.h"
#include "FloorBounds.h"

vector<TowerDef> TowerTable::s_towers;

bool TowerTable::Load(const string& dataDir, uint32 floorCount, OUT string& error)
{
	string text;
	if (Csv::ReadFile(dataDir + "\\towers.csv", OUT text) == false)
	{
		s_towers.clear();
		return true;
	}

	return Parse(text, floorCount, OUT error);
}

bool TowerTable::Parse(const string& csv, uint32 floorCount, OUT string& error)
{
	s_towers.clear();

	vector<TowerDef> towers;
	set<pair<uint32, string>> ids;

	CsvTable table;
	vector<int32> c;

	if (Csv::Parse(csv, "towers.csv", OUT table, OUT error) == false
		|| Csv::FindColumns(table, "towers.csv", { "tower_id", "floor_id", "x", "y" }, OUT c, OUT error) == false)
		return false;

	for (size_t r = 0; r < table.rows.size(); r++)
	{
		const vector<string>& row = table.rows[r];
		const string where = "towers.csv:" + to_string(table.lineNumbers[r]);

		TowerDef t;
		t.towerId = row[c[0]];

		if (t.towerId.empty())
		{
			error = where + " tower_id is empty";
			return false;
		}

		if (Csv::ParseUInt(row[c[1]], OUT t.floorId) == false || t.floorId >= floorCount)
		{
			error = where + " floor_id must be 0.." + to_string(floorCount - 1);
			return false;
		}

		if (ids.insert({ t.floorId, t.towerId }).second == false)
		{
			error = where + " duplicate tower " + t.towerId + " on floor " + to_string(t.floorId);
			return false;
		}

		if (Csv::ParseFloat(row[c[2]], OUT t.x) == false || Csv::ParseFloat(row[c[3]], OUT t.y) == false
			|| FloorBounds::Contains(t.x, t.y) == false)
		{
			error = where + " x and y must be numbers inside the floor (±" + to_string(static_cast<int32>(FloorBounds::HALF_CM)) + ")";
			return false;
		}

		towers.push_back(t);
	}

	s_towers = move(towers);
	return true;
}
