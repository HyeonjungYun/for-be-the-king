#include "pch.h"
#include "ChestSpawnTable.h"
#include "CsvTable.h"

vector<ChestSpawn> ChestSpawnTable::s_spawns;

namespace
{
	// object_id 는 CHEST_ID_BASE(30억) + chest_id 다. 가방 id(20억 ~)와 겹치지 않게 10억 안에 둔다.
	constexpr uint32 MAX_CHEST_ID = 999'999'999;

	// 이보다 짧으면 한 런(session_length) 안에서 같은 상자를 다시 털 수 있다 (quick-spec § 8)
	constexpr uint32 SESSION_LENGTH_SECONDS = 1800;
}

bool ChestSpawnTable::Load(const string& dataDir, uint32 floorCount, OUT string& error)
{
	string text;
	if (Csv::ReadFile(dataDir + "\\chest_spawns.csv", OUT text) == false)
	{
		error = "cannot open chest_spawns.csv";
		s_spawns.clear();
		return false;
	}

	return Parse(text, floorCount, OUT error);
}

bool ChestSpawnTable::Parse(const string& csv, uint32 floorCount, OUT string& error)
{
	s_spawns.clear();

	vector<ChestSpawn> spawns;
	unordered_set<uint32> ids;

	CsvTable table;
	vector<int32> c;

	if (Csv::Parse(csv, "chest_spawns.csv", OUT table, OUT error) == false
		|| Csv::FindColumns(table, "chest_spawns.csv", { "chest_id", "floor_id", "tier", "x", "y", "z", "yaw", "respawn_s" }, OUT c, OUT error) == false)
		return false;

	uint32 shortRespawns = 0;

	for (size_t r = 0; r < table.rows.size(); r++)
	{
		const vector<string>& row = table.rows[r];
		const string where = "chest_spawns.csv:" + to_string(table.lineNumbers[r]);

		ChestSpawn s;

		if (Csv::ParseUInt(row[c[0]], OUT s.chestId) == false || s.chestId == 0 || s.chestId > MAX_CHEST_ID)
		{
			error = where + " chest_id must be 1.." + to_string(MAX_CHEST_ID);
			return false;
		}

		if (ids.insert(s.chestId).second == false)
		{
			error = where + " duplicate chest_id " + to_string(s.chestId);
			return false;
		}

		if (Csv::ParseUInt(row[c[1]], OUT s.floorId) == false || s.floorId >= floorCount)
		{
			error = where + " floor_id must be 0.." + to_string(floorCount - 1);
			return false;
		}

		if (FloorItemPool::ParseTier(row[c[2]], OUT s.tier) == false)
		{
			error = where + " unknown tier '" + row[c[2]] + "' (open / house)";
			return false;
		}

		// 열 때 롤할 풀이 없으면 빈 상자가 된다 - 빈 상자는 만들지 않는다 (§ 5)
		if (FloorItemPool::Find(s.floorId, s.tier) == nullptr)
		{
			error = where + " no floor_item_pool for floor " + to_string(s.floorId) + " " + FloorItemPool::TierName(s.tier);
			return false;
		}

		if (Csv::ParseFloat(row[c[3]], OUT s.x) == false || Csv::ParseFloat(row[c[4]], OUT s.y) == false
			|| Csv::ParseFloat(row[c[5]], OUT s.z) == false || Csv::ParseFloat(row[c[6]], OUT s.yaw) == false)
		{
			error = where + " x, y, z and yaw must be numbers";
			return false;
		}

		if (Csv::ParseUInt(row[c[7]], OUT s.respawnSeconds) == false || s.respawnSeconds == 0)
		{
			error = where + " respawn_s must be a positive number";
			return false;
		}

		if (s.respawnSeconds < SESSION_LENGTH_SECONDS)
			shortRespawns++;

		spawns.push_back(s);
	}

	// 오류는 아니다 - 리스폰 검증 때 일부러 줄인다. 알리기만 한다.
	if (shortRespawns > 0)
	{
		LOG_LINE << "[BOOT] warning - " << shortRespawns << " chest(s) respawn faster than " << SESSION_LENGTH_SECONDS
			<< "s - one run can loot the same chest twice\n";
	}

	s_spawns = move(spawns);
	return true;
}
