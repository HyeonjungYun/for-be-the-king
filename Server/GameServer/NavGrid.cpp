#include "pch.h"
#include "NavGrid.h"
#include "WallTable.h"
#include <cmath>

namespace
{
	constexpr float SQRT2 = 1.41421356f;

	// 한 스레드에 하나 - 창 크기 배열을 매번 할당하지 않는다. 세대 번호로 지우는 비용도 없앤다
	struct Scratch
	{
		vector<float>	g;
		vector<int32>	parent;
		vector<uint32>	seen;		// == gen 이면 g · parent 가 이번 탐색의 값
		vector<uint32>	closed;		// == gen 이면 닫혔다
		vector<pair<float, int32>>	heap;
		uint32			gen = 0;

		void Begin()
		{
			const size_t n = static_cast<size_t>(NavGrid::WINDOW_CELLS) * NavGrid::WINDOW_CELLS;
			if (g.size() != n)
			{
				g.assign(n, 0.f);
				parent.assign(n, -1);
				seen.assign(n, 0);
				closed.assign(n, 0);
			}
			if (++gen == 0)
			{
				fill(seen.begin(), seen.end(), 0);
				fill(closed.begin(), closed.end(), 0);
				gen = 1;
			}
			heap.clear();
		}
	};

	thread_local Scratch t_scratch;

	float Octile(int32 dx, int32 dy)
	{
		dx = abs(dx);
		dy = abs(dy);
		return (static_cast<float>(max(dx, dy)) + (SQRT2 - 1.f) * static_cast<float>(min(dx, dy))) * NavGrid::CELL_CM;
	}

	vector<unique_ptr<NavGrid>> s_grids;
}

int32 NavGrid::CellOf(float v)
{
	const int32 c = static_cast<int32>(::floorf((v + FloorBounds::HALF_CM) / CELL_CM));
	return (std::clamp)(c, 0, GRID_N - 1);
}

float NavGrid::CentreOf(int32 cell)
{
	return -FloorBounds::HALF_CM + (static_cast<float>(cell) + 0.5f) * CELL_CM;
}

bool NavGrid::Build(const WallMap& walls)
{
	_clear.clear();
	if (walls.Count() == 0)
		return false;

	_clear.assign(static_cast<size_t>(GRID_N) * GRID_N, 0);

	// 해안선 - 줄마다 변과의 교차로 안쪽 구간을 구한다 (칸마다 다각형 판정을 하면 36 만 × 꼭짓점 수다)
	const vector<OutlinePoint>& outline = walls.Outline();
	vector<float> xs;

	for (int32 gy = 0; gy < GRID_N; gy++)
	{
		const float y = CentreOf(gy);

		xs.clear();
		if (outline.empty() == false)
		{
			const size_t n = outline.size();
			for (size_t i = 0; i < n; i++)
			{
				const OutlinePoint& a = outline[i];
				const OutlinePoint& b = outline[(i + 1) % n];
				if ((a.y <= y) != (b.y <= y))
					xs.push_back(a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y));
			}
			sort(xs.begin(), xs.end());
		}

		size_t interval = 0;
		for (int32 gx = 0; gx < GRID_N; gx++)
		{
			const float x = CentreOf(gx);

			if (outline.empty() == false)
			{
				while (interval + 1 < xs.size() && x > xs[interval + 1])
					interval += 2;
				const bool inside = (interval + 1 < xs.size()) && x >= xs[interval] && x <= xs[interval + 1];
				if (inside == false)
					continue;
			}

			const float c = walls.Clearance(x, y, CLEARANCE_CAP_CM);
			if (c > 0.f)
				_clear[static_cast<size_t>(gy) * GRID_N + gx] = static_cast<uint16>((std::min)(c, CLEARANCE_CAP_CM));
		}
	}

	return true;
}

float NavGrid::ClearanceAt(float x, float y) const
{
	if (_clear.empty() || std::isfinite(x) == false || std::isfinite(y) == false || FloorBounds::Contains(x, y) == false)
		return 0.f;
	return static_cast<float>(_clear[static_cast<size_t>(CellOf(y)) * GRID_N + CellOf(x)]);
}

bool NavGrid::Walkable(float x, float y, float radiusCm) const
{
	return ClearanceAt(x, y) >= radiusCm + CLEARANCE_MARGIN_CM;
}

uint32 NavGrid::WalkableCells(float radiusCm) const
{
	const float need = radiusCm + CLEARANCE_MARGIN_CM;
	uint32 count = 0;
	for (uint16 c : _clear)
		count += (static_cast<float>(c) >= need) ? 1 : 0;
	return count;
}

bool NavGrid::WalkableCell(int32 gx, int32 gy, float need) const
{
	if (gx < 0 || gy < 0 || gx >= GRID_N || gy >= GRID_N)
		return false;
	return static_cast<float>(_clear[static_cast<size_t>(gy) * GRID_N + gx]) >= need;
}

bool NavGrid::Snap(int32& gx, int32& gy, float need) const
{
	if (WalkableCell(gx, gy, need))
		return true;

	int32 bestX = 0;
	int32 bestY = 0;
	int32 bestD2 = INT32_MAX;
	for (int32 dy = -SNAP_CELLS; dy <= SNAP_CELLS; dy++)
	{
		for (int32 dx = -SNAP_CELLS; dx <= SNAP_CELLS; dx++)
		{
			const int32 d2 = dx * dx + dy * dy;
			if (d2 < bestD2 && WalkableCell(gx + dx, gy + dy, need))
			{
				bestD2 = d2;
				bestX = gx + dx;
				bestY = gy + dy;
			}
		}
	}

	if (bestD2 == INT32_MAX)
		return false;

	gx = bestX;
	gy = bestY;
	return true;
}

NavGrid::PathResult NavGrid::FindPath(float fromX, float fromY, float toX, float toY, float radiusCm, const NavSweepFn& sweep) const
{
	PathResult result;
	if (IsBuilt() == false || std::isfinite(fromX) == false || std::isfinite(fromY) == false
		|| std::isfinite(toX) == false || std::isfinite(toY) == false)
	{
		result.failReason = "none";
		return result;
	}

	const float need = radiusCm + CLEARANCE_MARGIN_CM;

	int32 sx = CellOf(fromX);
	int32 sy = CellOf(fromY);
	int32 tx = CellOf(toX);
	int32 ty = CellOf(toY);

	// 창 - 출발과 도착의 중점에 맞춘다. 둘 다 안에 들어야 한다
	const int32 wx0 = (std::clamp)((sx + tx) / 2 - WINDOW_CELLS / 2, 0, GRID_N - WINDOW_CELLS);
	const int32 wy0 = (std::clamp)((sy + ty) / 2 - WINDOW_CELLS / 2, 0, GRID_N - WINDOW_CELLS);
	auto inWindow = [&](int32 gx, int32 gy) { return gx >= wx0 && gy >= wy0 && gx < wx0 + WINDOW_CELLS && gy < wy0 + WINDOW_CELLS; };

	if (inWindow(sx, sy) == false || inWindow(tx, ty) == false)
	{
		result.failReason = "window";
		return result;
	}

	if (Snap(sx, sy, need) == false || inWindow(sx, sy) == false)
	{
		result.failReason = "start";
		return result;
	}
	if (Snap(tx, ty, need) == false || inWindow(tx, ty) == false)
	{
		result.failReason = "goal";
		return result;
	}

	Scratch& s = t_scratch;
	s.Begin();

	auto local = [&](int32 gx, int32 gy) { return (gy - wy0) * WINDOW_CELLS + (gx - wx0); };
	auto heapLess = [](const pair<float, int32>& a, const pair<float, int32>& b) { return a.first > b.first; };	// 최소 힙

	const int32 startLi = local(sx, sy);
	const int32 goalLi = local(tx, ty);
	s.g[startLi] = 0.f;
	s.parent[startLi] = -1;
	s.seen[startLi] = s.gen;
	s.heap.push_back({ Octile(tx - sx, ty - sy), startLi });

	static constexpr int32 DX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
	static constexpr int32 DY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };

	bool reached = false;
	while (s.heap.empty() == false)
	{
		pop_heap(s.heap.begin(), s.heap.end(), heapLess);
		const int32 li = s.heap.back().second;
		s.heap.pop_back();

		if (s.closed[li] == s.gen)
			continue;
		s.closed[li] = s.gen;

		if (li == goalLi)
		{
			reached = true;
			break;
		}

		if (++result.expansions > MAX_EXPANSIONS)
			break;

		const int32 gx = wx0 + li % WINDOW_CELLS;
		const int32 gy = wy0 + li / WINDOW_CELLS;

		for (int32 k = 0; k < 8; k++)
		{
			const int32 nx = gx + DX[k];
			const int32 ny = gy + DY[k];
			if (inWindow(nx, ny) == false || WalkableCell(nx, ny, need) == false)
				continue;

			// 대각선은 옆 두 칸이 다 트여야 - 모서리를 깎지 않는다
			const bool diagonal = (k >= 4);
			if (diagonal && (WalkableCell(gx + DX[k], gy, need) == false || WalkableCell(gx, gy + DY[k], need) == false))
				continue;

			const int32 ni = local(nx, ny);
			if (s.closed[ni] == s.gen)
				continue;

			const float cost = s.g[li] + (diagonal ? SQRT2 : 1.f) * CELL_CM;
			if (s.seen[ni] == s.gen && cost >= s.g[ni])
				continue;

			s.seen[ni] = s.gen;
			s.g[ni] = cost;
			s.parent[ni] = li;
			s.heap.push_back({ cost + Octile(tx - nx, ty - ny), ni });
			push_heap(s.heap.begin(), s.heap.end(), heapLess);
		}
	}

	if (reached == false)
	{
		result.failReason = (result.expansions > MAX_EXPANSIONS) ? "budget" : "none";
		return result;
	}

	// 칸 중심 목록 (출발 칸 → 도착 칸)
	vector<pair<float, float>> cells;
	for (int32 li = goalLi; li != -1; li = s.parent[li])
		cells.emplace_back(CentreOf(wx0 + li % WINDOW_CELLS), CentreOf(wy0 + li / WINDOW_CELLS));
	reverse(cells.begin(), cells.end());
	cells.emplace_back(toX, toY);		// 마지막 = 실제 도착점

	// 당겨 펴기 - 지금 자리에서 막히지 않고 닿는 가장 먼 점까지 (처음 막히는 곳에서 멈춘다)
	float cx = fromX;
	float cy = fromY;
	size_t i = 0;
	while (i < cells.size())
	{
		size_t j = i;
		if (sweep != nullptr)
		{
			while (j + 1 < cells.size() && sweep(cx, cy, cells[j + 1].first, cells[j + 1].second, radiusCm) == false)
				j++;
		}
		else
		{
			j = cells.size() - 1;
		}

		// 첫 칸 중심조차 막혔다(벽에 붙어 출발) - 그 칸으로 한 걸음 가 본다. 걸음의 벽 쓸기가 미끄러뜨린다
		result.points.push_back(cells[j]);
		cx = cells[j].first;
		cy = cells[j].second;
		i = j + 1;
	}

	result.found = true;
	return result;
}

uint32 NavGridTable::Build(uint32 floorCount)
{
	s_grids.clear();
	s_grids.resize(floorCount);

	uint32 built = 0;
	for (uint32 floor = 0; floor < floorCount; floor++)
	{
		auto grid = make_unique<NavGrid>();
		if (grid->Build(WallTable::Get(floor)))
		{
			s_grids[floor] = move(grid);
			built++;
		}
	}
	return built;
}

const NavGrid* NavGridTable::Get(uint32 floorId)
{
	return (floorId < s_grids.size()) ? s_grids[floorId].get() : nullptr;
}

void NavGridTable::Clear()
{
	s_grids.clear();
}
