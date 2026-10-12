#pragma once
#include "MonsterTable.h"
#include "HouseTable.h"
#include "WallMap.h"
#include <random>

/*---------------------
	PatrolRouteGen
---------------------*/

// 무리 순찰 길 생성 (2026-10-12 사용자 지시 "맵의 몬스터 출현 로직" · design/quick-specs/pack-routes-2026-10-12.md).
//
// 층 하나에 무리 pack_count 개의 닫힌 순찰 고리를 만든다. 손으로 그린 patrol_routes.csv 대신 부팅 때마다 다르게 (시드).
//   띠           - 묶음마다 가로지르는 방향을 하나 정하고(긴 축에 수직 ± 40°, 시드) 층을 그 방향의 띠 pack_count 개로 나눈다.
//                  길 하나 = 띠 하나 → 길끼리 겹치지 않고(sep_min) 층 전체에 고르게 퍼진다
//   끝에서 끝    - 띠의 아래 끝에서 위 끝으로 (끝은 해안 띠의 칸). 길의 두 끝은 해안이다
//   가로지르기   - 띠가 집 · 타워로 끊겨 건너갈 수 없으면 같은 끝에서 나와 섬 중심에 가장 가까운 칸을 거쳐 돌아온다 ("중간까지는 가까이")
//   집 회피      - 집 볼륨에서 house_min, 타워 · 진입 지점 20 m, 해안 coast_min, 벽은 가장 큰 몸 + 20 cm 로 쓸어서
//   다르게       - 방향 · 잡음(A* 비용) · 끝 칸 · 출발 자리가 시드마다 다르다. 후보 candidates 개 중 새로 덮는 칸이 가장 많은 길
//   두 줄 고리   - 격자 길을 당겨 편 중심선을 ± lane_gap/2 로 밀어 가는 줄 · 되돌아오는 줄을 만들고 끝에서 잇는다. 우두머리가 자기 줄을 거슬러 걷지 않는다
//
// 순수 로직 - WallMap · 집 · 타워 · 진입 지점을 주입받는다. 같은 시드 = 같은 결과 (GameServerTests RouteGenTest).
// 판정은 격자(cell_cm)로 하고, 마지막에 선분마다 정확히 다시 검사한다 (ValidateRoute) - 격자가 놓친 얇은 벽은 여기서 잡힌다.

// Server/Data/pack_rules.csv — 층마다 한 줄. 없는 층은 CSV 무리(monster_packs.csv)를 그대로 쓴다
struct PackRules
{
	uint32	floorId = 0;
	uint32	packCount = 10;			// 무리 수
	uint32	reactivePct = 20;		// 반응형 비율 % (나머지 탐지형) - 2:8 = 20
	uint32	memberCount = 3;		// 우두머리 뒤 기본 몬스터 수
	float	spacingCm = 150.f;
	float	patrolSpeedCms = 170.f;
	uint32	respawnS = 1800;
	uint32	routeSets = 3;			// 부팅 때 만드는 길 묶음 수 - 방마다 하나를 고른다 (인스턴스끼리도 다르게)
	float	sepMinCm = 1200.f;		// 길끼리 최소 거리
	float	houseMinCm = 1500.f;	// 집 볼륨에서
	float	coastMinCm = 1000.f;	// 해안에서
	float	coverRadiusCm = 3000.f;	// 덮임 판정 · 고르게 퍼짐 점수
	float	cellCm = 200.f;			// 격자 칸
	uint32	candidates = 8;			// 길 하나당 후보 수
	float	laneGapCm = 300.f;		// 되돌아오는 줄과 가는 줄 사이 (중심선 ± 절반)
};

class PackRulesTable
{
public:
	// 파일이 없으면 규칙 0 (CSV 무리 그대로). 잘못된 행은 부팅 거부
	static bool		Load(const string& dataDir, uint32 floorCount, OUT string& error);
	static bool		Parse(const string& csv, uint32 floorCount, OUT string& error);
	static const PackRules*	Find(uint32 floorId);
	static const vector<PackRules>& All() { return s_rules; }

private:
	static vector<PackRules>	s_rules;
};

// 생성에 필요한 세계 - 테스트는 합성 벽 · 집으로 만든다
struct RouteGenWorld
{
	const WallMap*				walls = nullptr;
	vector<HouseDef>			houses;
	vector<pair<float, float>>	towers;
	vector<pair<float, float>>	entries;
	float						bodyRadiusCm = 75.f;	// 벽 쓸기 반지름 = 가장 큰 몸 + ROUTE_CLEARANCE_MARGIN_CM
};

struct RouteGenStats
{
	uint64	seed = 0;
	uint32	routes = 0;
	uint32	crossings = 0;			// 띠를 끝에서 끝까지 가로지른 길
	uint32	sameSide = 0;			// 띠가 끊겨 같은 끝으로 돌아온 길 (섬 중심에 가장 가까운 칸을 거친다)
	uint32	pockets = 0;			// 해안에 닿지 않는 안쪽 주머니의 길 (집 · 타워 사이)
	uint32	sideRoutes = 0;			// 섬 본체에서 떨어진 성분(집 · 진입 · 타워 원이 봉인한 땅)에 둔 길 (R17)
	uint32	components = 0;			// 길이 들어갈 만한 걸을 수 있는 성분 수 (본체 포함)
	uint32	droppedCells = 0;		// 너무 작아 버린 성분의 칸 수 (길 하나 분량 미만)
	float	crossDeg = 0.f;			// 가로지르는 방향 (도) - 긴 축에 수직 ± CROSS_TILT
	uint32	walkableCells = 0;
	float	coveragePct = 0.f;		// cover_radius 안에 든 걸을 수 있는 칸 비율
	float	minSepCm = 0.f;			// 길끼리 가장 가까운 거리 (0 = 길 1개 이하)
	float	avgLengthM = 0.f;		// 고리 길이 평균
	uint32	attempts = 1;			// 섞어 시도한 배치 수 (성공 BEST_OF 개를 모으거나 MAX_ATTEMPTS 까지)
	uint32	merged = 0;				// 길이 안 들어간 띠 수 (그만큼 넘침으로)
	uint32	overflow = 0;			// 띠 밖 빈 자리에 둔 길 수
	uint32	retries = 0;			// 후보 중 길을 못 찾은 수 (아래 셋의 합)
	uint32	failPath = 0;			// 격자에서 길이 없었다
	uint32	failPull = 0;			// 가는 줄을 당겨 펴다 정확 검사에 걸렸다
	uint32	failLane = 0;			// 두 줄(중심선 ± lane_gap/2)이 정확 검사에 걸렸다
	uint64	elapsedUs = 0;
};

struct GeneratedPack
{
	MonsterPackDef	def;			// route 는 이 구조체의 route 를 가리킨다 (RouteSet 안에서 고정)
	PatrolRoute		route;
	bool			crossing = false;
	float			lengthCm = 0.f;
	uint32			startSector = 0;	// = 띠 번호 (0 부터, 띠 축을 따라). pack_count = 넘침, pack_count + 1 = 본체 밖 성분 (R17)
	uint32			endSector = 0;
	float			centreDistCm = 0.f;	// 섬 중심에서 길까지
	float			bandCentreMinCm = 0.f;	// 이 띠 안에서 섬 중심에 가장 가까운 칸까지 - 같은 쪽 길은 여기까지 간다
	uint32			coastEnds = 0;		// 길의 두 끝 가운데 해안 띠에 있는 것 (횡단은 2)
};

struct RouteSet
{
	uint32					floorId = 0;
	uint64					seed = 0;
	vector<GeneratedPack>	packs;
	RouteGenStats			stats;
};

class PatrolRouteGen
{
public:
	// 길 묶음 하나. types = 무리에 쓸 수 있는 종류(우두머리 tier leader · 기본 tier field, aggro_kind detect/reactive).
	// 실패(무리 수만큼 길을 못 만듦 · 종류 없음)면 error 에 이유를 남기고 false
	static bool		Generate(const RouteGenWorld& world, const PackRules& rules, const vector<MonsterTypeDef>& types,
						uint64 seed, OUT RouteSet& out, OUT string& error);

	// 고리 하나를 정확히 검사 - 선분마다 벽 쓸기 · 해안 · 집 · 타워 · 진입 지점 (부팅 검사 ValidateRouteGeometry 의 상위 집합)
	static bool		ValidateRoute(const RouteGenWorld& world, const PackRules& rules, const PatrolRoute& route, OUT string& error);

	// 두 고리의 가장 가까운 거리 (선분 ↔ 선분)
	static float	RouteDistance(const PatrolRoute& a, const PatrolRoute& b);

	// 점 → 고리 거리
	static float	PointToRoute(float x, float y, const PatrolRoute& route);

	// 고리 길이 (닫는 선분 포함)
	static float	LoopLength(const PatrolRoute& route);

	// 섬 다각형 무게중심 (해안선이 없으면 (0, 0))
	static pair<float, float>	Centroid(const WallMap& walls);

	// 증거 · 사람이 보는 용 - route_id,floor_id,seq,x,y,kind,crossing
	static string	ToCsv(const RouteSet& set);
};
