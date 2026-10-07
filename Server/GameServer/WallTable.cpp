#include "pch.h"
#include "WallTable.h"
#include "CsvTable.h"
#include "SpawnPlacement.h"
#include <cmath>

// 스폰이 진입 지점에서 이 반경까지 퍼진다 - 무벽 원이 그보다 작으면 스폰이 벽에 박힌다
static_assert(WallTable::ENTRY_CLEAR_RADIUS_CM >= SpawnPlacement::MAX_RADIUS_CM, "spawn spread must stay inside the wall-free circle");

vector<WallMap> WallTable::s_maps;
vector<string> WallTable::s_versions;

atomic<bool> GWallCheck = true;

namespace
{
	bool ParseVersions(const string& csv, uint32 floorCount, OUT vector<string>& versions, OUT string& error)
	{
		CsvTable table;
		vector<int32> c;

		if (Csv::Parse(csv, "level_versions.csv", OUT table, OUT error) == false
			|| Csv::FindColumns(table, "level_versions.csv", { "floor_id", "data_version" }, OUT c, OUT error) == false)
			return false;

		for (size_t r = 0; r < table.rows.size(); r++)
		{
			const vector<string>& row = table.rows[r];
			const string where = "level_versions.csv:" + to_string(table.lineNumbers[r]);

			uint32 floor = 0;
			if (Csv::ParseUInt(row[c[0]], OUT floor) == false || floor >= floorCount)
			{
				error = where + " floor_id must be 0.." + to_string(floorCount - 1);
				return false;
			}

			if (row[c[1]].empty())
			{
				error = where + " data_version is empty";
				return false;
			}

			if (versions[floor].empty() == false)
			{
				error = where + " duplicate floor_id " + to_string(floor);
				return false;
			}

			versions[floor] = row[c[1]];
		}

		return true;
	}

	bool ParseWalls(const string& csv, uint32 floorCount, OUT vector<vector<WallBox>>& perFloor, OUT string& error)
	{
		CsvTable table;
		vector<int32> c;

		if (Csv::Parse(csv, "walls.csv", OUT table, OUT error) == false
			|| Csv::FindColumns(table, "walls.csv", { "floor_id", "kind", "cx", "cy", "half_length", "half_thickness", "yaw" }, OUT c, OUT error) == false)
			return false;

		for (size_t r = 0; r < table.rows.size(); r++)
		{
			const vector<string>& row = table.rows[r];
			const string where = "walls.csv:" + to_string(table.lineNumbers[r]);

			uint32 floor = 0;
			if (Csv::ParseUInt(row[c[0]], OUT floor) == false || floor >= floorCount)
			{
				error = where + " floor_id must be 0.." + to_string(floorCount - 1);
				return false;
			}

			WallBox w;
			if (ParseWallKind(row[c[1]], OUT w.kind) == false)
			{
				error = where + " unknown kind '" + row[c[1]] + "' (wall, door_post, pillar, free_pillar, cover, tower)";
				return false;
			}

			// from_chars 는 nan · inf 도 읽는다 - 따로 막는다
			if (Csv::ParseFloat(row[c[2]], OUT w.cx) == false || Csv::ParseFloat(row[c[3]], OUT w.cy) == false
				|| Csv::ParseFloat(row[c[4]], OUT w.halfLength) == false || Csv::ParseFloat(row[c[5]], OUT w.halfThickness) == false
				|| Csv::ParseFloat(row[c[6]], OUT w.yawDeg) == false
				|| std::isfinite(w.cx) == false || std::isfinite(w.cy) == false || std::isfinite(w.halfLength) == false
				|| std::isfinite(w.halfThickness) == false || std::isfinite(w.yawDeg) == false)
			{
				error = where + " cx, cy, half_length, half_thickness and yaw must be finite numbers";
				return false;
			}

			if (w.halfLength <= 0.f || w.halfThickness <= 0.f)
			{
				error = where + " half_length and half_thickness must be > 0";
				return false;
			}

			// 내보내기가 [0, 180) 으로 정규화한다 (ADR-0006 E9) - 벗어나면 내보내기 쪽 버그다
			if (w.yawDeg < 0.f || w.yawDeg >= 180.f)
			{
				error = where + " yaw must be in [0, 180)";
				return false;
			}

			perFloor[floor].push_back(w);
		}

		return true;
	}
}

bool WallTable::Load(const string& dataDir, uint32 floorCount, const vector<EntryPoint>& entries, OUT string& error)
{
	string walls;
	string versions;
	const bool hasWalls = Csv::ReadFile(dataDir + "\\walls.csv", OUT walls);
	const bool hasVersions = Csv::ReadFile(dataDir + "\\level_versions.csv", OUT versions);

	return Parse(hasWalls ? &walls : nullptr, hasVersions ? &versions : nullptr, floorCount, entries, OUT error);
}

bool WallTable::Parse(const string* wallsCsv, const string* versionsCsv, uint32 floorCount,
	const vector<EntryPoint>& entries, OUT string& error)
{
	s_maps.assign(floorCount, WallMap());
	s_versions.assign(floorCount, string());

	vector<string> versions(floorCount);
	if (versionsCsv != nullptr && ParseVersions(*versionsCsv, floorCount, OUT versions, OUT error) == false)
		return false;

	vector<vector<WallBox>> perFloor(floorCount);
	if (wallsCsv != nullptr && ParseWalls(*wallsCsv, floorCount, OUT perFloor, OUT error) == false)
		return false;

	vector<WallMap> maps(floorCount);
	for (uint32 floor = 0; floor < floorCount; floor++)
	{
		if (versions[floor].empty() == false && perFloor[floor].empty())
		{
			error = "floor " + to_string(floor) + " is in level_versions.csv but walls.csv has no wall on it";
			return false;
		}

		string buildError;
		if (maps[floor].Build(move(perFloor[floor]), OUT buildError) == false)
		{
			error = "walls.csv floor " + to_string(floor) + ": " + buildError;
			return false;
		}
	}

	// 진입 지점 9 m 안에 벽이 있으면 스폰이 벽에 박힐 수 있다 (Core Rule 15)
	for (const EntryPoint& e : entries)
	{
		if (e.floorId < floorCount && maps[e.floorId].Overlaps(e.x, e.y, ENTRY_CLEAR_RADIUS_CM))
		{
			error = "entry point " + to_string(e.entryId) + " (floor " + to_string(e.floorId) + ") has a wall within "
				+ to_string(static_cast<int32>(ENTRY_CLEAR_RADIUS_CM)) + " cm";
			return false;
		}
	}

	s_maps = move(maps);
	s_versions = move(versions);
	return true;
}

const WallMap& WallTable::Get(uint32 floorId)
{
	static const WallMap empty;
	return (floorId < s_maps.size()) ? s_maps[floorId] : empty;
}

const string& WallTable::DataVersion(uint32 floorId)
{
	static const string empty;
	return (floorId < s_versions.size()) ? s_versions[floorId] : empty;
}
