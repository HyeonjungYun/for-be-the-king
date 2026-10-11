#include "pch.h"
#include "MonsterTable.h"
#include "CsvTable.h"
#include "FloorBounds.h"
#include "SkillTable.h"
#include "EntryPointTable.h"
#include "WallTable.h"
#include "TowerTable.h"

unordered_map<uint32, MonsterTypeDef>	MonsterTable::s_types;
vector<MonsterSpawnDef>					MonsterTable::s_spawns;
unordered_map<uint32, PatrolRoute>		MonsterTable::s_routes;
vector<MonsterPackDef>					MonsterTable::s_packs;

namespace
{
	bool ParseTier(const string& s, OUT MonsterTier& out)
	{
		if (s == "field") { out = MonsterTier::Field; return true; }
		if (s == "house") { out = MonsterTier::House; return true; }
		if (s == "boss") { out = MonsterTier::Boss; return true; }
		if (s == "leader") { out = MonsterTier::Leader; return true; }
		return false;
	}

	bool ParseAggroKind(const string& s, OUT MonsterAggroKind& out)
	{
		if (s == "guard") { out = MonsterAggroKind::Guard; return true; }
		if (s == "detect") { out = MonsterAggroKind::Detect; return true; }
		if (s == "reactive") { out = MonsterAggroKind::Reactive; return true; }
		return false;
	}

	bool ParseInt(const string& s, OUT int32& out)
	{
		uint32 v = 0;
		if (Csv::ParseUInt(s, OUT v) == false || v > static_cast<uint32>(INT32_MAX))
			return false;
		out = static_cast<int32>(v);
		return true;
	}

	// 범위 검사 한 줄 - 실패하면 "<where> <name> must be lo..hi" 를 남긴다
	template<typename T>
	bool InRange(T v, T lo, T hi, const string& where, const char* name, OUT string& error)
	{
		if (v >= lo && v <= hi)
			return true;

		ostringstream os;
		os << where << ' ' << name << " must be " << lo << ".." << hi << " (got " << v << ")";
		error = os.str();
		return false;
	}

	float Dist(float ax, float ay, float bx, float by)
	{
		const float dx = ax - bx;
		const float dy = ay - by;
		return ::sqrtf(dx * dx + dy * dy);
	}

	bool ParseTypes(const string& csv, OUT unordered_map<uint32, MonsterTypeDef>& types, OUT string& error)
	{
		CsvTable table;
		vector<int32> c;

		if (Csv::Parse(csv, "monster_types.csv", OUT table, OUT error) == false
			|| Csv::FindColumns(table, "monster_types.csv", {
				"monster_type_id", "name_key", "tier", "max_hp", "attack_power", "attack_speed", "attack_range_cm",
				"armor", "magic_resist", "resistance", "move_speed_cms", "radius_cm", "aggro_radius_cm", "leash_radius_cm",
				"search_s", "give_up_s", "assist_radius_cm", "displace_immune", "skill_id", "drop_tier", "drop_chance_pct", "drop_rolls" },
				OUT c, OUT error) == false)
			return false;

		// aggro_kind 는 고를 열 - 없으면 전부 guard (예전 데이터 · 테스트 픽스처가 그대로 읽힌다)
		auto kindIt = table.columns.find("aggro_kind");
		const int32 kindCol = (kindIt != table.columns.end()) ? kindIt->second : -1;

		for (size_t r = 0; r < table.rows.size(); r++)
		{
			const vector<string>& row = table.rows[r];
			const string where = "monster_types.csv:" + to_string(table.lineNumbers[r]);

			MonsterTypeDef t;
			uint32 displace = 0;

			if (kindCol >= 0 && kindCol < static_cast<int32>(row.size()) && row[kindCol].empty() == false
				&& ParseAggroKind(row[kindCol], OUT t.aggroKind) == false)
			{
				error = where + " unknown aggro_kind '" + row[kindCol] + "' (guard / detect / reactive)";
				return false;
			}
			float moveSpeed = 0.f, radius = 0.f, aggro = 0.f, leash = 0.f, assist = 0.f;

			if (Csv::ParseUInt(row[c[0]], OUT t.typeId) == false || t.typeId == 0)
			{
				error = where + " monster_type_id must be a positive number";
				return false;
			}

			if (types.count(t.typeId) > 0)
			{
				error = where + " duplicate monster_type_id " + to_string(t.typeId);
				return false;
			}

			t.nameKey = row[c[1]];
			if (t.nameKey.empty())
			{
				error = where + " name_key is empty";
				return false;
			}

			if (ParseTier(row[c[2]], OUT t.tier) == false)
			{
				error = where + " unknown tier '" + row[c[2]] + "' (field / house / boss)";
				return false;
			}

			if (ParseInt(row[c[3]], OUT t.maxHp) == false || ParseInt(row[c[4]], OUT t.attackPower) == false
				|| Csv::ParseFloat(row[c[5]], OUT t.attackSpeed) == false || Csv::ParseFloat(row[c[6]], OUT t.attackRangeCm) == false
				|| ParseInt(row[c[7]], OUT t.armor) == false || ParseInt(row[c[8]], OUT t.magicResist) == false
				|| Csv::ParseFloat(row[c[9]], OUT t.resistance) == false || Csv::ParseFloat(row[c[10]], OUT moveSpeed) == false
				|| Csv::ParseFloat(row[c[11]], OUT radius) == false || Csv::ParseFloat(row[c[12]], OUT aggro) == false
				|| Csv::ParseFloat(row[c[13]], OUT leash) == false || Csv::ParseFloat(row[c[14]], OUT t.searchS) == false
				|| Csv::ParseFloat(row[c[15]], OUT t.giveUpS) == false || Csv::ParseFloat(row[c[16]], OUT assist) == false
				|| Csv::ParseUInt(row[c[17]], OUT displace) == false || Csv::ParseUInt(row[c[18]], OUT t.skillId) == false
				|| Csv::ParseUInt(row[c[20]], OUT t.dropChancePct) == false || Csv::ParseUInt(row[c[21]], OUT t.dropRolls) == false)
			{
				error = where + " every value except name_key, tier and drop_tier must be a non-negative number";
				return false;
			}

			t.moveSpeedCms = moveSpeed;
			t.radiusCm = radius;
			t.aggroRadiusCm = aggro;
			t.leashRadiusCm = leash;
			t.assistRadiusCm = assist;

			if (InRange(t.maxHp, 1, 100000, where, "max_hp", error) == false
				|| InRange(t.attackPower, 0, 1000, where, "attack_power", error) == false
				|| InRange(t.attackSpeed, 0.5f, 2.5f, where, "attack_speed", error) == false
				|| InRange(t.attackRangeCm, 50.f, 600.f, where, "attack_range_cm", error) == false
				|| InRange(t.armor, 0, 300, where, "armor", error) == false
				|| InRange(t.magicResist, 0, 300, where, "magic_resist", error) == false
				|| InRange(t.resistance, 0.f, 1.f, where, "resistance", error) == false
				|| InRange(moveSpeed, 1.f, MonsterConfig::MAX_SPEED_CMS, where, "move_speed_cms (cap 0.9 x 340 - players must be able to outrun)", error) == false
				|| InRange(radius, 20.f, MonsterConfig::MAX_RADIUS_CM, where, "radius_cm", error) == false
				|| InRange(aggro, 200.f, 800.f, where, "aggro_radius_cm", error) == false
				|| InRange(t.searchS, 0.f, 10.f, where, "search_s", error) == false
				// 탐지형 무리는 "둘 중 하나가 죽을 때까지" - 포기 시간 0 (끔) 을 허용한다
				|| InRange(t.giveUpS, t.aggroKind == MonsterAggroKind::Detect ? 0.f : 1.f, 60.f, where, "give_up_s", error) == false
				|| InRange(assist, 0.f, 3000.f, where, "assist_radius_cm", error) == false
				|| InRange(displace, 0u, 1u, where, "displace_immune", error) == false
				|| InRange(t.dropChancePct, 0u, 100u, where, "drop_chance_pct", error) == false
				|| InRange(t.dropRolls, 0u, 4u, where, "drop_rolls", error) == false)
				return false;

			// 탐지형은 끈이 없다 (0 = 무한). 그 밖은 끈 > 감지
			const bool unlimitedLeash = (t.aggroKind == MonsterAggroKind::Detect && leash == 0.f);
			if (unlimitedLeash == false && leash <= aggro)
			{
				error = where + " leash_radius_cm must be greater than aggro_radius_cm (detect packs may use 0 = no leash)";
				return false;
			}

			t.displaceImmune = (displace == 1);

			if (t.skillId != 0 && SkillTable::Find(t.skillId) == nullptr)
			{
				error = where + " skill_id " + to_string(t.skillId) + " is not in SkillTable";
				return false;
			}

			const string& dropTier = row[c[19]];
			if (dropTier == "none")
			{
				t.drops = false;
			}
			else if (FloorItemPool::ParseTier(dropTier, OUT t.dropTier))
			{
				t.drops = true;
			}
			else
			{
				error = where + " unknown drop_tier '" + dropTier + "' (none / open / house)";
				return false;
			}

			types[t.typeId] = t;
		}

		if (types.empty())
		{
			error = "monster_types.csv has no rows";
			return false;
		}

		return true;
	}

	bool ParseSpawns(const string& csv, uint32 floorCount, const unordered_map<uint32, MonsterTypeDef>& types,
		OUT vector<MonsterSpawnDef>& spawns, OUT string& error)
	{
		CsvTable table;
		vector<int32> c;

		if (Csv::Parse(csv, "monster_spawns.csv", OUT table, OUT error) == false
			|| Csv::FindColumns(table, "monster_spawns.csv", { "spawn_id", "floor_id", "monster_type_id", "x", "y", "z", "yaw", "group_id", "respawn_s" },
				OUT c, OUT error) == false)
			return false;

		unordered_set<uint32> ids;
		uint32 shortRespawns = 0;

		for (size_t r = 0; r < table.rows.size(); r++)
		{
			const vector<string>& row = table.rows[r];
			const string where = "monster_spawns.csv:" + to_string(table.lineNumbers[r]);

			MonsterSpawnDef s;

			if (Csv::ParseUInt(row[c[0]], OUT s.spawnId) == false || s.spawnId == 0)
			{
				error = where + " spawn_id must be a positive number";
				return false;
			}

			if (ids.insert(s.spawnId).second == false)
			{
				error = where + " duplicate spawn_id " + to_string(s.spawnId);
				return false;
			}

			if (Csv::ParseUInt(row[c[1]], OUT s.floorId) == false || s.floorId >= floorCount)
			{
				error = where + " floor_id must be 0.." + to_string(floorCount - 1);
				return false;
			}

			auto typeIt = types.end();
			if (Csv::ParseUInt(row[c[2]], OUT s.typeId) == false || (typeIt = types.find(s.typeId)) == types.end())
			{
				error = where + " unknown monster_type_id '" + row[c[2]] + "'";
				return false;
			}
			const MonsterTypeDef& type = typeIt->second;

			if (Csv::ParseFloat(row[c[3]], OUT s.x) == false || Csv::ParseFloat(row[c[4]], OUT s.y) == false
				|| Csv::ParseFloat(row[c[5]], OUT s.z) == false || Csv::ParseFloat(row[c[6]], OUT s.yaw) == false
				|| Csv::ParseUInt(row[c[7]], OUT s.groupId) == false || Csv::ParseUInt(row[c[8]], OUT s.respawnSeconds) == false)
			{
				error = where + " x, y, z, yaw, group_id and respawn_s must be numbers";
				return false;
			}

			// 경계에서 10 m - 정상 추격으로 경계에 닿지 않는다
			constexpr float LIMIT = FloorBounds::HALF_CM - MonsterConfig::BOUNDARY_MIN_DIST_CM;
			if (FloorBounds::Contains(s.x, s.y) == false || ::fabsf(s.x) > LIMIT || ::fabsf(s.y) > LIMIT)
			{
				error = where + " home must be inside the floor and at least 10 m from its edge";
				return false;
			}

			// 집에서 몸이 벽에 걸리면 첫 걸음부터 막힌다 (AC-MA-02)
			if (WallTable::Get(s.floorId).Overlaps(s.x, s.y, type.radiusCm))
			{
				ostringstream os;
				os << where << " home (" << s.x << ", " << s.y << ") radius " << type.radiusCm << " overlaps a server wall";
				error = os.str();
				return false;
			}

			for (const EntryPoint& e : EntryPointTable::All())
			{
				if (e.floorId == s.floorId && Dist(s.x, s.y, e.x, e.y) < MonsterConfig::ENTRY_MIN_DIST_CM)
				{
					error = where + " home is closer than 20 m to entry point " + to_string(e.entryId);
					return false;
				}
			}

			for (const TowerDef& t : TowerTable::All())
			{
				if (t.floorId == s.floorId && Dist(s.x, s.y, t.x, t.y) < MonsterConfig::TOWER_MIN_DIST_CM)
				{
					error = where + " home is closer than 35 m to tower " + t.towerId;
					return false;
				}
			}

			for (const MonsterSpawnDef& other : spawns)
			{
				if (other.floorId == s.floorId && other.typeId == s.typeId && Dist(s.x, s.y, other.x, other.y) <= 1.f)
				{
					error = where + " same monster type as spawn " + to_string(other.spawnId) + " at the same place";
					return false;
				}
			}

			if (type.drops && FloorItemPool::Find(s.floorId, type.dropTier) == nullptr)
			{
				error = where + " no floor_item_pool for floor " + to_string(s.floorId) + " " + FloorItemPool::TierName(type.dropTier)
					+ " (monster type " + to_string(s.typeId) + " drops from it)";
				return false;
			}

			if (s.respawnSeconds < MonsterConfig::MIN_RESPAWN_S)
			{
				error = where + " respawn_s must be at least " + to_string(MonsterConfig::MIN_RESPAWN_S);
				return false;
			}

			if (s.respawnSeconds < MonsterConfig::SESSION_LENGTH_S)
				shortRespawns++;

			spawns.push_back(s);
		}

		if (shortRespawns > 0)
		{
			LOG_LINE << "[MON] warning - " << shortRespawns << " spawn(s) respawn faster than " << MonsterConfig::SESSION_LENGTH_S
				<< "s - one run can hunt the same spot twice\n";
		}

		return true;
	}
}

namespace
{
	// 점 → 선분 거리 (순찰 길 ↔ 진입 지점 · 타워)
	float SegmentDist(float px, float py, float ax, float ay, float bx, float by)
	{
		const float vx = bx - ax;
		const float vy = by - ay;
		const float len2 = vx * vx + vy * vy;
		float t = (len2 > 0.0001f) ? ((px - ax) * vx + (py - ay) * vy) / len2 : 0.f;
		t = (std::max)(0.f, (std::min)(1.f, t));
		return Dist(px, py, ax + vx * t, ay + vy * t);
	}

	bool ParseRoutes(const string& csv, uint32 floorCount, OUT unordered_map<uint32, PatrolRoute>& routes, OUT string& error)
	{
		CsvTable table;
		vector<int32> c;
		if (Csv::Parse(csv, "patrol_routes.csv", OUT table, OUT error) == false
			|| Csv::FindColumns(table, "patrol_routes.csv", { "route_id", "floor_id", "seq", "x", "y" }, OUT c, OUT error) == false)
			return false;

		for (size_t r = 0; r < table.rows.size(); r++)
		{
			const vector<string>& row = table.rows[r];
			const string where = "patrol_routes.csv:" + to_string(table.lineNumbers[r]);

			uint32 routeId = 0, floorId = 0, seq = 0;
			float x = 0.f, y = 0.f;
			if (Csv::ParseUInt(row[c[0]], OUT routeId) == false || routeId == 0 || Csv::ParseUInt(row[c[1]], OUT floorId) == false
				|| Csv::ParseUInt(row[c[2]], OUT seq) == false || Csv::ParseFloat(row[c[3]], OUT x) == false || Csv::ParseFloat(row[c[4]], OUT y) == false)
			{
				error = where + " route_id (> 0), floor_id, seq, x, y must be numbers";
				return false;
			}
			if (floorId >= floorCount)
			{
				error = where + " floor_id must be 0.." + to_string(floorCount - 1);
				return false;
			}

			PatrolRoute& route = routes[routeId];
			if (route.points.empty())
			{
				route.routeId = routeId;
				route.floorId = floorId;
			}
			if (route.floorId != floorId)
			{
				error = where + " route " + to_string(routeId) + " changes floor";
				return false;
			}
			if (seq != route.points.size())
			{
				error = where + " route " + to_string(routeId) + " seq must count up from 0 without gaps (expected " + to_string(route.points.size()) + ")";
				return false;
			}

			// 경계에서 10 m - 정상 추격으로 경계에 닿지 않는다 (집 규칙과 같다)
			constexpr float LIMIT = FloorBounds::HALF_CM - MonsterConfig::BOUNDARY_MIN_DIST_CM;
			if (FloorBounds::Contains(x, y) == false || ::fabsf(x) > LIMIT || ::fabsf(y) > LIMIT)
			{
				error = where + " point must be inside the floor and at least 10 m from its edge";
				return false;
			}

			route.points.emplace_back(x, y);
		}

		for (auto& [id, route] : routes)
		{
			if (route.points.size() < 3)
			{
				error = "patrol_routes.csv route " + to_string(id) + " needs at least 3 points (a closed loop)";
				return false;
			}
		}
		return true;
	}

	// 길의 모든 선분(닫는 선분 포함)을 radius 로 쓸어 벽 · 진입 지점 · 타워를 본다
	bool ValidateRouteGeometry(const PatrolRoute& route, float radius, OUT string& error)
	{
		const WallMap& walls = WallTable::Get(route.floorId);
		const size_t n = route.points.size();

		for (size_t i = 0; i < n; i++)
		{
			const auto& a = route.points[i];
			const auto& b = route.points[(i + 1) % n];
			const string where = "patrol_routes.csv route " + to_string(route.routeId) + " segment " + to_string(i) + "->" + to_string((i + 1) % n);

			if (Dist(a.first, a.second, b.first, b.second) < 100.f)
			{
				error = where + " is shorter than 1 m";
				return false;
			}

			if (walls.SweepBlocked(a.first, a.second, b.first, b.second, radius))
			{
				ostringstream os;
				os << where << " (" << a.first << "," << a.second << ")->(" << b.first << "," << b.second << ") radius " << radius << " hits a server wall";
				error = os.str();
				return false;
			}

			for (const EntryPoint& e : EntryPointTable::All())
			{
				if (e.floorId == route.floorId && SegmentDist(e.x, e.y, a.first, a.second, b.first, b.second) < MonsterConfig::ENTRY_MIN_DIST_CM)
				{
					error = where + " passes closer than 20 m to entry point " + to_string(e.entryId);
					return false;
				}
			}

			for (const TowerDef& t : TowerTable::All())
			{
				if (t.floorId == route.floorId && SegmentDist(t.x, t.y, a.first, a.second, b.first, b.second) < MonsterConfig::ROUTE_TOWER_MIN_DIST_CM)
				{
					error = where + " passes closer than 20 m to tower " + t.towerId;
					return false;
				}
			}
		}
		return true;
	}

	bool ParsePackRows(const string& csv, uint32 floorCount, const unordered_map<uint32, MonsterTypeDef>& types,
		const unordered_map<uint32, PatrolRoute>& routes, OUT vector<MonsterPackDef>& packs, OUT string& error)
	{
		CsvTable table;
		vector<int32> c;
		if (Csv::Parse(csv, "monster_packs.csv", OUT table, OUT error) == false
			|| Csv::FindColumns(table, "monster_packs.csv", { "pack_id", "floor_id", "route_id", "start_seq", "leader_type_id", "member_type_id",
				"member_count", "spacing_cm", "patrol_speed_cms", "respawn_s" }, OUT c, OUT error) == false)
			return false;

		unordered_set<uint32> ids;
		for (size_t r = 0; r < table.rows.size(); r++)
		{
			const vector<string>& row = table.rows[r];
			const string where = "monster_packs.csv:" + to_string(table.lineNumbers[r]);

			MonsterPackDef p;
			if (Csv::ParseUInt(row[c[0]], OUT p.packId) == false || p.packId == 0 || Csv::ParseUInt(row[c[1]], OUT p.floorId) == false
				|| Csv::ParseUInt(row[c[2]], OUT p.routeId) == false || Csv::ParseUInt(row[c[3]], OUT p.startSeq) == false
				|| Csv::ParseUInt(row[c[4]], OUT p.leaderTypeId) == false || Csv::ParseUInt(row[c[5]], OUT p.memberTypeId) == false
				|| Csv::ParseUInt(row[c[6]], OUT p.memberCount) == false || Csv::ParseFloat(row[c[7]], OUT p.spacingCm) == false
				|| Csv::ParseFloat(row[c[8]], OUT p.patrolSpeedCms) == false || Csv::ParseUInt(row[c[9]], OUT p.respawnSeconds) == false)
			{
				error = where + " every value must be a non-negative number (pack_id > 0)";
				return false;
			}

			if (ids.insert(p.packId).second == false)
			{
				error = where + " duplicate pack_id " + to_string(p.packId);
				return false;
			}
			if (p.floorId >= floorCount)
			{
				error = where + " floor_id must be 0.." + to_string(floorCount - 1);
				return false;
			}

			auto routeIt = routes.find(p.routeId);
			if (routeIt == routes.end() || routeIt->second.floorId != p.floorId)
			{
				error = where + " route " + to_string(p.routeId) + " does not exist on floor " + to_string(p.floorId);
				return false;
			}
			if (p.startSeq >= routeIt->second.points.size())
			{
				error = where + " start_seq is past the end of route " + to_string(p.routeId);
				return false;
			}

			auto leaderIt = types.find(p.leaderTypeId);
			auto memberIt = types.find(p.memberTypeId);
			if (leaderIt == types.end() || memberIt == types.end())
			{
				error = where + " unknown leader_type_id or member_type_id";
				return false;
			}

			const MonsterTypeDef& leader = leaderIt->second;
			const MonsterTypeDef& member = memberIt->second;
			if (leader.aggroKind == MonsterAggroKind::Guard || leader.aggroKind != member.aggroKind)
			{
				error = where + " leader and members must share one pack aggro_kind (detect / reactive)";
				return false;
			}
			p.kind = leader.aggroKind;

			if (p.memberCount > MonsterConfig::PACK_MAX_MEMBERS)
			{
				error = where + " member_count must be 0.." + to_string(MonsterConfig::PACK_MAX_MEMBERS);
				return false;
			}
			if (p.spacingCm < leader.radiusCm + member.radiusCm + 20.f || p.spacingCm > 400.f)
			{
				error = where + " spacing_cm must be at least the two radii + 20 and at most 400";
				return false;
			}
			const float slowest = (std::min)(leader.moveSpeedCms, member.moveSpeedCms);
			if (p.patrolSpeedCms <= 0.f || p.patrolSpeedCms > slowest * MonsterConfig::PATROL_SPEED_MAX_RATIO)
			{
				error = where + " patrol_speed_cms must be in (0, 0.8 x the slowest member speed] - followers need room to catch up";
				return false;
			}
			if (p.respawnSeconds < MonsterConfig::MIN_RESPAWN_S)
			{
				error = where + " respawn_s must be at least " + to_string(MonsterConfig::MIN_RESPAWN_S);
				return false;
			}
			for (const MonsterTypeDef* t : { &leader, &member })
			{
				if (t->drops && FloorItemPool::Find(p.floorId, t->dropTier) == nullptr)
				{
					error = where + " no floor_item_pool for floor " + to_string(p.floorId) + " " + FloorItemPool::TierName(t->dropTier);
					return false;
				}
			}

			const float radius = (std::max)(leader.radiusCm, member.radiusCm) + MonsterConfig::ROUTE_CLEARANCE_MARGIN_CM;
			if (ValidateRouteGeometry(routeIt->second, radius, OUT error) == false)
			{
				error = where + " - " + error;
				return false;
			}

			packs.push_back(p);
		}
		return true;
	}
}

bool MonsterTable::Load(const string& dataDir, uint32 floorCount, OUT string& error)
{
	string types, spawns;
	const bool hasTypes = Csv::ReadFile(dataDir + "\\monster_types.csv", OUT types);
	const bool hasSpawns = Csv::ReadFile(dataDir + "\\monster_spawns.csv", OUT spawns);

	if (Parse(hasTypes ? &types : nullptr, hasSpawns ? &spawns : nullptr, floorCount, OUT error) == false)
		return false;

	// 무리 · 순찰 길 (2026-10-11) - 종류 다음에
	string routes, packs;
	const bool hasRoutes = Csv::ReadFile(dataDir + "\\patrol_routes.csv", OUT routes);
	const bool hasPacks = Csv::ReadFile(dataDir + "\\monster_packs.csv", OUT packs);
	return ParsePacks(hasRoutes ? &routes : nullptr, hasPacks ? &packs : nullptr, floorCount, OUT error);
}

bool MonsterTable::ParsePacks(const string* routesCsv, const string* packsCsv, uint32 floorCount, OUT string& error)
{
	s_routes.clear();
	s_packs.clear();

	if (packsCsv == nullptr)
		return true;		// 길만 있어도 무리 0

	if (routesCsv == nullptr)
	{
		error = "monster_packs.csv exists but patrol_routes.csv does not";
		return false;
	}

	unordered_map<uint32, PatrolRoute> routes;
	if (ParseRoutes(*routesCsv, floorCount, OUT routes, OUT error) == false)
		return false;

	vector<MonsterPackDef> packs;
	if (ParsePackRows(*packsCsv, floorCount, s_types, routes, OUT packs, OUT error) == false)
		return false;

	s_routes = move(routes);
	s_packs = move(packs);
	for (MonsterPackDef& p : s_packs)
		p.route = &s_routes.at(p.routeId);		// 표는 이 뒤로 바뀌지 않는다
	return true;
}

const PatrolRoute* MonsterTable::FindRoute(uint32 routeId)
{
	auto it = s_routes.find(routeId);
	return it == s_routes.end() ? nullptr : &it->second;
}

const char* MonsterTable::AggroKindName(MonsterAggroKind kind)
{
	switch (kind)
	{
	case MonsterAggroKind::Guard:		return "guard";
	case MonsterAggroKind::Detect:		return "detect";
	case MonsterAggroKind::Reactive:	return "reactive";
	default:							return "?";
	}
}

bool MonsterTable::Parse(const string* typesCsv, const string* spawnsCsv, uint32 floorCount, OUT string& error)
{
	s_types.clear();
	s_spawns.clear();

	if (typesCsv == nullptr)
	{
		if (spawnsCsv != nullptr)
		{
			error = "monster_spawns.csv exists but monster_types.csv does not";
			return false;
		}
		return true;
	}

	unordered_map<uint32, MonsterTypeDef> types;
	if (ParseTypes(*typesCsv, OUT types, OUT error) == false)
		return false;

	vector<MonsterSpawnDef> spawns;
	if (spawnsCsv != nullptr && ParseSpawns(*spawnsCsv, floorCount, types, OUT spawns, OUT error) == false)
		return false;

	s_types = move(types);
	s_spawns = move(spawns);
	return true;
}

const MonsterTypeDef* MonsterTable::FindType(uint32 typeId)
{
	auto it = s_types.find(typeId);
	return it == s_types.end() ? nullptr : &it->second;
}

const char* MonsterTable::TierName(MonsterTier tier)
{
	switch (tier)
	{
	case MonsterTier::Field:	return "field";
	case MonsterTier::House:	return "house";
	case MonsterTier::Boss:		return "boss";
	case MonsterTier::Leader:	return "leader";
	default:					return "?";
	}
}
