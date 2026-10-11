#include "pch.h"
#include "BotPath.h"
#include <fstream>
#include <sstream>
#include <queue>

namespace
{
	constexpr float		HALF_CM				= FloorBounds::HALF_CM;		// 섬을 감싸는 600 m 사각형 (2026-10-11)
	constexpr float		CELL_CM				= 100.f;
	constexpr int32		GRID				= static_cast<int32>(2.f * HALF_CM / CELL_CM);	// 60,000 / 100 = 600 (36만 칸)
	constexpr float		CLEARANCE_CM		= 60.f;		// 칸 중심이 벽에서 이만큼 떨어져야 지나간다
	constexpr float		SMOOTH_RADIUS_CM	= 40.f;		// 건너뛸 때 이 굵기로 쓸어 본다 - 서버 검증(점)보다 굵게
	constexpr int32		NEAREST_FREE_MAX	= 40;		// 도착점이 벽 안이면 이 칸 수 안에서 빈 칸을 찾는다 (타워 반지름 + 여유)

	WallMap			s_map;
	vector<uint8>	s_blocked;
	bool			s_loaded = false;
	uint32			s_floor = 0;

	int32 CellOf(float v)	{ return std::clamp(static_cast<int32>((v + HALF_CM) / CELL_CM), 0, GRID - 1); }
	float CenterOf(int32 c)	{ return -HALF_CM + (static_cast<float>(c) + 0.5f) * CELL_CM; }
	int32 Index(int32 cx, int32 cy) { return cy * GRID + cx; }

	// 서버 ItemTable::DefaultDataDir 와 같은 규칙 - FBTK_DATA_DIR, 없으면 exe 기준 ..\..\Data
	string DataDir()
	{
		char* raw = nullptr;
		size_t length = 0;
		if (_dupenv_s(&raw, &length, "FBTK_DATA_DIR") == 0 && raw != nullptr)
		{
			const string dir = raw;
			::free(raw);
			return dir;
		}

		char exePath[MAX_PATH] = {};
		::GetModuleFileNameA(nullptr, exePath, MAX_PATH);
		string dir = exePath;
		dir = dir.substr(0, dir.find_last_of("\\/"));
		return dir + "\\..\\..\\Data";
	}

	// 칸에서 가장 가까운 빈 칸 (너비 우선). 없으면 -1
	int32 NearestFree(int32 start)
	{
		if (s_blocked[start] == 0)
			return start;

		vector<uint8> seen(s_blocked.size(), 0);
		queue<pair<int32, int32>> open;		// (칸, 거리)
		open.push({ start, 0 });
		seen[start] = 1;

		while (open.empty() == false)
		{
			const auto [cell, depth] = open.front();
			open.pop();

			if (s_blocked[cell] == 0)
				return cell;
			if (depth >= NEAREST_FREE_MAX)
				continue;

			const int32 cx = cell % GRID;
			const int32 cy = cell / GRID;
			const int32 dx[4] = { 1, -1, 0, 0 };
			const int32 dy[4] = { 0, 0, 1, -1 };
			for (int32 k = 0; k < 4; k++)
			{
				const int32 nx = cx + dx[k];
				const int32 ny = cy + dy[k];
				if (nx < 0 || ny < 0 || nx >= GRID || ny >= GRID)
					continue;
				const int32 next = Index(nx, ny);
				if (seen[next])
					continue;
				seen[next] = 1;
				open.push({ next, depth + 1 });
			}
		}

		return -1;
	}
}

bool BotPath::Load(uint32 floorId)
{
	s_loaded = false;

	const string path = DataDir() + "\\walls.csv";
	ifstream in(path);
	if (in.is_open() == false)
	{
		LOG_LINE << "[BOT PATH] no walls.csv at " << path << " - walking straight\n";
		return false;
	}

	vector<WallBox> walls;
	string line;
	getline(in, line);		// floor_id,kind,cx,cy,half_length,half_thickness,yaw

	while (getline(in, line))
	{
		if (line.empty() == false && line.back() == '\r')
			line.pop_back();
		if (line.empty())
			continue;

		string f[7];
		stringstream ss(line);
		for (string& field : f)
			getline(ss, field, ',');

		// 서버가 부팅 때 이미 검증한 파일이다 - 여기서는 읽기만
		if (static_cast<uint32>(::strtoul(f[0].c_str(), nullptr, 10)) != floorId)
			continue;

		WallBox w;
		if (ParseWallKind(f[1], OUT w.kind) == false)
			continue;
		w.cx = static_cast<float>(::atof(f[2].c_str()));
		w.cy = static_cast<float>(::atof(f[3].c_str()));
		w.halfLength = static_cast<float>(::atof(f[4].c_str()));
		w.halfThickness = static_cast<float>(::atof(f[5].c_str()));
		w.yawDeg = static_cast<float>(::atof(f[6].c_str()));
		walls.push_back(w);
	}

	if (walls.empty())
	{
		LOG_LINE << "[BOT PATH] floor " << floorId << " has no walls - walking straight\n";
		return false;
	}

	string error;
	if (s_map.Build(move(walls), OUT error) == false)
	{
		LOG_LINE << "[BOT PATH] walls rejected: " << error << " - walking straight\n";
		return false;
	}

	s_blocked.assign(GRID * GRID, 0);
	int32 blocked = 0;
	for (int32 cy = 0; cy < GRID; cy++)
	{
		for (int32 cx = 0; cx < GRID; cx++)
		{
			if (s_map.Overlaps(CenterOf(cx), CenterOf(cy), CLEARANCE_CM))
			{
				s_blocked[Index(cx, cy)] = 1;
				blocked++;
			}
		}
	}

	s_loaded = true;
	s_floor = floorId;
	LOG_LINE << "[BOT PATH] floor=" << floorId << " walls=" << s_map.Count() << " blocked_cells=" << blocked << "/" << (GRID * GRID) << '\n';
	return true;
}

bool BotPath::IsLoaded()
{
	return s_loaded;
}

uint32 BotPath::LoadedFloor()
{
	return s_floor;
}

vector<pair<float, float>> BotPath::Find(float fromX, float fromY, float toX, float toY)
{
	vector<pair<float, float>> result;
	if (s_loaded == false)
		return result;

	const int32 start = NearestFree(Index(CellOf(fromX), CellOf(fromY)));
	const int32 goal = NearestFree(Index(CellOf(toX), CellOf(toY)));
	if (start < 0 || goal < 0)
		return result;

	// A* - 8 방향, 대각선은 양옆 두 칸이 모두 비었을 때만 (모서리를 가로지르지 않게). 휴리스틱 = 옥타일 거리
	const int32 count = GRID * GRID;
	vector<float> cost(count, FLT_MAX);
	vector<int32> parent(count, -1);
	vector<uint8> closed(count, 0);

	auto heuristic = [goal](int32 cell)
		{
			const float dx = static_cast<float>(std::abs(cell % GRID - goal % GRID));
			const float dy = static_cast<float>(std::abs(cell / GRID - goal / GRID));
			return (dx + dy) + (1.41421356f - 2.f) * (std::min)(dx, dy);
		};

	using Node = pair<float, int32>;		// (추정 총비용, 칸)
	priority_queue<Node, vector<Node>, greater<Node>> open;
	cost[start] = 0.f;
	open.push({ heuristic(start), start });

	while (open.empty() == false)
	{
		const int32 cell = open.top().second;
		open.pop();

		if (closed[cell])
			continue;
		closed[cell] = 1;

		if (cell == goal)
			break;

		const int32 cx = cell % GRID;
		const int32 cy = cell / GRID;

		for (int32 dy = -1; dy <= 1; dy++)
		{
			for (int32 dx = -1; dx <= 1; dx++)
			{
				if (dx == 0 && dy == 0)
					continue;

				const int32 nx = cx + dx;
				const int32 ny = cy + dy;
				if (nx < 0 || ny < 0 || nx >= GRID || ny >= GRID)
					continue;

				const int32 next = Index(nx, ny);
				if (s_blocked[next] || closed[next])
					continue;

				const bool diagonal = (dx != 0 && dy != 0);
				if (diagonal && (s_blocked[Index(cx + dx, cy)] || s_blocked[Index(cx, cy + dy)]))
					continue;

				const float step = diagonal ? 1.41421356f : 1.f;
				if (cost[cell] + step < cost[next])
				{
					cost[next] = cost[cell] + step;
					parent[next] = cell;
					open.push({ cost[next] + heuristic(next), next });
				}
			}
		}
	}

	if (closed[goal] == 0)
		return result;

	// 칸 경로 (시작 다음 칸 → 도착 칸)
	vector<pair<float, float>> cells;
	for (int32 cell = goal; cell != start && cell >= 0; cell = parent[cell])
		cells.push_back({ CenterOf(cell % GRID), CenterOf(cell / GRID) });
	reverse(cells.begin(), cells.end());

	// 도착점이 빈 곳이면 그 점 그대로 - 칸 중심으로 끝나면 최대 70 cm 어긋난다.
	// 마지막 칸 중심 대신 넣을 때는 그 앞 점에서 곧장 갈 수 있을 때만 (안 되면 칸 중심을 지나서 간다 - 리뷰 ⑤)
	if (s_map.Overlaps(toX, toY, CLEARANCE_CM) == false)
	{
		const float px = (cells.size() >= 2) ? cells[cells.size() - 2].first : fromX;
		const float py = (cells.size() >= 2) ? cells[cells.size() - 2].second : fromY;

		if (cells.empty())
			cells.push_back({ toX, toY });
		else if (s_map.SweepBlocked(px, py, toX, toY, SMOOTH_RADIUS_CM) == false)
			cells.back() = { toX, toY };
		else
			cells.push_back({ toX, toY });
	}

	// 건너뛰기 - 지금 점에서 벽에 닿지 않고 곧장 갈 수 있는 가장 먼 칸까지. 꺾는 점만 남는다
	float ax = fromX;
	float ay = fromY;
	size_t i = 0;
	while (i < cells.size())
	{
		size_t j = i;
		while (j + 1 < cells.size()
			&& s_map.SweepBlocked(ax, ay, cells[j + 1].first, cells[j + 1].second, SMOOTH_RADIUS_CM) == false)
		{
			j++;
		}

		result.push_back(cells[j]);
		ax = cells[j].first;
		ay = cells[j].second;
		i = j + 1;
	}

	return result;
}
