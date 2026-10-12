#include "pch.h"
#include "PatrolRouteGen.h"
#include "CsvTable.h"
#include "FloorBounds.h"
#include <queue>
#include <functional>
#include <sstream>
#include <algorithm>
#include <cfloat>

vector<PackRules> PackRulesTable::s_rules;

/*-------- pack_rules.csv --------*/

namespace
{
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
}

bool PackRulesTable::Load(const string& dataDir, uint32 floorCount, OUT string& error)
{
	s_rules.clear();
	string text;
	if (Csv::ReadFile(dataDir + "\\pack_rules.csv", OUT text) == false)
		return true;		// 규칙 없음 - CSV 무리 그대로
	return Parse(text, floorCount, OUT error);
}

bool PackRulesTable::Parse(const string& csv, uint32 floorCount, OUT string& error)
{
	s_rules.clear();

	CsvTable table;
	vector<int32> c;
	if (Csv::Parse(csv, "pack_rules.csv", OUT table, OUT error) == false
		|| Csv::FindColumns(table, "pack_rules.csv", { "floor_id", "pack_count", "reactive_pct", "member_count", "spacing_cm", "patrol_speed_cms",
			"respawn_s", "route_sets", "sep_min_cm", "house_min_cm", "coast_min_cm", "cover_radius_cm", "cell_cm", "candidates", "lane_gap_cm" },
			OUT c, OUT error) == false)
		return false;

	vector<PackRules> rules;
	for (size_t r = 0; r < table.rows.size(); r++)
	{
		const vector<string>& row = table.rows[r];
		const string where = "pack_rules.csv:" + to_string(table.lineNumbers[r]);
		PackRules p;
		if (Csv::ParseUInt(row[c[0]], OUT p.floorId) == false || Csv::ParseUInt(row[c[1]], OUT p.packCount) == false
			|| Csv::ParseUInt(row[c[2]], OUT p.reactivePct) == false || Csv::ParseUInt(row[c[3]], OUT p.memberCount) == false
			|| Csv::ParseFloat(row[c[4]], OUT p.spacingCm) == false || Csv::ParseFloat(row[c[5]], OUT p.patrolSpeedCms) == false
			|| Csv::ParseUInt(row[c[6]], OUT p.respawnS) == false || Csv::ParseUInt(row[c[7]], OUT p.routeSets) == false
			|| Csv::ParseFloat(row[c[8]], OUT p.sepMinCm) == false || Csv::ParseFloat(row[c[9]], OUT p.houseMinCm) == false
			|| Csv::ParseFloat(row[c[10]], OUT p.coastMinCm) == false
			|| Csv::ParseFloat(row[c[11]], OUT p.coverRadiusCm) == false || Csv::ParseFloat(row[c[12]], OUT p.cellCm) == false
			|| Csv::ParseUInt(row[c[13]], OUT p.candidates) == false || Csv::ParseFloat(row[c[14]], OUT p.laneGapCm) == false)
		{
			error = where + " every column must be a number";
			return false;
		}
		if (p.floorId >= floorCount)
		{
			error = where + " floor_id must be 0.." + to_string(floorCount - 1);
			return false;
		}
		for (const PackRules& other : rules)
		{
			if (other.floorId == p.floorId)
			{
				error = where + " duplicate floor_id " + to_string(p.floorId);
				return false;
			}
		}
		if (InRange(p.packCount, 1u, 40u, where, "pack_count", OUT error) == false
			|| InRange(p.reactivePct, 0u, 100u, where, "reactive_pct", OUT error) == false
			|| InRange(p.memberCount, 1u, MonsterConfig::PACK_MAX_MEMBERS, where, "member_count", OUT error) == false
			|| InRange(p.spacingCm, 100.f, 400.f, where, "spacing_cm", OUT error) == false
			|| InRange(p.patrolSpeedCms, 50.f, MonsterConfig::MAX_SPEED_CMS, where, "patrol_speed_cms", OUT error) == false
			|| InRange(p.respawnS, MonsterConfig::MIN_RESPAWN_S, 86400u, where, "respawn_s", OUT error) == false
			|| InRange(p.routeSets, 1u, 16u, where, "route_sets", OUT error) == false
			|| InRange(p.sepMinCm, 500.f, 10000.f, where, "sep_min_cm", OUT error) == false
			|| InRange(p.houseMinCm, 0.f, 10000.f, where, "house_min_cm", OUT error) == false
			|| InRange(p.coastMinCm, 0.f, 5000.f, where, "coast_min_cm", OUT error) == false
			|| InRange(p.coverRadiusCm, 500.f, 10000.f, where, "cover_radius_cm", OUT error) == false
			|| InRange(p.cellCm, 200.f, 1000.f, where, "cell_cm", OUT error) == false
			|| InRange(p.candidates, 1u, 64u, where, "candidates", OUT error) == false
			|| InRange(p.laneGapCm, 200.f, 1000.f, where, "lane_gap_cm", OUT error) == false)
			return false;
		rules.push_back(p);
	}

	s_rules = move(rules);
	return true;
}

const PackRules* PackRulesTable::Find(uint32 floorId)
{
	for (const PackRules& r : s_rules)
	{
		if (r.floorId == floorId)
			return &r;
	}
	return nullptr;
}

/*-------- 기하 --------*/

namespace
{
	constexpr float PI_F = 3.14159265f;
	constexpr float SAMPLE_CM = 100.f;		// 선분 정확 검사 표본 간격
	constexpr int32 MAX_PULL_SKIP = 48;		// 당겨 펴기 - 한 번에 건너뛰는 최대 칸 수
	constexpr float MIN_ROUTE_LEN_CM = 8000.f;	// 길 하나가 들어갈 만한 성분 - 이 길이만큼의 칸
	constexpr float CROSS_TILT_DEG = 40.f;	// 가로지르는 방향 = 긴 축에 수직 ± 이만큼 (묶음마다 다르다)
	constexpr float END_BAND_CM = 2000.f;	// 띠의 양 끝 - 해안 띠 가운데 d 투영이 끝에서 이 안인 칸에서 길의 끝을 고른다
	constexpr size_t CENTRE_PICK = 6;		// 같은 쪽 길의 경유점 - 섬 중심에 가장 가까운 칸 6 개 가운데
	constexpr float OUT_BAND_COST = 4.f;	// 띠 밖 칸의 A* 추가 비용 - 돌아갈 때만 벗어난다
	constexpr float NOISE_AMP = 0.6f;		// A* 비용 잡음 (0..amp) - 클수록 구불구불, 땅을 더 먹는다
	constexpr float OUT_BAND_PENALTY = 25.f;	// 후보 점수 - 띠 밖 표본 하나마다 (덮는 칸 수에서 뺀다)
	constexpr float OVERFLOW_MIN_LEN_CM = 4000.f;	// 넘침 길이 들어갈 최소 성분 (칸 수 = 이 길이 / cell)
	constexpr float MIN_CROSS_CM = 5000.f;		// 횡단의 두 끝(해안 칸)은 직선으로 이만큼 떨어진다 (AC-PR-02)
	constexpr uint32 SIDE_ROUTES_MAX = 1;		// 본체 밖 성분에 두는 길 수 - 가장 큰 성분 하나만 (R17)
	constexpr float MIN_CROSS_INLAND_CM = 3000.f;	// 그 직선의 가운데는 해안에서 이만큼 안쪽 - 같은 해안선의 두 칸을 잇는 해안 띠 고리는 횡단이 아니다 (R18)
	constexpr float MIN_LOOP_LEN_CM = 8000.f;		// 고리 둘레 하한 - 이보다 짧은 후보는 버린다 ("끝에서 끝" 은 긴 길이다)

	float Dist(float ax, float ay, float bx, float by)
	{
		const float dx = ax - bx;
		const float dy = ay - by;
		return ::sqrtf(dx * dx + dy * dy);
	}

	float SegmentDist(float px, float py, float ax, float ay, float bx, float by)
	{
		const float vx = bx - ax;
		const float vy = by - ay;
		const float len2 = vx * vx + vy * vy;
		float t = (len2 > 0.0001f) ? ((px - ax) * vx + (py - ay) * vy) / len2 : 0.f;
		t = (std::max)(0.f, (std::min)(1.f, t));
		return Dist(px, py, ax + vx * t, ay + vy * t);
	}

	float Cross(float ax, float ay, float bx, float by, float cx, float cy)
	{
		return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
	}

	bool SegmentsIntersect(float ax, float ay, float bx, float by, float cx, float cy, float dx, float dy)
	{
		const float d1 = Cross(cx, cy, dx, dy, ax, ay);
		const float d2 = Cross(cx, cy, dx, dy, bx, by);
		const float d3 = Cross(ax, ay, bx, by, cx, cy);
		const float d4 = Cross(ax, ay, bx, by, dx, dy);
		return ((d1 > 0.f && d2 < 0.f) || (d1 < 0.f && d2 > 0.f)) && ((d3 > 0.f && d4 < 0.f) || (d3 < 0.f && d4 > 0.f));
	}

	float SegSegDist(float ax, float ay, float bx, float by, float cx, float cy, float dx, float dy)
	{
		if (SegmentsIntersect(ax, ay, bx, by, cx, cy, dx, dy))
			return 0.f;
		return (std::min)({ SegmentDist(ax, ay, cx, cy, dx, dy), SegmentDist(bx, by, cx, cy, dx, dy),
			SegmentDist(cx, cy, ax, ay, bx, by), SegmentDist(dx, dy, ax, ay, bx, by) });
	}

	using Pt = pair<float, float>;

	float PolylineDist(float px, float py, const vector<Pt>& pts, bool closed)
	{
		float best = FLT_MAX;
		const size_t n = pts.size();
		if (n == 1)
			return Dist(px, py, pts[0].first, pts[0].second);
		const size_t segs = closed ? n : n - 1;
		for (size_t i = 0; i < segs; i++)
		{
			const Pt& a = pts[i];
			const Pt& b = pts[(i + 1) % n];
			best = (std::min)(best, SegmentDist(px, py, a.first, a.second, b.first, b.second));
		}
		return best;
	}

	/*-------- 격자 --------*/

	struct Grid
	{
		float			minX = 0.f;
		float			minY = 0.f;
		float			cell = 400.f;
		int32			nx = 0;
		int32			ny = 0;
		vector<uint8>	walkable;		// 정적 - 벽 · 해안 · 집 · 타워 · 진입 지점
		vector<uint8>	coast;			// 해안 띠의 칸 (길의 끝은 여기서 고른다)
		vector<uint8>	blocked;		// 동적 - 앞서 정한 길 sep_min 안
		vector<uint8>	covered;		// 동적 - cover_radius 안

		int32	Index(int32 i, int32 j) const { return j * nx + i; }
		bool	In(int32 i, int32 j) const { return i >= 0 && j >= 0 && i < nx && j < ny; }
		float	Cx(int32 idx) const { return minX + (static_cast<float>(idx % nx) + 0.5f) * cell; }
		float	Cy(int32 idx) const { return minY + (static_cast<float>(idx / nx) + 0.5f) * cell; }
		int32	CellOf(float x, float y) const
		{
			const int32 i = static_cast<int32>(::floorf((x - minX) / cell));
			const int32 j = static_cast<int32>(::floorf((y - minY) / cell));
			return In(i, j) ? Index(i, j) : -1;
		}
		bool	Passable(int32 idx) const { return walkable[idx] != 0 && blocked[idx] == 0; }
	};

	struct GenContext
	{
		const RouteGenWorld*	world = nullptr;
		const PackRules*		rules = nullptr;
		Grid					grid;
		Pt						centroid;
		float					margin = 0.f;			// 칸 중심 검사 여유 = 0.71 × cell (칸 안 어느 점도 중심에서 이만큼 안)
		mt19937_64				rng;
		uint32					components = 0;			// BuildGrid 가 남긴 성분 수
		uint32					droppedCells = 0;		// BuildGrid 가 버린 칸 수 (길 하나 분량이 안 되는 조각)
	};

	// 정적 통과 가능 - 칸 중심으로 판단하되 margin 을 더해 칸 안 어느 점도 규칙을 지키게
	bool StaticOk(const GenContext& g, float x, float y, float extra)
	{
		const RouteGenWorld& w = *g.world;
		const PackRules& r = *g.rules;
		if (w.walls->InsideFloor(x, y) == false || w.walls->CoastGap(x, y) < r.coastMinCm + extra)
			return false;
		if (w.walls->Overlaps(x, y, w.bodyRadiusCm + extra))
			return false;
		for (const HouseDef& h : w.houses)
		{
			if (h.Distance(x, y) < r.houseMinCm + extra)
				return false;
		}
		for (const Pt& t : w.towers)
		{
			if (Dist(x, y, t.first, t.second) < MonsterConfig::ROUTE_TOWER_MIN_DIST_CM + extra)
				return false;
		}
		for (const Pt& e : w.entries)
		{
			if (Dist(x, y, e.first, e.second) < MonsterConfig::ENTRY_MIN_DIST_CM + extra)
				return false;
		}
		return true;
	}

	// 길 하나 분량(minCells)이 안 되는 연결 성분(4 방향)을 버린다. 섬 본체에서 떨어졌어도 충분히 큰 땅은 남긴다 -
	// 집 15 m · 진입 · 타워 20 m · 해안 10 m 원이 봉인한 서쪽 lobe 같은 곳에도 길이 들어가야 "곳곳에 고르게" 다 (R17, 검증자 2026-10-12 수정 1).
	// 예전엔 가장 큰 성분만 남겨 그 땅이 통째로 사라졌고 덮임 % 는 남은 땅만 셌다
	void DropSmallComponents(Grid& g, int32 minCells, OUT uint32& kept, OUT uint32& droppedCells)
	{
		vector<int32> comp(g.walkable.size(), -1);
		vector<int32> sizes;
		int32 next = 0;
		vector<int32> stack;
		for (int32 idx = 0; idx < static_cast<int32>(g.walkable.size()); idx++)
		{
			if (g.walkable[idx] == 0 || comp[idx] != -1)
				continue;
			int32 size = 0;
			stack.clear();
			stack.push_back(idx);
			comp[idx] = next;
			while (stack.empty() == false)
			{
				const int32 cur = stack.back();
				stack.pop_back();
				size++;
				const int32 i = cur % g.nx;
				const int32 j = cur / g.nx;
				const int32 di[4] = { 1, -1, 0, 0 };
				const int32 dj[4] = { 0, 0, 1, -1 };
				for (int32 k = 0; k < 4; k++)
				{
					const int32 ni = i + di[k];
					const int32 nj = j + dj[k];
					if (g.In(ni, nj) == false)
						continue;
					const int32 n = g.Index(ni, nj);
					if (g.walkable[n] != 0 && comp[n] == -1)
					{
						comp[n] = next;
						stack.push_back(n);
					}
				}
			}
			sizes.push_back(size);
			next++;
		}
		kept = 0;
		droppedCells = 0;
		for (int32 s : sizes)
			kept += (s >= minCells) ? 1u : 0u;
		for (size_t idx = 0; idx < g.walkable.size(); idx++)
		{
			if (g.walkable[idx] != 0 && sizes[comp[idx]] < minCells)
			{
				g.walkable[idx] = 0;
				droppedCells++;
			}
		}
	}

	void BuildGrid(GenContext& g)
	{
		const WallMap& walls = *g.world->walls;
		const PackRules& r = *g.rules;
		Grid& grid = g.grid;

		float minX = -FloorBounds::HALF_CM, maxX = FloorBounds::HALF_CM, minY = -FloorBounds::HALF_CM, maxY = FloorBounds::HALF_CM;
		if (walls.HasOutline())
		{
			minX = FLT_MAX; maxX = -FLT_MAX; minY = FLT_MAX; maxY = -FLT_MAX;
			for (const OutlinePoint& p : walls.Outline())
			{
				minX = (std::min)(minX, p.x); maxX = (std::max)(maxX, p.x);
				minY = (std::min)(minY, p.y); maxY = (std::max)(maxY, p.y);
			}
		}
		grid.cell = r.cellCm;
		grid.minX = minX;
		grid.minY = minY;
		grid.nx = static_cast<int32>(::ceilf((maxX - minX) / grid.cell));
		grid.ny = static_cast<int32>(::ceilf((maxY - minY) / grid.cell));
		const size_t count = static_cast<size_t>(grid.nx) * grid.ny;
		grid.walkable.assign(count, 0);
		grid.coast.assign(count, 0);
		grid.blocked.assign(count, 0);
		grid.covered.assign(count, 0);
		g.margin = grid.cell * 0.7072f + r.laneGapCm * 0.5f;		// 칸 안 어느 점 + 중심선에서 줄까지 (두 줄은 중심선 ± lane_gap/2)

		for (int32 idx = 0; idx < static_cast<int32>(count); idx++)
			grid.walkable[idx] = StaticOk(g, grid.Cx(idx), grid.Cy(idx), g.margin) ? 1 : 0;
		DropSmallComponents(grid, static_cast<int32>(MIN_ROUTE_LEN_CM / grid.cell), OUT g.components, OUT g.droppedCells);

		// 해안 띠 - 걸을 수 있는 칸 가운데 해안 안쪽 한계에서 3 칸 안 (끝 칸을 고를 여지)
		for (int32 idx = 0; idx < static_cast<int32>(count); idx++)
		{
			if (grid.walkable[idx] != 0 && walls.CoastGap(grid.Cx(idx), grid.Cy(idx)) <= r.coastMinCm + g.margin + 3.f * grid.cell)
				grid.coast[idx] = 1;
		}
	}

	/*-------- A* --------*/

	// 부드러운 잡음 - 8 칸 격자에 난수를 놓고 선형 보간. 같은 길을 두 번 그리지 않게 한다
	struct ValueNoise
	{
		int32			lattice = 8;
		int32			lx = 0;
		int32			ly = 0;
		vector<float>	v;
		float			amp = 1.f;

		void Build(const Grid& g, mt19937_64& rng, float amplitude)
		{
			amp = amplitude;
			lx = g.nx / lattice + 2;
			ly = g.ny / lattice + 2;
			v.resize(static_cast<size_t>(lx) * ly);
			uniform_real_distribution<float> d(0.f, 1.f);
			for (float& f : v)
				f = d(rng);
		}

		float At(const Grid& g, int32 idx) const
		{
			const float fi = static_cast<float>(idx % g.nx) / static_cast<float>(lattice);
			const float fj = static_cast<float>(idx / g.nx) / static_cast<float>(lattice);
			const int32 i0 = static_cast<int32>(fi);
			const int32 j0 = static_cast<int32>(fj);
			const float tx = fi - static_cast<float>(i0);
			const float ty = fj - static_cast<float>(j0);
			auto at = [&](int32 i, int32 j) { return v[static_cast<size_t>((std::min)(j, ly - 1)) * lx + (std::min)(i, lx - 1)]; };
			const float a = at(i0, j0) * (1.f - tx) + at(i0 + 1, j0) * tx;
			const float b = at(i0, j0 + 1) * (1.f - tx) + at(i0 + 1, j0 + 1) * tx;
			return (a * (1.f - ty) + b * ty) * amp;
		}
	};

	// extra 는 칸마다 더하는 비용 (없으면 nullptr). 시작 · 목표 칸은 막혀 있어도 지난다
	bool AStar(const Grid& g, int32 start, int32 goal, const ValueNoise& noise, const vector<float>* extra, OUT vector<int32>& path)
	{
		path.clear();
		if (start < 0 || goal < 0 || start == goal)
			return false;

		const size_t count = g.walkable.size();
		vector<float> gScore(count, FLT_MAX);
		vector<int32> parent(count, -1);
		vector<uint8> closed(count, 0);
		auto h = [&](int32 idx)
			{
				const float dx = ::fabsf(static_cast<float>(idx % g.nx - goal % g.nx));
				const float dy = ::fabsf(static_cast<float>(idx / g.nx - goal / g.nx));
				return (std::max)(dx, dy) + 0.41421f * (std::min)(dx, dy);
			};
		using Node = pair<float, int32>;
		priority_queue<Node, vector<Node>, greater<Node>> open;
		gScore[start] = 0.f;
		open.emplace(h(start), start);

		const int32 di[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
		const int32 dj[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };

		while (open.empty() == false)
		{
			const auto [f, cur] = open.top();
			open.pop();
			if (closed[cur])
				continue;
			closed[cur] = 1;
			if (cur == goal)
				break;

			const int32 i = cur % g.nx;
			const int32 j = cur / g.nx;
			for (int32 k = 0; k < 8; k++)
			{
				const int32 ni = i + di[k];
				const int32 nj = j + dj[k];
				if (g.In(ni, nj) == false)
					continue;
				const int32 n = g.Index(ni, nj);
				if (closed[n])
					continue;
				if (n != goal && g.Passable(n) == false)
					continue;
				if (k >= 4)
				{
					// 대각선 - 모서리를 깎지 않는다 (양옆 칸도 지나갈 수 있어야)
					const int32 a = g.Index(i + di[k], j);
					const int32 b = g.Index(i, j + dj[k]);
					if (g.Passable(a) == false || g.Passable(b) == false)
						continue;
				}
				const float step = (k >= 4) ? 1.41421f : 1.f;
				const float cost = step * (1.f + noise.At(g, n) + (extra ? (*extra)[n] : 0.f));
				const float ng = gScore[cur] + cost;
				if (ng < gScore[n])
				{
					gScore[n] = ng;
					parent[n] = cur;
					open.emplace(ng + h(n), n);
				}
			}
		}

		if (parent[goal] == -1)
			return false;
		for (int32 cur = goal; cur != -1; cur = parent[cur])
			path.push_back(cur);
		reverse(path.begin(), path.end());
		return true;
	}

	/*-------- 정확 검사 --------*/

	// 점 하나 - 정적 규칙(inflate 만큼 띄워) + 동적(그 칸이 다른 길 때문에 막혀 있지 않음)
	bool PointOk(const GenContext& g, float x, float y, float inflate, bool checkMask)
	{
		if (StaticOk(g, x, y, inflate) == false)
			return false;
		if (checkMask == false)
			return true;
		const int32 idx = g.grid.CellOf(x, y);
		return idx >= 0 && g.grid.blocked[idx] == 0;
	}

	// checkMask - 중심선은 띠 마스크(칸)를 지켜야 하고, 두 줄은 정적 규칙만 본다 (띠 shrink 가 칸 여유 + 줄 오프셋을 이미 품었다)
	bool SegmentOk(const GenContext& g, const Pt& a, const Pt& b, float inflate, bool checkMask)
	{
		const WallMap& walls = *g.world->walls;
		const float len = Dist(a.first, a.second, b.first, b.second);
		if (len < 100.f)
			return false;		// 부팅 검사 "1 m 보다 짧다"
		if (walls.SweepBlocked(a.first, a.second, b.first, b.second, g.world->bodyRadiusCm + inflate))
			return false;
		const int32 samples = static_cast<int32>(len / SAMPLE_CM) + 1;
		for (int32 s = 0; s <= samples; s++)
		{
			const float t = static_cast<float>(s) / static_cast<float>(samples);
			if (PointOk(g, a.first + (b.first - a.first) * t, a.second + (b.second - a.second) * t, inflate, checkMask) == false)
				return false;
		}
		return true;
	}

	// 칸 길을 당겨 편다 - 선분마다 정확 검사. 이웃 칸 선분조차 안 되면 false
	bool Pull(const GenContext& g, const vector<int32>& cells, float inflate, OUT vector<Pt>& out)
	{
		out.clear();
		vector<Pt> pts;
		pts.reserve(cells.size());
		for (int32 idx : cells)
			pts.emplace_back(g.grid.Cx(idx), g.grid.Cy(idx));

		const int32 n = static_cast<int32>(pts.size());
		if (n < 2)
			return false;
		out.push_back(pts[0]);
		int32 i = 0;
		while (i < n - 1)
		{
			int32 pick = -1;
			for (int32 j = (std::min)(n - 1, i + MAX_PULL_SKIP); j > i; j--)
			{
				if (SegmentOk(g, pts[i], pts[j], inflate, true))
				{
					pick = j;
					break;
				}
			}
			if (pick == -1)
				return false;
			out.push_back(pts[pick]);
			i = pick;
		}
		return true;
	}

	// 고리 둘레 d 안의 칸에 표시
	template<typename Fn>
	void ForCellsNear(const Grid& g, const vector<Pt>& pts, bool closed, float d, Fn&& fn)
	{
		const size_t n = pts.size();
		const size_t segs = closed ? n : n - 1;
		for (size_t s = 0; s < segs; s++)
		{
			const Pt& a = pts[s];
			const Pt& b = pts[(s + 1) % n];
			const int32 i0 = (std::max)(0, static_cast<int32>(::floorf(((std::min)(a.first, b.first) - d - g.minX) / g.cell)));
			const int32 i1 = (std::min)(g.nx - 1, static_cast<int32>(::floorf(((std::max)(a.first, b.first) + d - g.minX) / g.cell)));
			const int32 j0 = (std::max)(0, static_cast<int32>(::floorf(((std::min)(a.second, b.second) - d - g.minY) / g.cell)));
			const int32 j1 = (std::min)(g.ny - 1, static_cast<int32>(::floorf(((std::max)(a.second, b.second) + d - g.minY) / g.cell)));
			for (int32 j = j0; j <= j1; j++)
			{
				for (int32 i = i0; i <= i1; i++)
				{
					const int32 idx = g.Index(i, j);
					if (SegmentDist(g.Cx(idx), g.Cy(idx), a.first, a.second, b.first, b.second) < d)
						fn(idx);
				}
			}
		}
	}

	// 지금 지날 수 있는 칸의 연결 성분 (A* 와 같은 8 방향 · 모서리 규칙). 앞서 정한 길이 섬을 조각내므로
	// 끝점 둘과 경유점은 같은 성분에서 고른다 - 그래야 길이 있다
	void LabelComponents(const Grid& g, OUT vector<int32>& comp, OUT vector<int32>& sizes)
	{
		comp.assign(g.walkable.size(), -1);
		sizes.clear();
		vector<int32> stack;
		int32 next = 0;
		const int32 di[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
		const int32 dj[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
		for (int32 idx = 0; idx < static_cast<int32>(g.walkable.size()); idx++)
		{
			if (g.Passable(idx) == false || comp[idx] != -1)
				continue;
			comp[idx] = next;
			sizes.push_back(0);
			stack.clear();
			stack.push_back(idx);
			while (stack.empty() == false)
			{
				const int32 cur = stack.back();
				stack.pop_back();
				sizes.back()++;
				const int32 i = cur % g.nx;
				const int32 j = cur / g.nx;
				for (int32 k = 0; k < 8; k++)
				{
					const int32 ni = i + di[k];
					const int32 nj = j + dj[k];
					if (g.In(ni, nj) == false)
						continue;
					const int32 n = g.Index(ni, nj);
					if (g.Passable(n) == false || comp[n] != -1)
						continue;
					if (k >= 4 && (g.Passable(g.Index(i + di[k], j)) == false || g.Passable(g.Index(i, j + dj[k])) == false))
						continue;
					comp[n] = next;
					stack.push_back(n);
				}
			}
			next++;
		}
	}

	/*-------- 길 하나 --------*/

	struct Candidate
	{
		vector<Pt>	loop;			// 닫힌 고리 (앞 줄 + 되돌아오는 줄)
		bool		crossing = false;
		uint32		coastEnds = 0;		// 길의 두 끝 가운데 해안 띠 칸인 것
		int32		startSector = -1;
		int32		endSector = -1;
		int32		newCover = 0;
		float		score = -FLT_MAX;		// -FLT_MAX = 후보 없음
		float		lengthCm = 0.f;
	};

	// 중심선을 d 만큼 옆으로 민 줄. 꼭짓점은 앞뒤 선분 법선의 이등분(miter), 너무 뾰족하면 2d 로 자른다
	vector<Pt> OffsetPolyline(const vector<Pt>& c, float d)
	{
		const size_t n = c.size();
		vector<Pt> out(n);
		auto dir = [&](size_t i) -> Pt
			{
				const float dx = c[i + 1].first - c[i].first;
				const float dy = c[i + 1].second - c[i].second;
				const float len = ::sqrtf(dx * dx + dy * dy);
				return (len > 0.001f) ? Pt{ dx / len, dy / len } : Pt{ 1.f, 0.f };
			};
		for (size_t i = 0; i < n; i++)
		{
			Pt nrm;
			if (i == 0 || i + 1 == n)
			{
				const Pt u = dir(i == 0 ? 0 : n - 2);
				nrm = { -u.second, u.first };
			}
			else
			{
				const Pt u1 = dir(i - 1);
				const Pt u2 = dir(i);
				const Pt n1{ -u1.second, u1.first };
				const Pt n2{ -u2.second, u2.first };
				float mx = n1.first + n2.first;
				float my = n1.second + n2.second;
				const float ml = ::sqrtf(mx * mx + my * my);
				if (ml < 0.001f)
					nrm = n1;
				else
				{
					mx /= ml; my /= ml;
					// 이등분 방향 길이 = d / cos(θ/2) = d / (ml / 2) - 2d 로 자른다
					const float scale = (std::min)(2.f, 2.f / ml);
					nrm = { mx * scale, my * scale };
				}
			}
			out[i] = { c[i].first + nrm.first * d, c[i].second + nrm.second * d };
		}
		return out;
	}

	bool LoopSelfIntersects(const vector<Pt>& loop)
	{
		const size_t n = loop.size();
		for (size_t i = 0; i < n; i++)
		{
			const Pt& a = loop[i];
			const Pt& b = loop[(i + 1) % n];
			for (size_t j = i + 2; j < n; j++)
			{
				if (i == 0 && j + 1 == n)
					continue;		// 이웃 선분
				const Pt& c = loop[j];
				const Pt& d = loop[(j + 1) % n];
				if (SegmentsIntersect(a.first, a.second, b.first, b.second, c.first, c.second, d.first, d.second))
					return true;
			}
		}
		return false;
	}

	enum class BuildFail : uint8 { None, Path, Pull, Lane };

	// 격자 길 → 중심선(당겨 편다, lane_gap/2 띄운 검사) → 두 줄(중심선 ± lane_gap/2) → 닫힌 고리 → 선분마다 정확 검사
	bool BuildCandidate(GenContext& g, int32 startCell, int32 endCell, int32 viaCell, const ValueNoise& noise, const vector<float>* extra,
		OUT Candidate& out, OUT BuildFail& why)
	{
		why = BuildFail::None;
		vector<int32> cells;
		if (viaCell >= 0)
		{
			vector<int32> a, b;
			if (AStar(g.grid, startCell, viaCell, noise, extra, OUT a) == false || AStar(g.grid, viaCell, endCell, noise, extra, OUT b) == false)
			{
				why = BuildFail::Path;
				return false;
			}
			cells = a;
			cells.insert(cells.end(), b.begin() + 1, b.end());
		}
		else if (AStar(g.grid, startCell, endCell, noise, extra, OUT cells) == false)
		{
			why = BuildFail::Path;
			return false;
		}

		const float half = g.rules->laneGapCm * 0.5f;
		vector<Pt> centre;
		if (Pull(g, cells, half, OUT centre) == false || centre.size() < 2)
		{
			why = BuildFail::Pull;
			return false;
		}

		// 가는 줄(+), 되돌아오는 줄(−, 거꾸로) - 끝에서 lane_gap 만큼 건너가 닫힌다
		const vector<Pt> fwd = OffsetPolyline(centre, half);
		vector<Pt> back = OffsetPolyline(centre, -half);
		reverse(back.begin(), back.end());
		out.loop = fwd;
		out.loop.insert(out.loop.end(), back.begin(), back.end());
		if (out.loop.size() < 3 || LoopSelfIntersects(out.loop))
		{
			why = BuildFail::Lane;
			return false;
		}
		out.lengthCm = 0.f;
		for (size_t i = 0; i < out.loop.size(); i++)
		{
			const Pt& a = out.loop[i];
			const Pt& b = out.loop[(i + 1) % out.loop.size()];
			if (SegmentOk(g, a, b, 0.f, false) == false)
			{
				why = BuildFail::Lane;
				return false;
			}
			out.lengthCm += Dist(a.first, a.second, b.first, b.second);
		}
		if (out.lengthCm < MIN_LOOP_LEN_CM)
		{
			why = BuildFail::Lane;
			return false;
		}
		return true;
	}

	// 띠 안의 (큰 성분) 칸 가운데 조건을 만족하는 것 하나를 무작위로. 없으면 -1
	template<typename Pred>
	int32 RandomCell(GenContext& g, const vector<int32>& comps, int32 comp, Pred&& pred)
	{
		vector<int32> pool;
		for (int32 idx = 0; idx < static_cast<int32>(comps.size()); idx++)
		{
			if (comps[idx] == comp && pred(idx))
				pool.push_back(idx);
		}
		if (pool.empty())
			return -1;
		uniform_int_distribution<size_t> d(0, pool.size() - 1);
		return pool[d(g.rng)];
	}

	// 성분 안에서 섬 중심에 가장 가까운 칸 k 개 가운데 하나 - 같은 쪽 길의 경유점 ("중간까지는 가까이")
	template<typename Pred>
	int32 NearCentreCell(GenContext& g, const vector<int32>& comps, int32 comp, size_t k, Pred&& pred)
	{
		vector<pair<float, int32>> pool;
		for (int32 idx = 0; idx < static_cast<int32>(comps.size()); idx++)
		{
			if (comps[idx] == comp && pred(idx))
				pool.emplace_back(Dist(g.grid.Cx(idx), g.grid.Cy(idx), g.centroid.first, g.centroid.second), idx);
		}
		if (pool.empty())
			return -1;
		sort(pool.begin(), pool.end());
		uniform_int_distribution<size_t> d(0, (std::min)(k, pool.size()) - 1);
		return pool[d(g.rng)].second;
	}

	// 횡단의 두 끝인가 (R18) - 직선 50 m 이상 떨어지고 그 직선의 가운데가 해안에서 30 m 넘게 안쪽. 아니면 같은 쪽 길(R6)로 간다
	bool IsCrossingPair(const GenContext& g, int32 a, int32 b)
	{
		const Grid& grid = g.grid;
		const float ax = grid.Cx(a), ay = grid.Cy(a), bx = grid.Cx(b), by = grid.Cy(b);
		if (Dist(ax, ay, bx, by) < MIN_CROSS_CM)
			return false;
		return g.world->walls->CoastGap((ax + bx) * 0.5f, (ay + by) * 0.5f) >= MIN_CROSS_INLAND_CM;
	}

	void Commit(GenContext& g, const Candidate& c)
	{
		ForCellsNear(g.grid, c.loop, true, g.rules->coverRadiusCm, [&](int32 idx) { g.grid.covered[idx] = 1; });
	}

	// 띠 밖을 지나는 표본 수 (100 cm 마다) - 후보 점수에서 뺀다. 이웃 띠의 땅을 먹는 길은 뒤로 밀린다
	int32 OutOfBand(const GenContext& g, const vector<Pt>& loop, const vector<float>& extra)
	{
		int32 n = 0;
		for (size_t i = 0; i < loop.size(); i++)
		{
			const Pt& a = loop[i];
			const Pt& b = loop[(i + 1) % loop.size()];
			const int32 samples = static_cast<int32>(Dist(a.first, a.second, b.first, b.second) / SAMPLE_CM) + 1;
			for (int32 k = 0; k < samples; k++)
			{
				const float t = static_cast<float>(k) / static_cast<float>(samples);
				const int32 idx = g.grid.CellOf(a.first + (b.first - a.first) * t, a.second + (b.second - a.second) * t);
				if (idx >= 0 && extra[idx] != 0.f)
					n++;
			}
		}
		return n;
	}

	int32 NewCover(const GenContext& g, const vector<Pt>& loop)
	{
		vector<uint8> seen(g.grid.walkable.size(), 0);
		int32 n = 0;
		ForCellsNear(g.grid, loop, true, g.rules->coverRadiusCm, [&](int32 idx)
			{
				if (seen[idx] == 0 && g.grid.walkable[idx] != 0 && g.grid.covered[idx] == 0)
				{
					seen[idx] = 1;
					n++;
				}
			});
		return n;
	}
}

/*-------- 공개 --------*/

pair<float, float> PatrolRouteGen::Centroid(const WallMap& walls)
{
	if (walls.HasOutline() == false)
		return { 0.f, 0.f };
	const vector<OutlinePoint>& o = walls.Outline();
	double area = 0.0, cx = 0.0, cy = 0.0;
	for (size_t i = 0; i < o.size(); i++)
	{
		const OutlinePoint& a = o[i];
		const OutlinePoint& b = o[(i + 1) % o.size()];
		const double f = static_cast<double>(a.x) * b.y - static_cast<double>(b.x) * a.y;
		area += f;
		cx += (a.x + b.x) * f;
		cy += (a.y + b.y) * f;
	}
	if (::fabs(area) < 1.0)
		return { 0.f, 0.f };
	area *= 0.5;
	return { static_cast<float>(cx / (6.0 * area)), static_cast<float>(cy / (6.0 * area)) };
}

float PatrolRouteGen::LoopLength(const PatrolRoute& route)
{
	float len = 0.f;
	for (size_t i = 0; i < route.points.size(); i++)
	{
		const Pt& a = route.points[i];
		const Pt& b = route.points[(i + 1) % route.points.size()];
		len += Dist(a.first, a.second, b.first, b.second);
	}
	return len;
}

float PatrolRouteGen::PointToRoute(float x, float y, const PatrolRoute& route)
{
	return PolylineDist(x, y, route.points, true);
}

float PatrolRouteGen::RouteDistance(const PatrolRoute& a, const PatrolRoute& b)
{
	float best = FLT_MAX;
	for (size_t i = 0; i < a.points.size(); i++)
	{
		const Pt& a0 = a.points[i];
		const Pt& a1 = a.points[(i + 1) % a.points.size()];
		for (size_t j = 0; j < b.points.size(); j++)
		{
			const Pt& b0 = b.points[j];
			const Pt& b1 = b.points[(j + 1) % b.points.size()];
			best = (std::min)(best, SegSegDist(a0.first, a0.second, a1.first, a1.second, b0.first, b0.second, b1.first, b1.second));
		}
	}
	return best;
}

bool PatrolRouteGen::ValidateRoute(const RouteGenWorld& world, const PackRules& rules, const PatrolRoute& route, OUT string& error)
{
	const size_t n = route.points.size();
	if (n < 3)
	{
		error = "route " + to_string(route.routeId) + " needs at least 3 points";
		return false;
	}
	for (size_t i = 0; i < n; i++)
	{
		const Pt& a = route.points[i];
		const Pt& b = route.points[(i + 1) % n];
		const string where = "route " + to_string(route.routeId) + " segment " + to_string(i);
		const float len = Dist(a.first, a.second, b.first, b.second);
		if (len < 100.f)
		{
			error = where + " is shorter than 1 m";
			return false;
		}
		if (world.walls->SweepBlocked(a.first, a.second, b.first, b.second, world.bodyRadiusCm))
		{
			error = where + " hits a server wall";
			return false;
		}
		const int32 samples = static_cast<int32>(len / SAMPLE_CM) + 1;
		for (int32 s = 0; s <= samples; s++)
		{
			const float t = static_cast<float>(s) / static_cast<float>(samples);
			const float x = a.first + (b.first - a.first) * t;
			const float y = a.second + (b.second - a.second) * t;
			if (world.walls->InsideFloor(x, y) == false || world.walls->CoastGap(x, y) < rules.coastMinCm - 1.f)
			{
				error = where + " runs closer than coast_min to the coast";
				return false;
			}
			for (const HouseDef& h : world.houses)
			{
				if (h.Distance(x, y) < rules.houseMinCm - 1.f)
				{
					error = where + " passes closer than house_min to house " + h.houseId;
					return false;
				}
			}
			for (const Pt& tw : world.towers)
			{
				if (Dist(x, y, tw.first, tw.second) < MonsterConfig::ROUTE_TOWER_MIN_DIST_CM - 1.f)
				{
					error = where + " passes closer than 20 m to a tower";
					return false;
				}
			}
			for (const Pt& e : world.entries)
			{
				if (Dist(x, y, e.first, e.second) < MonsterConfig::ENTRY_MIN_DIST_CM - 1.f)
				{
					error = where + " passes closer than 20 m to an entry point";
					return false;
				}
			}
		}
	}
	return true;
}

namespace
{
	constexpr uint32 MAX_ATTEMPTS = 10;		// 성공 BEST_OF 개를 모으려 섞어 시도하는 기본 횟수 - 하나라도 성공했으면 여기서 멈춘다
	constexpr uint32 HARD_MAX_ATTEMPTS = 30;	// 하나도 성공 못 했으면 파생 시드로 여기까지 이어 간다 - 운에 따라 부팅이 거부되는 길을 없앤다 (검증자 수정 10: 시도당 성공 ≈ 0.54)
	constexpr uint32 BEST_OF = 3;			// 성공한 배치 이만큼 가운데 횡단이 가장 많은 것

	bool GenerateOnce(const RouteGenWorld& world, const PackRules& rules, const vector<MonsterTypeDef>& types, uint64 seed, OUT RouteSet& out, OUT string& error);
}

bool PatrolRouteGen::Generate(const RouteGenWorld& world, const PackRules& rules, const vector<MonsterTypeDef>& types,
	uint64 seed, OUT RouteSet& out, OUT string& error)
{
	// 띠 배치는 섬 모양 · 집 · 타워 때문에 가끔 마지막 길 자리가 없다 - 같은 시드에서 정해진 순서로 섞어 다시 (결정적).
	// 성공한 배치 BEST_OF 개 가운데 횡단이 가장 많은 것(같으면 덮임이 큰 것)을 고른다 - "최대한 가로지르도록"
	const uint64 startUs = Utils::NowMicroseconds();
	string last;
	RouteSet best;
	uint32 successes = 0;
	uint32 attempt = 0;
	for (; attempt < HARD_MAX_ATTEMPTS && successes < BEST_OF && (attempt < MAX_ATTEMPTS || successes == 0); attempt++)
	{
		const uint64 sub = seed + static_cast<uint64>(attempt) * 0x9E3779B97F4A7C15ull;
		RouteSet trial;
		if (GenerateOnce(world, rules, types, sub, OUT trial, OUT last) == false)
			continue;
		successes++;
		if (best.packs.empty() || trial.stats.crossings > best.stats.crossings
			|| (trial.stats.crossings == best.stats.crossings && trial.stats.coveragePct > best.stats.coveragePct))
			best = move(trial);
	}
	if (best.packs.empty())
	{
		error = last + " (after " + to_string(attempt) + " attempts)";
		return false;
	}
	out = move(best);
	for (GeneratedPack& gp : out.packs)
		gp.def.route = &gp.route;		// move 뒤 자기 길을 가리키게
	out.seed = seed;
	out.stats.seed = seed;
	out.stats.attempts = attempt;
	out.stats.elapsedUs = Utils::NowMicroseconds() - startUs;
	return true;
}

namespace
{
	// 성분 comp 안 아무 데나 길 하나 - 해안 칸에서 100 m 넘게 떨어진 해안 칸으로(횡단), 안 되면 해안 → 섬 중심에 가장 가까운 칸 → 같은 쪽 해안 (sep_min 이상 떨어진),
	// 그것도 안 되면 왔던 길로. 넘침(R11)과 본체 밖 성분(R17)이 쓴다. tries 번 안에 못 찾으면 false (best 는 -FLT_MAX 그대로)
	bool PlaceAnywhere(GenContext& g, const vector<int32>& comps, int32 comp, const vector<float>& extra, uint32 tries, RouteGenStats& st, OUT Candidate& best)
	{
		const Grid& grid = g.grid;
		const PackRules& rules = *g.rules;
		for (uint32 t = 0; t < tries && best.score == -FLT_MAX; t++)
		{
			auto coastIn = [&](int32 idx) { return grid.coast[idx] != 0; };
			const int32 a = RandomCell(g, comps, comp, coastIn);
			if (a < 0)
				break;
			const float ax = grid.Cx(a), ay = grid.Cy(a);
			// 먼 해안 - a 에서 100 m 넘게 떨어진 건너편 해안 칸 (없으면 같은 쪽)
			auto farCoast = [&](int32 idx) { return grid.coast[idx] && Dist(grid.Cx(idx), grid.Cy(idx), ax, ay) >= 10000.f && IsCrossingPair(g, a, idx); };
			int32 b = RandomCell(g, comps, comp, farCoast);
			ValueNoise noise;
			noise.Build(grid, g.rng, NOISE_AMP);
			Candidate c;
			BuildFail why = BuildFail::None;
			bool ok = false;
			if (b >= 0 && b != a)
			{
				ok = BuildCandidate(g, a, b, -1, noise, &extra, OUT c, OUT why);
				c.crossing = ok;
				c.coastEnds = 2;
			}
			if (ok == false)
			{
				const int32 via = NearCentreCell(g, comps, comp, CENTRE_PICK, [](int32) { return true; });
				auto nearCoast = [&](int32 idx) { return grid.coast[idx] && Dist(grid.Cx(idx), grid.Cy(idx), ax, ay) >= rules.sepMinCm; };
				b = RandomCell(g, comps, comp, nearCoast);
				if (via >= 0 && via != a)
					ok = BuildCandidate(g, a, (b < 0) ? via : b, (b < 0) ? -1 : via, noise, &extra, OUT c, OUT why);
				c.crossing = false;
				c.coastEnds = 1u + ((b >= 0) ? 1u : 0u);
			}
			if (ok == false)
			{
				st.retries++;
				if (why == BuildFail::Path) st.failPath++;
				else if (why == BuildFail::Pull) st.failPull++;
				else st.failLane++;
				continue;
			}
			c.newCover = NewCover(g, c.loop);
			c.score = static_cast<float>(c.newCover);
			if (c.score > best.score)
				best = move(c);
		}
		return best.score != -FLT_MAX;
	}

bool GenerateOnce(const RouteGenWorld& world, const PackRules& rules, const vector<MonsterTypeDef>& types, uint64 seed, OUT RouteSet& out, OUT string& error)
{
	const uint64 startUs = Utils::NowMicroseconds();
	out = RouteSet();
	out.floorId = rules.floorId;
	out.seed = seed;

	if (world.walls == nullptr)
	{
		error = "route gen needs a wall map";
		return false;
	}

	// 종류 - 방식마다 우두머리(leader) · 기본(field) 하나 이상
	vector<const MonsterTypeDef*> leaders[2], members[2];
	for (const MonsterTypeDef& t : types)
	{
		const int32 k = (t.aggroKind == MonsterAggroKind::Detect) ? 0 : (t.aggroKind == MonsterAggroKind::Reactive) ? 1 : -1;
		if (k < 0)
			continue;
		if (t.tier == MonsterTier::Leader)
			leaders[k].push_back(&t);
		else if (t.tier == MonsterTier::Field)
			members[k].push_back(&t);
	}
	const uint32 reactiveCount = (rules.packCount * rules.reactivePct + 50) / 100;
	const uint32 detectCount = rules.packCount - reactiveCount;
	if ((detectCount > 0 && (leaders[0].empty() || members[0].empty())) || (reactiveCount > 0 && (leaders[1].empty() || members[1].empty())))
	{
		error = "monster_types.csv needs a leader and a field type for every aggro kind the pack rules ask for (detect " + to_string(detectCount)
			+ ", reactive " + to_string(reactiveCount) + ")";
		return false;
	}

	GenContext g;
	g.world = &world;
	g.rules = &rules;
	g.rng.seed(seed);
	g.centroid = PatrolRouteGen::Centroid(*world.walls);
	BuildGrid(g);
	Grid& grid = g.grid;

	uint32 walkable = 0;
	for (uint8 w : grid.walkable)
		walkable += w;
	const int32 minRouteCells = static_cast<int32>(MIN_ROUTE_LEN_CM / rules.cellCm);
	if (walkable < rules.packCount * static_cast<uint32>(minRouteCells))
	{
		error = "floor " + to_string(rules.floorId) + " has too little walkable ground for " + to_string(rules.packCount) + " packs (" + to_string(walkable) + " cells)";
		return false;
	}

	RouteGenStats& st = out.stats;
	st.components = g.components;
	st.droppedCells = g.droppedCells;
	vector<int32> comps, sizes;
	vector<float> extra(grid.walkable.size(), 0.f);
	fill(grid.blocked.begin(), grid.blocked.end(), 0);		// 막힌 칸 = 이미 놓은 고리의 sep_min 띠 (전부)

	// 0. 섬 본체에서 떨어진 가장 큰 성분 하나 (R17) - 집 · 진입 · 타워 원이 봉인한 땅(서쪽 lobe)에 길 하나. 더 작은 조각(타워 · 집 사이 30 m 주머니)은 두지 않는다
	LabelComponents(grid, OUT comps, OUT sizes);
	int32 mainComp = -1;
	for (size_t c = 0; c < sizes.size(); c++)
		if (mainComp < 0 || sizes[c] > sizes[mainComp]) mainComp = static_cast<int32>(c);
	{
		vector<int32> side;
		for (size_t c = 0; c < sizes.size(); c++)
			if (static_cast<int32>(c) != mainComp && sizes[c] >= minRouteCells) side.push_back(static_cast<int32>(c));
		sort(side.begin(), side.end(), [&](int32 a, int32 b) { return sizes[a] > sizes[b]; });
		const uint32 sideMax = SIDE_ROUTES_MAX;
		for (int32 comp : side)
		{
			if (st.sideRoutes >= sideMax || out.packs.size() + 1 >= rules.packCount)
				break;
			Candidate best;
			if (PlaceAnywhere(g, comps, comp, extra, rules.candidates * 3, st, OUT best) == false)
				continue;
			Commit(g, best);
			ForCellsNear(grid, best.loop, true, rules.sepMinCm + g.margin, [&](int32 idx) { grid.blocked[idx] = 1; });
			GeneratedPack gp;
			gp.route.routeId = 601 + static_cast<uint32>(out.packs.size());
			gp.route.floorId = rules.floorId;
			gp.route.points = best.loop;
			gp.crossing = best.crossing;
			gp.lengthCm = best.lengthCm;
			gp.startSector = rules.packCount + 1;		// 본체 밖 성분
			gp.endSector = rules.packCount + 1;
			gp.centreDistCm = PatrolRouteGen::PointToRoute(g.centroid.first, g.centroid.second, gp.route);
			gp.bandCentreMinCm = gp.centreDistCm;
			gp.coastEnds = best.coastEnds;
			out.packs.push_back(move(gp));
			st.sideRoutes++;
		}
	}
	const uint32 bands = rules.packCount - st.sideRoutes;

	// 1. 가로지르는 방향 - 층의 긴 축에 수직 ± 40° (시드마다 다르다). 띠는 그 방향을 따라 나란히 놓인다
	uniform_real_distribution<float> unit(0.f, 1.f);
	const bool longIsX = (static_cast<float>(grid.nx) * grid.cell) > (static_cast<float>(grid.ny) * grid.cell);
	const float crossDeg = (longIsX ? 90.f : 0.f) + (unit(g.rng) * 2.f - 1.f) * CROSS_TILT_DEG;
	const float dx = ::cosf(crossDeg * PI_F / 180.f);
	const float dy = ::sinf(crossDeg * PI_F / 180.f);
	const float nx = -dy;
	const float ny = dx;

	// 2. 본체(가장 큰 성분)의 칸을 띠 축(n)에 투영 - 칸 수 등분으로 bands 띠 (섬 끝의 뾰족한 꼬리는 끝 띠에 붙는다). 본체 밖 성분은 0 에서 끝났다
	vector<float> proj(grid.walkable.size(), 0.f);
	vector<float> sorted;
	sorted.reserve(walkable);
	for (int32 idx = 0; idx < static_cast<int32>(proj.size()); idx++)
	{
		proj[idx] = grid.Cx(idx) * nx + grid.Cy(idx) * ny;
		if (grid.walkable[idx] && comps[idx] == mainComp)
			sorted.push_back(proj[idx]);
	}
	sort(sorted.begin(), sorted.end());
	// 띠 경계 = 걸을 수 있는 칸 수의 등분위 - 집 · 타워 · 진입 지점으로 빽빽한 곳은 넓은 띠, 트인 곳은 좁은 띠 (길이 들어갈 땅이 같게).
	// 띠는 선호 영역이라 좁아도 된다 (A* 가 비용을 물고 벗어난다). 띠에 못 넣은 길은 맨 마지막에 섬 어디든 빈 자리에 둔다
	const float shrink = rules.sepMinCm * 0.5f + g.margin;		// 이웃 띠의 줄과 sep_min 이상 (칸 여유 + 줄 오프셋 포함)
	const float pmin = sorted.front();
	const float pmax = sorted.back();
	if (pmax - pmin < (2.f * shrink + grid.cell) * static_cast<float>(rules.packCount))
	{
		ostringstream os;
		os << "floor " << rules.floorId << ": " << rules.packCount << " packs with sep_min " << rules.sepMinCm << " cm need " << ((2.f * shrink + grid.cell) * rules.packCount) / 100.f
			<< " m across the floor, it has " << (pmax - pmin) / 100.f << " m";
		error = os.str();
		return false;
	}
	vector<float> bounds(bands + 1);
	for (uint32 i = 0; i <= bands; i++)
		bounds[i] = sorted[(std::min)(sorted.size() - 1, static_cast<size_t>(sorted.size()) * i / bands)];

	uint32 carry = 0;
	for (uint32 r = 0; r < bands; r++)
	{
		// 띠 = 선호 영역 (밖은 A* 비용 × (1 + OUT_BAND_COST)) - 집 · 타워 · 진입 지점을 돌아갈 때만 잠깐 벗어난다.
		// 겹치지 않음은 막힌 칸(앞서 놓은 고리의 sep_min)이 보장한다
		const float lo = (r == 0) ? -FLT_MAX : bounds[r] + shrink;
		const float hi = (r + 1 == bands) ? FLT_MAX : bounds[r + 1] - shrink;
		for (int32 idx = 0; idx < static_cast<int32>(proj.size()); idx++)
			extra[idx] = (proj[idx] < lo || proj[idx] > hi) ? OUT_BAND_COST : 0.f;
		const uint32 need = 1;		// 띠마다 하나 - 못 넣으면 맨 마지막 넘침 단계로
		uint32 placed = 0;

		for (uint32 q = 0; q < need; q++)
		{
			LabelComponents(grid, OUT comps, OUT sizes);

			// 띠 안 칸이 가장 많은 성분이 길의 터
			vector<int32> bandCells(sizes.size(), 0);
			for (int32 idx = 0; idx < static_cast<int32>(comps.size()); idx++)
				if (comps[idx] >= 0 && extra[idx] == 0.f) bandCells[comps[idx]]++;
			int32 comp = -1;
			for (size_t c = 0; c < sizes.size(); c++)
				if (comp < 0 || bandCells[c] > bandCells[comp]) comp = static_cast<int32>(c);
			if (comp < 0 || bandCells[comp] < minRouteCells)
				break;

			// 끝 = 띠 안의 해안 칸. d 투영으로 아래 해안 · 위 해안을 가른다. 한쪽뿐이면 같은 쪽 길, 아예 없으면 띠 안 칸의 양 끝
			float dmin = FLT_MAX, dmax = -FLT_MAX, cmin = FLT_MAX, cmax = -FLT_MAX, bandCentreMin = FLT_MAX;
			for (int32 idx = 0; idx < static_cast<int32>(comps.size()); idx++)
			{
				if (comps[idx] != comp || extra[idx] != 0.f)
					continue;
				const float d = grid.Cx(idx) * dx + grid.Cy(idx) * dy;
				dmin = (std::min)(dmin, d);
				dmax = (std::max)(dmax, d);
				if (grid.coast[idx])
				{
					cmin = (std::min)(cmin, d);
					cmax = (std::max)(cmax, d);
				}
				bandCentreMin = (std::min)(bandCentreMin, Dist(grid.Cx(idx), grid.Cy(idx), g.centroid.first, g.centroid.second));
			}
			const bool hasCoast = (cmin != FLT_MAX);
			const bool twoCoasts = hasCoast && (cmax - cmin >= 2.f * END_BAND_CM);
			auto dOf = [&](int32 idx) { return grid.Cx(idx) * dx + grid.Cy(idx) * dy; };
			auto inBand = [&](int32 idx) { return extra[idx] == 0.f; };
			auto lowCoast = [&](int32 idx) { return inBand(idx) && grid.coast[idx] && dOf(idx) <= cmin + END_BAND_CM; };
			auto highCoast = [&](int32 idx) { return inBand(idx) && grid.coast[idx] && dOf(idx) >= cmax - END_BAND_CM; };
			auto anyCoast = [&](int32 idx) { return inBand(idx) && grid.coast[idx] != 0; };
			auto lowEnd = [&](int32 idx) { return inBand(idx) && dOf(idx) <= dmin + END_BAND_CM; };
			auto highEnd = [&](int32 idx) { return inBand(idx) && dOf(idx) >= dmax - END_BAND_CM; };
			auto bandNearCentre = [&](int32 idx) { return inBand(idx); };

			Candidate best;
			if (twoCoasts || hasCoast == false)
			{
				// 횡단 - 아래 끝에서 위 끝으로
				for (uint32 t = 0; t < rules.candidates; t++)
				{
					const int32 a = twoCoasts ? RandomCell(g, comps, comp, lowCoast) : RandomCell(g, comps, comp, lowEnd);
					const int32 b = twoCoasts ? RandomCell(g, comps, comp, highCoast) : RandomCell(g, comps, comp, highEnd);
					if (a < 0 || b < 0 || a == b)
						continue;
					if (twoCoasts && IsCrossingPair(g, a, b) == false)
						continue;		// 같은 해안선의 두 칸 (끝 띠가 해안을 따라 놓인 경우) - 횡단이 아니다, 같은 쪽 길로
					ValueNoise noise;
					noise.Build(grid, g.rng, NOISE_AMP);
					Candidate c;
					BuildFail why = BuildFail::None;
					if (BuildCandidate(g, a, b, -1, noise, &extra, OUT c, OUT why) == false)
					{
						st.retries++;
						if (why == BuildFail::Path) st.failPath++;
						else if (why == BuildFail::Pull) st.failPull++;
						else st.failLane++;
						continue;
					}
					// 횡단 = 두 끝이 다 해안. 해안이 없는 띠(집 사이 안쪽 주머니)의 끝에서 끝은 횡단이 아니다 (pocket)
					c.coastEnds = (grid.coast[a] ? 1u : 0u) + (grid.coast[b] ? 1u : 0u);
					c.crossing = (c.coastEnds == 2);
					c.newCover = NewCover(g, c.loop);
					c.score = static_cast<float>(c.newCover) * (c.crossing ? 1.25f : 1.f) - static_cast<float>(OutOfBand(g, c.loop, extra)) * OUT_BAND_PENALTY;
					if (c.score > best.score)
						best = move(c);
				}
			}
			if (best.score == -FLT_MAX && hasCoast)
			{
				// 같은 쪽 - 해안에서 나와 띠 안에서 섬 중심에 가장 가까운 칸을 거쳐 해안의 다른 칸(sep_min 이상 떨어진)으로. 없으면 왔던 길을 되돌아간다
				for (uint32 t = 0; t < rules.candidates; t++)
				{
					const int32 a = RandomCell(g, comps, comp, anyCoast);
					const int32 via = NearCentreCell(g, comps, comp, CENTRE_PICK, bandNearCentre);
					if (a < 0 || via < 0 || via == a)
						continue;
					const float ax = grid.Cx(a), ay = grid.Cy(a);
					auto farCoast = [&](int32 idx) { return anyCoast(idx) && Dist(grid.Cx(idx), grid.Cy(idx), ax, ay) >= rules.sepMinCm; };
					int32 b = RandomCell(g, comps, comp, farCoast);
					if (b < 0)
						b = a;
					ValueNoise noise;
					noise.Build(grid, g.rng, NOISE_AMP);
					Candidate c;
					BuildFail why = BuildFail::None;
					if (BuildCandidate(g, a, (b == a) ? via : b, (b == a) ? -1 : via, noise, &extra, OUT c, OUT why) == false)
					{
						st.retries++;
						if (why == BuildFail::Path) st.failPath++;
						else if (why == BuildFail::Pull) st.failPull++;
						else st.failLane++;
						continue;
					}
					c.crossing = false;
					c.coastEnds = 1u + ((b != a) ? 1u : 0u);
					c.newCover = NewCover(g, c.loop);
					c.score = static_cast<float>(c.newCover) - static_cast<float>(OutOfBand(g, c.loop, extra)) * OUT_BAND_PENALTY;
					if (c.score > best.score)
						best = move(c);
				}
			}
			if (best.score == -FLT_MAX)
				break;

			Commit(g, best);
			ForCellsNear(grid, best.loop, true, rules.sepMinCm + g.margin, [&](int32 idx) { grid.blocked[idx] = 1; });
			GeneratedPack gp;
			gp.route.routeId = 601 + static_cast<uint32>(out.packs.size());
			gp.route.floorId = rules.floorId;
			gp.route.points = best.loop;
			gp.crossing = best.crossing;
			gp.lengthCm = best.lengthCm;
			gp.startSector = r;
			gp.endSector = r;
			gp.centreDistCm = PatrolRouteGen::PointToRoute(g.centroid.first, g.centroid.second, gp.route);
			gp.bandCentreMinCm = bandCentreMin;
			gp.coastEnds = best.coastEnds;
			out.packs.push_back(move(gp));
			placed++;
		}

		if (placed < need)
		{
			carry += need - placed;
			st.merged++;
		}
	}

	// 넘침 - 띠에 못 넣은 길은 섬 어디든 남은 빈 자리에 (PlaceAnywhere - 가장 큰 남은 성분)
	for (uint32 left = 0; left < carry; left++)
	{
		fill(extra.begin(), extra.end(), 0.f);
		LabelComponents(grid, OUT comps, OUT sizes);
		const int32 overflowCells = static_cast<int32>(OVERFLOW_MIN_LEN_CM / rules.cellCm);
		int32 comp = -1;
		for (size_t c = 0; c < sizes.size(); c++)
			if (sizes[c] >= overflowCells && (comp < 0 || sizes[c] > sizes[comp])) comp = static_cast<int32>(c);
		Candidate best;
		if (comp >= 0)
			PlaceAnywhere(g, comps, comp, extra, rules.candidates * 3, st, OUT best);
		if (best.score == -FLT_MAX)
		{
			error = "could not place patrol route " + to_string(out.packs.size() + 1) + " of " + to_string(rules.packCount) + " on floor " + to_string(rules.floorId)
				+ " (seed " + to_string(seed) + ", no free ground left after the bands, no path " + to_string(st.failPath) + " / pull " + to_string(st.failPull)
				+ " / lane " + to_string(st.failLane) + ") - loosen sep_min_cm or house_min_cm";
			return false;
		}
		Commit(g, best);
		ForCellsNear(grid, best.loop, true, rules.sepMinCm + g.margin, [&](int32 idx) { grid.blocked[idx] = 1; });
		GeneratedPack gp;
		gp.route.routeId = 601 + static_cast<uint32>(out.packs.size());
		gp.route.floorId = rules.floorId;
		gp.route.points = best.loop;
		gp.crossing = best.crossing;
		gp.lengthCm = best.lengthCm;
		gp.startSector = rules.packCount;		// 띠 번호 밖 = 넘침
		gp.endSector = rules.packCount;
		gp.centreDistCm = PatrolRouteGen::PointToRoute(g.centroid.first, g.centroid.second, gp.route);
		gp.bandCentreMinCm = gp.centreDistCm;
		gp.coastEnds = best.coastEnds;
		out.packs.push_back(move(gp));
		st.overflow++;
	}
	fill(grid.blocked.begin(), grid.blocked.end(), 0);
	st.crossDeg = crossDeg;
	// 무리 정의 - 반응형 reactive_pct, 어느 길이 반응형인지는 시드로
	vector<uint32> order(rules.packCount);
	for (uint32 i = 0; i < rules.packCount; i++)
		order[i] = i;
	shuffle(order.begin(), order.end(), g.rng);
	for (uint32 i = 0; i < rules.packCount; i++)
	{
		GeneratedPack& gp = out.packs[order[i]];
		const bool reactive = i < reactiveCount;
		const int32 k = reactive ? 1 : 0;
		const MonsterTypeDef* leader = leaders[k][uniform_int_distribution<size_t>(0, leaders[k].size() - 1)(g.rng)];
		const MonsterTypeDef* member = members[k][uniform_int_distribution<size_t>(0, members[k].size() - 1)(g.rng)];

		MonsterPackDef& d = gp.def;
		d.packId = gp.route.routeId;
		d.floorId = rules.floorId;
		d.routeId = gp.route.routeId;
		d.startSeq = uniform_int_distribution<uint32>(0, static_cast<uint32>(gp.route.points.size()) - 1)(g.rng);
		d.leaderTypeId = leader->typeId;
		d.memberTypeId = member->typeId;
		d.memberCount = rules.memberCount;
		d.spacingCm = rules.spacingCm;
		// 순찰 속도 ≤ 0.8 × 가장 느린 몸 (뒤처진 몸이 따라잡을 여유, MonsterConfig::PATROL_SPEED_MAX_RATIO)
		d.patrolSpeedCms = (std::min)(rules.patrolSpeedCms, MonsterConfig::PATROL_SPEED_MAX_RATIO * (std::min)(leader->moveSpeedCms, member->moveSpeedCms));
		d.respawnSeconds = rules.respawnS;
		d.kind = reactive ? MonsterAggroKind::Reactive : MonsterAggroKind::Detect;
		d.route = &gp.route;
	}

	// 통계 - 정확 검사도 여기서 (격자가 놓친 것이 있으면 실패)
	st.seed = seed;
	st.routes = static_cast<uint32>(out.packs.size());
	st.walkableCells = walkable;
	st.minSepCm = 0.f;
	float lenSum = 0.f;
	for (size_t i = 0; i < out.packs.size(); i++)
	{
		const GeneratedPack& a = out.packs[i];
		if (a.crossing) st.crossings++; else if (a.coastEnds == 0) st.pockets++; else st.sameSide++;
		lenSum += a.lengthCm;
		if (PatrolRouteGen::ValidateRoute(world, rules, a.route, OUT error) == false)
			return false;
		for (size_t j = i + 1; j < out.packs.size(); j++)
		{
			const float d = PatrolRouteGen::RouteDistance(a.route, out.packs[j].route);
			if (st.minSepCm == 0.f || d < st.minSepCm)
				st.minSepCm = d;
		}
	}
	if (out.packs.size() >= 2 && st.minSepCm < rules.sepMinCm - 1.f)
	{
		error = "generated routes come within " + to_string(static_cast<int32>(st.minSepCm)) + " cm of each other (sep_min " + to_string(static_cast<int32>(rules.sepMinCm)) + ")";
		return false;
	}
	uint32 coveredCells = 0;
	for (size_t idx = 0; idx < grid.walkable.size(); idx++)
		coveredCells += (grid.walkable[idx] != 0 && grid.covered[idx] != 0) ? 1 : 0;
	st.coveragePct = walkable ? 100.f * static_cast<float>(coveredCells) / static_cast<float>(walkable) : 0.f;
	st.avgLengthM = out.packs.empty() ? 0.f : lenSum / static_cast<float>(out.packs.size()) / 100.f;
	st.elapsedUs = Utils::NowMicroseconds() - startUs;
	return true;
}
}

string PatrolRouteGen::ToCsv(const RouteSet& set)
{
	ostringstream os;
	os << "route_id,floor_id,seq,x,y,kind,crossing\n";
	for (const GeneratedPack& gp : set.packs)
	{
		for (size_t i = 0; i < gp.route.points.size(); i++)
		{
			os << gp.route.routeId << ',' << gp.route.floorId << ',' << i << ',' << ::lroundf(gp.route.points[i].first) << ',' << ::lroundf(gp.route.points[i].second)
				<< ',' << MonsterTable::AggroKindName(gp.def.kind) << ',' << (gp.crossing ? 1 : 0) << '\n';
		}
	}
	return os.str();
}
