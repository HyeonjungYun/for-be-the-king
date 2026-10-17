#include "pch.h"
#include "WallMap.h"
#include "FloorBounds.h"
#include <cmath>
#include <type_traits>

namespace
{
	constexpr double	PI = 3.14159265358979323846;
	constexpr float		GRID_ORIGIN_CM = -FloorBounds::HALF_CM;

	// 상자 기준 좌표 (로컬 X = 길이 방향, 로컬 Y = 두께 방향). 차를 double 로 구해 먼 층 좌표에서도 정밀도를 잃지 않는다
	struct LocalPoint
	{
		double	x;
		double	y;
	};

	LocalPoint ToLocal(const WallBox& w, float x, float y)
	{
		const double dx = static_cast<double>(x) - w.cx;
		const double dy = static_cast<double>(y) - w.cy;
		return { dx * w.ux + dy * w.uy, -dx * w.uy + dy * w.ux };
	}

	// 선분 p + t·d (t ∈ [0, 1]) 가 열린 축 정렬 상자 |x| < hx · |y| < hy 에 처음 들어가는 t. 없으면 NO_HIT
	float SegmentVsOpenBox(const LocalPoint& p, double dx, double dy, double hx, double hy)
	{
		double lo = 0.0;
		double hi = 1.0;

		// 축마다 |p + t·d| < h 인 t 의 열린 구간을 [lo, hi] 와 겹친다
		auto clip = [&lo, &hi](double pos, double d, double h)
		{
			if (d == 0.0)
				return std::fabs(pos) < h;		// 이 축으로는 움직이지 않는다 - 처음부터 안이어야 한다

			double t0 = (-h - pos) / d;
			double t1 = (h - pos) / d;
			if (t0 > t1)
				std::swap(t0, t1);

			lo = (std::max)(lo, t0);
			hi = (std::min)(hi, t1);
			return true;
		};

		if (clip(p.x, dx, hx) == false || clip(p.y, dy, hy) == false)
			return WallGeom::NO_HIT;

		// 열린 구간이라 lo == hi 는 닿기만 한 것이다 - 면을 따라 미끄러지기 · 끝점이 면에 닿기
		return (lo < hi) ? static_cast<float>(lo) : WallGeom::NO_HIT;
	}

	// 선분이 열린 원 |q - c| < r 에 처음 들어가는 t. 없으면 NO_HIT
	float SegmentVsOpenCircle(const LocalPoint& p, double dx, double dy, double cx, double cy, double r)
	{
		const double fx = p.x - cx;
		const double fy = p.y - cy;
		const double a = dx * dx + dy * dy;
		const double c = fx * fx + fy * fy - r * r;

		if (a == 0.0)
			return (c < 0.0) ? 0.f : WallGeom::NO_HIT;

		const double b = fx * dx + fy * dy;		// 근의 공식의 b / 2
		const double disc = b * b - a * c;
		if (disc <= 0.0)
			return WallGeom::NO_HIT;				// 스치기만 한다 - 안으로 들어가지 않는다

		const double s = std::sqrt(disc);
		const double lo = (std::max)((-b - s) / a, 0.0);
		const double hi = (std::min)((-b + s) / a, 1.0);

		return (lo < hi) ? static_cast<float>(lo) : WallGeom::NO_HIT;
	}

	// 층 밖 좌표는 가장자리 칸으로 - 벽은 모두 층 안에 있으므로 겹침을 놓치지 않는다
	int32 CellOf(float v)
	{
		const double cell = std::floor((static_cast<double>(v) - GRID_ORIGIN_CM) / WallMap::CELL_CM);
		if (cell < 0.0)
			return 0;
		if (cell > WallMap::GRID_N - 1)
			return WallMap::GRID_N - 1;
		return static_cast<int32>(cell);
	}

	const char* const KIND_NAMES[] = { "wall", "door_post", "pillar", "free_pillar", "cover", "tower", "boundary" };
	static_assert(size(KIND_NAMES) == static_cast<size_t>(WallKind::Boundary) + 1, "one name per WallKind");

	// 점에서 선분 a-b 까지 거리와 가장 가까운 점 (double - 해안선은 수백 m 라 float 차이가 cm 를 넘을 수 있다)
	double PointSegment(double px, double py, const OutlinePoint& a, const OutlinePoint& b, OUT double& qx, OUT double& qy)
	{
		const double vx = static_cast<double>(b.x) - a.x;
		const double vy = static_cast<double>(b.y) - a.y;
		const double l2 = vx * vx + vy * vy;
		double t = (l2 > 0.0) ? ((px - a.x) * vx + (py - a.y) * vy) / l2 : 0.0;
		t = (std::min)((std::max)(t, 0.0), 1.0);
		qx = a.x + vx * t;
		qy = a.y + vy * t;
		return std::sqrt((px - qx) * (px - qx) + (py - qy) * (py - qy));
	}

	// 다각형 안 (반직선 교차 - check_layout.py inside_outline 과 같은 판정). 면 위는 거리 0 이라 부르는 쪽이 따로 안으로 본다
	bool InsidePolygon(const vector<OutlinePoint>& poly, double px, double py)
	{
		bool inside = false;
		const size_t n = poly.size();
		for (size_t i = 0; i < n; i++)
		{
			const OutlinePoint& a = poly[i];
			const OutlinePoint& b = poly[(i + 1) % n];
			if ((a.y > py) != (b.y > py))
			{
				const double x = a.x + (py - a.y) * (static_cast<double>(b.x) - a.x) / (static_cast<double>(b.y) - a.y);
				if (px < x)
					inside = !inside;
			}
		}
		return inside;
	}

	// 두 선분이 끝점 말고 다른 곳에서 만나는가 (이웃 변은 부르는 쪽이 뺀다)
	bool SegmentsCross(const OutlinePoint& a, const OutlinePoint& b, const OutlinePoint& c, const OutlinePoint& d)
	{
		auto cross = [](const OutlinePoint& o, const OutlinePoint& p, const OutlinePoint& q)
		{
			return (static_cast<double>(p.x) - o.x) * (static_cast<double>(q.y) - o.y) - (static_cast<double>(p.y) - o.y) * (static_cast<double>(q.x) - o.x);
		};
		const double d1 = cross(c, d, a);
		const double d2 = cross(c, d, b);
		const double d3 = cross(a, b, c);
		const double d4 = cross(a, b, d);
		if (((d1 > 0.0 && d2 < 0.0) || (d1 < 0.0 && d2 > 0.0)) && ((d3 > 0.0 && d4 < 0.0) || (d3 < 0.0 && d4 > 0.0)))
			return true;

		// 한 끝점이 다른 선분 위 - 겹치거나 닿은 꼬임
		auto onSegment = [](const OutlinePoint& p, const OutlinePoint& q, const OutlinePoint& r)
		{
			return (std::min)(p.x, q.x) <= r.x && r.x <= (std::max)(p.x, q.x) && (std::min)(p.y, q.y) <= r.y && r.y <= (std::max)(p.y, q.y);
		};
		return (d1 == 0.0 && onSegment(c, d, a)) || (d2 == 0.0 && onSegment(c, d, b)) || (d3 == 0.0 && onSegment(a, b, c)) || (d4 == 0.0 && onSegment(a, b, d));
	}

	constexpr float		PUSH_EPS_CM = 0.5f;		// 밀어낸 점이 면 위에 걸치지 않게 - "벽 안" 은 열린 집합이다
	constexpr float		RING_STEP_CM = 10.f;
	constexpr int32		RING_DIRECTIONS = 32;
}

const char* WallKindName(WallKind kind)
{
	const size_t index = static_cast<size_t>(kind);
	return (index < size(KIND_NAMES)) ? KIND_NAMES[index] : "unknown";
}

bool ParseWallKind(const string& s, OUT WallKind& out)
{
	for (size_t i = 0; i < size(KIND_NAMES); i++)
	{
		if (s == KIND_NAMES[i])
		{
			out = static_cast<WallKind>(i);
			return true;
		}
	}

	return false;
}

void WallGeom::ComputeAxis(WallBox& w)
{
	const double rad = static_cast<double>(w.yawDeg) * PI / 180.0;
	double c = std::cos(rad);
	double s = std::sin(rad);

	// 90° 의 배수는 정확히 0 · ±1 로 - cos(90°) 가 6e-17 로 남으면 면 위의 점이 안 · 밖을 오간다
	if (std::fabs(c) < 1e-12)
		c = 0.0;
	if (std::fabs(s) < 1e-12)
		s = 0.0;

	w.ux = static_cast<float>(c);
	w.uy = static_cast<float>(s);
}

float WallGeom::SignedDistance(const WallBox& w, float x, float y)
{
	const LocalPoint p = ToLocal(w, x, y);
	const double qx = std::fabs(p.x) - w.halfLength;
	const double qy = std::fabs(p.y) - w.halfThickness;

	if (qx <= 0.0 && qy <= 0.0)
		return static_cast<float>((std::max)(qx, qy));		// 안 - 가장 가까운 면까지 (음수)

	const double ox = (std::max)(qx, 0.0);
	const double oy = (std::max)(qy, 0.0);
	return static_cast<float>(std::sqrt(ox * ox + oy * oy));
}

float WallGeom::Sweep(const WallBox& w, float ax, float ay, float bx, float by, float r)
{
	// 이미 겹쳤다 - 스폰 · 넉백 오차로 벽 안이거나, 벽면에 붙은 채 밀린다.
	// 더 파고들 때만 그 자리에서 막고, 나가거나 나란히 가면 이 벽은 보지 않는다 (AC-LM-04)
	const float overlapA = r - SignedDistance(w, ax, ay);
	if (overlapA > 0.f)
	{
		const float overlapB = r - SignedDistance(w, bx, by);
		return (overlapB > overlapA + EPS_CM) ? 0.f : NO_HIT;
	}

	const LocalPoint a = ToLocal(w, ax, ay);
	const LocalPoint b = ToLocal(w, bx, by);
	const double dx = b.x - a.x;
	const double dy = b.y - a.y;
	const double hl = w.halfLength;
	const double ht = w.halfThickness;

	if (r <= 0.f)
		return SegmentVsOpenBox(a, dx, dy, hl, ht);

	// r 만큼 부풀린 둥근 상자 = X 로 늘린 상자 ∪ Y 로 늘린 상자 ∪ 모서리 원 4개. 첫 진입은 그중 가장 이른 것
	float t = (std::min)(SegmentVsOpenBox(a, dx, dy, hl + r, ht), SegmentVsOpenBox(a, dx, dy, hl, ht + r));
	for (const double sx : { -hl, hl })
	{
		for (const double sy : { -ht, ht })
			t = (std::min)(t, SegmentVsOpenCircle(a, dx, dy, sx, sy, r));
	}

	return t;
}

bool WallMap::Build(vector<WallBox> walls, OUT string& error)
{
	_walls.clear();
	_cellStart.clear();
	_cellWalls.clear();

	if (walls.size() > static_cast<size_t>(MAX_WALLS))
	{
		error = to_string(walls.size()) + " walls, the limit is " + to_string(MAX_WALLS) + " per floor";
		return false;
	}

	struct CellRange
	{
		int32	x0, x1, y0, y1;
	};

	vector<CellRange> ranges;
	ranges.reserve(walls.size());
	vector<int32> counts(GRID_N * GRID_N, 0);

	for (size_t i = 0; i < walls.size(); i++)
	{
		WallBox& w = walls[i];
		const string where = "wall #" + to_string(i);

		if (std::isfinite(w.cx) == false || std::isfinite(w.cy) == false || std::isfinite(w.yawDeg) == false
			|| std::isfinite(w.halfLength) == false || std::isfinite(w.halfThickness) == false)
		{
			error = where + " has a non-finite value";
			return false;
		}

		if (w.halfLength <= 0.f || w.halfThickness <= 0.f)
		{
			error = where + " half_length and half_thickness must be > 0";
			return false;
		}

		WallGeom::ComputeAxis(w);

		// 외접 사각형 - 이것이 층 사각형 안이면 꼭짓점 넷도 안이다 (Core Rule 15)
		const float ex = std::fabs(w.ux) * w.halfLength + std::fabs(w.uy) * w.halfThickness;
		const float ey = std::fabs(w.uy) * w.halfLength + std::fabs(w.ux) * w.halfThickness;

		if (FloorBounds::Contains(w.cx - ex, w.cy - ey) == false || FloorBounds::Contains(w.cx + ex, w.cy + ey) == false)
		{
			error = where + " at (" + to_string(static_cast<int32>(w.cx)) + ", " + to_string(static_cast<int32>(w.cy))
				+ ") reaches outside the floor (±" + to_string(static_cast<int32>(FloorBounds::HALF_CM)) + " cm)";
			return false;
		}

		const CellRange range{ CellOf(w.cx - ex), CellOf(w.cx + ex), CellOf(w.cy - ey), CellOf(w.cy + ey) };
		for (int32 gy = range.y0; gy <= range.y1; gy++)
		{
			for (int32 gx = range.x0; gx <= range.x1; gx++)
				counts[gy * GRID_N + gx]++;
		}

		ranges.push_back(range);
	}

	// 칸별 목록을 한 배열에 이어 붙인다 (CSR) - 질의는 칸마다 연속된 한 구간만 읽는다
	_cellStart.assign(GRID_N * GRID_N + 1, 0);
	for (int32 cell = 0; cell < GRID_N * GRID_N; cell++)
		_cellStart[cell + 1] = _cellStart[cell] + counts[cell];

	_cellWalls.assign(_cellStart.back(), 0);
	vector<int32> next(_cellStart.begin(), _cellStart.end() - 1);

	for (size_t i = 0; i < ranges.size(); i++)
	{
		for (int32 gy = ranges[i].y0; gy <= ranges[i].y1; gy++)
		{
			for (int32 gx = ranges[i].x0; gx <= ranges[i].x1; gx++)
				_cellWalls[next[gy * GRID_N + gx]++] = static_cast<uint16>(i);
		}
	}

	_walls = move(walls);
	return true;
}

bool WallMap::SetOutline(vector<OutlinePoint> points, OUT string& error)
{
	if (points.empty())
	{
		_outline.clear();
		return true;
	}

	const size_t n = points.size();
	if (n < 3 || n > static_cast<size_t>(MAX_OUTLINE_POINTS))
	{
		error = "the coast needs 3 to " + to_string(MAX_OUTLINE_POINTS) + " points, got " + to_string(n);
		return false;
	}

	double twiceArea = 0.0;
	for (size_t i = 0; i < n; i++)
	{
		const OutlinePoint& p = points[i];
		if (FloorBounds::Contains(p.x, p.y) == false)
		{
			error = "coast point #" + to_string(i) + " is not a number inside the floor square (±" + to_string(static_cast<int32>(FloorBounds::HALF_CM)) + " cm)";
			return false;
		}

		const OutlinePoint& q = points[(i + 1) % n];
		if (p.x == q.x && p.y == q.y)
		{
			error = "coast points #" + to_string(i) + " and the next are the same point";
			return false;
		}
		twiceArea += static_cast<double>(p.x) * q.y - static_cast<double>(q.x) * p.y;
	}

	if (std::fabs(twiceArea) < 1.0)
	{
		error = "the coast has no area";
		return false;
	}

	// 꼬인 해안선은 안 · 밖이 뒤섞인다 - 이웃하지 않는 변끼리 만나면 거부 (n ≤ 512 라 n² 이면 충분)
	for (size_t i = 0; i < n; i++)
	{
		for (size_t j = i + 1; j < n; j++)
		{
			const bool neighbours = (j == i + 1) || (i == 0 && j == n - 1);
			if (neighbours == false && SegmentsCross(points[i], points[(i + 1) % n], points[j], points[(j + 1) % n]))
			{
				error = "the coast crosses itself (edges #" + to_string(i) + " and #" + to_string(j) + ")";
				return false;
			}
		}
	}

	_outline = move(points);
	return true;
}

bool WallMap::InsideFloor(float x, float y) const
{
	if (FloorBounds::Contains(x, y) == false)
		return false;
	if (_outline.empty())
		return true;

	return CoastGap(x, y) >= 0.f;
}

float WallMap::CoastGap(float x, float y) const
{
	if (std::isfinite(x) == false || std::isfinite(y) == false)
		return -std::numeric_limits<float>::infinity();

	if (_outline.empty())
		return FloorBounds::HALF_CM - (std::max)(std::fabs(x), std::fabs(y));

	double best = std::numeric_limits<double>::infinity();
	double qx = 0.0;
	double qy = 0.0;
	const size_t n = _outline.size();
	for (size_t i = 0; i < n; i++)
		best = (std::min)(best, PointSegment(x, y, _outline[i], _outline[(i + 1) % n], OUT qx, OUT qy));

	// 면 위(거리 0)는 안이다 - 반직선 판정이 어느 쪽으로 떨어지든
	return static_cast<float>((best == 0.0 || InsidePolygon(_outline, x, y)) ? best : -best);
}

void WallMap::PullInsideCoast(float& x, float& y, float inset) const
{
	if (_outline.empty() || CoastGap(x, y) >= inset)
		return;

	// 가장 가까운 해안 점 q - 바다면 q 를 지나 안으로, 안인데 너무 가까우면 q 에서 멀어지는 쪽으로 inset 만큼
	double best = std::numeric_limits<double>::infinity();
	double bx = x;
	double by = y;
	size_t bestEdge = 0;
	const size_t n = _outline.size();
	for (size_t i = 0; i < n; i++)
	{
		double qx = 0.0;
		double qy = 0.0;
		const double d = PointSegment(x, y, _outline[i], _outline[(i + 1) % n], OUT qx, OUT qy);
		if (d < best)
		{
			best = d;
			bx = qx;
			by = qy;
			bestEdge = i;
		}
	}

	// 안쪽 방향 - 점이 해안선 위에 있으면 변의 법선 중 다각형 안을 향하는 쪽
	const OutlinePoint& a = _outline[bestEdge];
	const OutlinePoint& b = _outline[(bestEdge + 1) % n];
	double nx = -(static_cast<double>(b.y) - a.y);
	double ny = static_cast<double>(b.x) - a.x;
	const double len = std::sqrt(nx * nx + ny * ny);
	if (len <= 0.0)
		return;
	nx /= len;
	ny /= len;
	if (InsidePolygon(_outline, bx + nx * 1.0, by + ny * 1.0) == false)
	{
		nx = -nx;
		ny = -ny;
	}

	x = static_cast<float>(bx + nx * inset);
	y = static_cast<float>(by + ny * inset);
}

// 사각형이 걸치는 칸의 벽을 모두 넘긴다. 두 칸에 걸친 벽은 두 번 넘어올 수 있다 - 결과(최솟값 · 하나라도)는 같다.
// fn 이 bool 을 돌려주면 true 에서 멈춘다 ("하나라도" 질의 - 시야 · 겹침. 예전엔 답이 나온 뒤에도 남은 칸을 다 돌았다, 2026-10-17)
template <typename Fn>
void WallMap::ForEachCandidate(float minX, float minY, float maxX, float maxY, Fn&& fn) const
{
	if (_walls.empty())
		return;

	const int32 x0 = CellOf(minX);
	const int32 x1 = CellOf(maxX);
	const int32 y0 = CellOf(minY);
	const int32 y1 = CellOf(maxY);

	for (int32 gy = y0; gy <= y1; gy++)
	{
		for (int32 gx = x0; gx <= x1; gx++)
		{
			const int32 cell = gy * GRID_N + gx;
			for (int32 k = _cellStart[cell]; k < _cellStart[cell + 1]; k++)
			{
				if constexpr (std::is_same_v<std::invoke_result_t<Fn, const WallBox&>, bool>)
				{
					if (fn(_walls[_cellWalls[k]]))
						return;
				}
				else
				{
					fn(_walls[_cellWalls[k]]);
				}
			}
		}
	}
}

bool WallMap::Overlaps(float x, float y, float r) const
{
	if (std::isfinite(x) == false || std::isfinite(y) == false || std::isfinite(r) == false)
		return true;

	r = (std::max)(r, 0.f);

	bool hit = false;
	ForEachCandidate(x - r, y - r, x + r, y + r, [&](const WallBox& w)
	{
		hit = WallGeom::SignedDistance(w, x, y) < r;
		return hit;
	});

	// r = 0 이면 위의 "< 0" 이 곧 "면 제외 안" 이다
	return hit;
}

float WallMap::SweepFirstHit(float ax, float ay, float bx, float by, float r, OUT WallKind* hitKind) const
{
	if (std::isfinite(ax) == false || std::isfinite(ay) == false || std::isfinite(bx) == false
		|| std::isfinite(by) == false || std::isfinite(r) == false)
		return 0.f;

	r = (std::max)(r, 0.f);

	float best = 1.f;
	const WallBox* bestWall = nullptr;

	ForEachCandidate((std::min)(ax, bx) - r, (std::min)(ay, by) - r, (std::max)(ax, bx) + r, (std::max)(ay, by) + r,
		[&](const WallBox& w)
		{
			const float t = WallGeom::Sweep(w, ax, ay, bx, by, r);
			if (t < best)
			{
				best = t;
				bestWall = &w;
			}
		});

	if (hitKind != nullptr && bestWall != nullptr)
		*hitKind = bestWall->kind;

	return best;
}

bool WallMap::SweepBlocked(float ax, float ay, float bx, float by, float r, OUT WallKind* hitKind) const
{
	return SweepFirstHit(ax, ay, bx, by, r, hitKind) < 1.f;
}

bool WallMap::SightBlocked(float ax, float ay, float bx, float by) const
{
	if (std::isfinite(ax) == false || std::isfinite(ay) == false || std::isfinite(bx) == false || std::isfinite(by) == false)
		return true;

	bool blocked = false;
	ForEachCandidate((std::min)(ax, bx), (std::min)(ay, by), (std::max)(ax, bx), (std::max)(ay, by), [&](const WallBox& w)
		{
			blocked = w.kind != WallKind::Cover && WallGeom::Sweep(w, ax, ay, bx, by, 0.f) < 1.f;
			return blocked;
		});

	return blocked;
}

bool WallMap::FindNearestFree(float x, float y, float r, float maxSearch, OUT float& outX, OUT float& outY) const
{
	if (std::isfinite(x) == false || std::isfinite(y) == false || std::isfinite(r) == false || std::isfinite(maxSearch) == false)
		return false;

	r = (std::max)(r, 0.f);
	maxSearch = (std::max)(maxSearch, 0.f);

	// 층 경계도 벽이다 - 원이 경계를 넘지 않는 범위 (ADR-0006 § ⑦ 넉백 · 대시와 같다)
	const float inset = FloorBounds::HALF_CM - r;
	if (inset <= 0.f)
		return false;

	// 섬이면 해안선도 경계다 - 바다 위 점은 해안선 안쪽 r 로 끌어온다 (2026-10-11). 오목한 해안에서는 끌어온 점이
	// 다른 변에 가까울 수 있어 consider 가 해안 거리를 한 번 더 본다
	auto clampToFloor = [this, inset, r](float& px, float& py)
	{
		px = (std::min)((std::max)(px, -inset), inset);
		py = (std::min)((std::max)(py, -inset), inset);
		PullInsideCoast(px, py, r + PUSH_EPS_CM);
	};

	bool found = false;
	float bestX = 0.f;
	float bestY = 0.f;
	float bestD2 = maxSearch * maxSearch;

	auto consider = [&](float px, float py)
	{
		clampToFloor(px, py);
		const float d2 = (px - x) * (px - x) + (py - y) * (py - y);
		if (d2 <= bestD2 && (_outline.empty() || CoastGap(px, py) >= r) && Overlaps(px, py, r) == false)
		{
			found = true;
			bestX = px;
			bestY = py;
			bestD2 = d2;
		}
	};

	// 1. 그 자리 - 층 밖이면 층 안의 가장 가까운 점. 비어 있으면 그것이 답이다
	float cx = x;
	float cy = y;
	clampToFloor(cx, cy);

	// 섬: 바다 위 목표는 해안까지 끌어온 점에서 잰다 - 바다로 쏜 블링크 · 순간이동이 실패하지 않고 해안에 내린다.
	// 사각형 층은 예전대로 원래 점에서 잰다 (경계 밖 maxSearch 넘게 떨어진 목표는 실패)
	if (_outline.empty() == false)
	{
		x = cx;
		y = cy;
	}
	consider(cx, cy);
	if (found && bestX == cx && bestY == cy)
	{
		outX = bestX;
		outY = bestY;
		return true;
	}

	// 2. 겹친 벽마다 그 벽 밖으로 밀어낸 점 - 면 넷 방향 + 모서리 원 방향. 벽 하나에 걸린 흔한 경우는 이것이 정확한 답이다
	ForEachCandidate(cx - r, cy - r, cx + r, cy + r, [&](const WallBox& w)
	{
		if (WallGeom::SignedDistance(w, cx, cy) >= r)
			return;

		const LocalPoint p = ToLocal(w, cx, cy);
		const double reachL = w.halfLength + r + PUSH_EPS_CM;
		const double reachT = w.halfThickness + r + PUSH_EPS_CM;

		auto toWorld = [&w](double l, double t)
		{
			return make_pair(static_cast<float>(w.cx + l * w.ux - t * w.uy), static_cast<float>(w.cy + l * w.uy + t * w.ux));
		};

		for (const auto& [l, t] : { make_pair(reachL, p.y), make_pair(-reachL, p.y), make_pair(p.x, reachT), make_pair(p.x, -reachT) })
		{
			const auto q = toWorld(l, t);
			consider(q.first, q.second);
		}

		// 모서리 바깥 - 가장 가까운 모서리에서 멀어지는 방향으로 r 만큼
		const double qx = (p.x >= 0.0) ? w.halfLength : -w.halfLength;
		const double qy = (p.y >= 0.0) ? w.halfThickness : -w.halfThickness;
		const double dx = p.x - qx;
		const double dy = p.y - qy;
		const double len = std::sqrt(dx * dx + dy * dy);
		if (std::fabs(p.x) > w.halfLength && std::fabs(p.y) > w.halfThickness && len > 0.0)
		{
			const auto q = toWorld(qx + dx / len * (r + PUSH_EPS_CM), qy + dy / len * (r + PUSH_EPS_CM));
			consider(q.first, q.second);
		}
	});

	// 3. 여러 벽이 맞물린 곳 - 밀어낸 점이 다른 벽에 걸렸거나 더 가까운 빈 곳이 있을 수 있다. 지금 답보다 가까운 고리만 훑는다
	const float limit = std::sqrt(bestD2);
	for (float s = RING_STEP_CM; s <= limit; s += RING_STEP_CM)
	{
		const bool hadBest = found;
		const float before = bestD2;
		for (int32 k = 0; k < RING_DIRECTIONS; k++)
		{
			const float a = 6.2831853f * k / RING_DIRECTIONS;
			consider(cx + s * std::cos(a), cy + s * std::sin(a));
		}
		if (found && (hadBest == false || bestD2 < before))
			break;		// 이 고리에서 찾았다 - 더 먼 고리는 볼 필요가 없다
	}

	if (found == false)
		return false;

	outX = bestX;
	outY = bestY;
	return true;
}

float WallMap::PenetrationDepth(float x, float y) const
{
	if (std::isfinite(x) == false || std::isfinite(y) == false)
		return 0.f;

	float depth = 0.f;
	ForEachCandidate(x, y, x, y, [&](const WallBox& w)
	{
		depth = (std::max)(depth, -WallGeom::SignedDistance(w, x, y));
	});

	return depth;
}

float WallMap::Clearance(float x, float y, float cap) const
{
	if (std::isfinite(x) == false || std::isfinite(y) == false)
		return -cap;

	float best = (std::min)(cap, FloorBounds::HALF_CM - (std::max)(std::fabs(x), std::fabs(y)));
	ForEachCandidate(x - cap, y - cap, x + cap, y + cap, [&](const WallBox& w)
	{
		best = (std::min)(best, WallGeom::SignedDistance(w, x, y));
	});

	return best;
}
