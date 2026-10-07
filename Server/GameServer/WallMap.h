#pragma once

/*-----------
	WallMap
-----------*/

// 층 하나의 서버 2D 벽 (docs/architecture/adr-0006-server-2d-walls.md · design/gdd/level-map.md Core Rule 14).
//
// 막는 모듈 하나 = 회전 상자 하나. 레벨 빌더가 내보낸 Server/Data/walls.csv 를 WallTable 이 읽어 층마다 만든다.
// Build 뒤에는 const 로만 쓴다 - 여러 Room 이 락 없이 함께 읽는다. 판 중에 벽은 바뀌지 않는다.
// 단위 cm, +X 북 · +Y 동. Z 는 없다 (level-map.md Core Rule 1 - 2).
//
// "벽 안" 은 열린 집합이다 - 면에 닿거나 면을 따라 미끄러지는 것은 막힘이 아니다 (AC-LM-04).

enum class WallKind : uint8 { Wall, DoorPost, Pillar, FreePillar, Cover, Tower };

// walls.csv 의 kind 열 이름 - wall · door_post · pillar · free_pillar · cover · tower
const char*	WallKindName(WallKind kind);
bool		ParseWallKind(const string& s, OUT WallKind& out);

struct WallBox
{
	WallKind	kind = WallKind::Wall;
	float		cx = 0.f;				// 중심 (cm)
	float		cy = 0.f;
	float		halfLength = 0.f;		// 로컬 X 반길이
	float		halfThickness = 0.f;	// 로컬 Y 반길이
	float		yawDeg = 0.f;			// 로컬 X 축 방향 (도)
	float		ux = 1.f;				// WallGeom::ComputeAxis 가 채운다 - 로컬 X 축 (cos, sin)
	float		uy = 0.f;
};

// 상자 하나에 대한 판정. WallMap 이 격자 후보마다 부르고, 테스트의 전수 검사 기준 구현도 이것을 쓴다.
namespace WallGeom
{
	constexpr float	NO_HIT = 2.f;		// Sweep 의 "안 닿는다" - 어떤 t 보다 크다
	constexpr float	EPS_CM = 0.1f;		// 벽 안에서 시작한 이동의 "겹침이 커졌다" 판정 여유

	void	ComputeAxis(WallBox& w);

	// 부호 거리 - 밖이면 상자까지 거리, 안이면 -(가장 가까운 면까지 거리), 면 위면 0
	float	SignedDistance(const WallBox& w, float x, float y);

	// a -> b 로 반지름 r 원을 밀 때 이 상자에 처음 들어가는 t ∈ [0, 1). 안 들어가면 NO_HIT.
	// a 에서 이미 겹쳤으면 (r - sd(a) > 0) b 의 겹침이 EPS_CM 넘게 커질 때만 0, 아니면 NO_HIT (ADR-0006 § ②)
	float	Sweep(const WallBox& w, float ax, float ay, float bx, float by, float r);
}

class WallMap
{
public:
	static constexpr int32	MAX_WALLS	= 2000;					// 층당 상한 - 넘으면 부팅 거부 (ADR-0006 § ④)
	static constexpr float	CELL_CM		= 800.f;				// 격자 칸 8 m
	static constexpr int32	GRID_N		= 35;					// 28,000 / 800
	static constexpr float	WALL_EPS_CM	= WallGeom::EPS_CM;

	// 실패하면 이유를 error 에 남기고 false. 맵은 빈 채로 둔다
	bool	Build(vector<WallBox> walls, OUT string& error);

	// (x, y) 중심 반지름 r 원이 벽 안과 겹치는가. r = 0 이면 점이 벽 안(면 제외)인가. 블링크 도착 · 부팅 9 m 검사
	bool	Overlaps(float x, float y, float r) const;

	// a -> b 로 반지름 r 원을 밀 때 처음 닿는 비율 t ∈ [0, 1]. 안 닿으면 1. hitKind 는 t < 1 일 때만 채운다.
	// 🔴 NaN · 무한대 좌표는 판정할 수 없어 0(막힘)이다 - 부르는 쪽이 먼저 FloorBounds 로 거른다
	float	SweepFirstHit(float ax, float ay, float bx, float by, float r, OUT WallKind* hitKind = nullptr) const;
	bool	SweepBlocked(float ax, float ay, float bx, float by, float r, OUT WallKind* hitKind = nullptr) const;

	// 점이 벽 안이면 가장 깊이 들어간 벽의 침투 깊이 (cm), 아니면 0. 테스트 · 로그용
	float	PenetrationDepth(float x, float y) const;

	int32					Count() const { return static_cast<int32>(_walls.size()); }
	const vector<WallBox>&	Walls() const { return _walls; }

private:
	template <typename Fn>
	void	ForEachCandidate(float minX, float minY, float maxX, float maxY, Fn&& fn) const;

	vector<WallBox>	_walls;
	vector<int32>	_cellStart;		// GRID_N² + 1 - 칸 i 의 벽 = _cellWalls[_cellStart[i] .. _cellStart[i + 1])
	vector<uint16>	_cellWalls;
};
