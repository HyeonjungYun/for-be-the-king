#include "pch.h"
#include "HouseTable.h"
#include "CsvTable.h"
#include "FloorBounds.h"

vector<HouseDef> HouseTable::s_houses;

float HouseDef::Distance(float x, float y) const
{
	const float dx = (x < x0) ? x0 - x : (x > x1) ? x - x1 : 0.f;
	const float dy = (y < y0) ? y0 - y : (y > y1) ? y - y1 : 0.f;
	return ::sqrtf(dx * dx + dy * dy);
}

bool HouseTable::Load(const string& dataDir, uint32 floorCount, OUT string& error)
{
	s_houses.clear();

	string text;
	if (Csv::ReadFile(dataDir + "\\houses.csv", OUT text) == false)
		return true;		// 집 없음

	return Parse(text, floorCount, OUT error);
}

bool HouseTable::Parse(const string& csv, uint32 floorCount, OUT string& error)
{
	s_houses.clear();

	CsvTable table;
	vector<int32> c;
	if (Csv::Parse(csv, "houses.csv", OUT table, OUT error) == false
		|| Csv::FindColumns(table, "houses.csv", { "house_id", "floor_id", "x0", "x1", "y0", "y1" }, OUT c, OUT error) == false)
		return false;

	vector<HouseDef> houses;
	for (size_t r = 0; r < table.rows.size(); r++)
	{
		const vector<string>& row = table.rows[r];
		const string where = "houses.csv:" + to_string(table.lineNumbers[r]);

		HouseDef h;
		h.houseId = row[c[0]];
		if (h.houseId.empty() || Csv::ParseUInt(row[c[1]], OUT h.floorId) == false
			|| Csv::ParseFloat(row[c[2]], OUT h.x0) == false || Csv::ParseFloat(row[c[3]], OUT h.x1) == false
			|| Csv::ParseFloat(row[c[4]], OUT h.y0) == false || Csv::ParseFloat(row[c[5]], OUT h.y1) == false)
		{
			error = where + " house_id, floor_id, x0, x1, y0, y1 must be filled with numbers";
			return false;
		}
		if (h.floorId >= floorCount)
		{
			error = where + " floor_id must be 0.." + to_string(floorCount - 1);
			return false;
		}
		if (h.x0 >= h.x1 || h.y0 >= h.y1)
		{
			error = where + " needs x0 < x1 and y0 < y1";
			return false;
		}
		if (FloorBounds::Contains(h.x0, h.y0) == false || FloorBounds::Contains(h.x1, h.y1) == false)
		{
			error = where + " lies outside the floor bounds";
			return false;
		}
		for (const HouseDef& other : houses)
		{
			if (other.houseId == h.houseId)
			{
				error = where + " duplicate house_id " + h.houseId;
				return false;
			}
		}
		houses.push_back(h);
	}

	s_houses = move(houses);
	return true;
}

vector<HouseDef> HouseTable::OnFloor(uint32 floorId)
{
	vector<HouseDef> out;
	for (const HouseDef& h : s_houses)
	{
		if (h.floorId == floorId)
			out.push_back(h);
	}
	return out;
}
