#pragma once
#include "ItemRoller.h"

/*-----------------
	MonsterTable
-----------------*/

// Server/Data/monster_types.csv · monster_spawns.csv — 몬스터 종류와 자리 (design/gdd/monster-ai.md § A).
// 부팅 때 한 번 읽고 이후 읽기 전용. 층 아이템 풀 · 스킬 · 진입 지점 · 서버 벽 · 타워 다음에 읽는다 -
// 자리마다 벽 겹침 · 진입 20 m · 타워 35 m 를 검사한다 (F8, level-map.md AC-LM-19 의 서버측 안전망).
//
// 🔴 monster_spawns.csv 가 없거나 층에 행이 없으면 그 층 몬스터 0 - G3 의 켬/끔 비교가 이 파일로 된다.

enum class MonsterTier : uint8 { Field, House, Boss, Leader };

// 어그로 방식 (2026-10-11 사용자 지시 "몬스터 개선").
//   guard    - 예전 몬스터: 집에 서 있다가 감지 · 피격으로 쫓고 끈(leash)이 끊기면 집으로 (집 지키는 병사 · 보스 · 테스트 더미)
//   detect   - 무리 · 플레이어 탐지형: 순찰하다 둘레의 플레이어를 보면 그 사람만 쫓는다 - 둘 중 하나가 죽을 때까지.
//              다른 플레이어가 무리를 때리면 그 사람으로 옮긴다
//   reactive - 무리 · 공격 반응형: 맞기 전엔 아무도 쫓지 않는다. 처음 어그로된 자리를 기억하고 거기서 leash 를 벗어나면 돌아가 순찰을 잇는다
enum class MonsterAggroKind : uint8 { Guard, Detect, Reactive };

struct MonsterTypeDef
{
	uint32		typeId = 0;
	string		nameKey;
	MonsterTier	tier = MonsterTier::Field;
	MonsterAggroKind aggroKind = MonsterAggroKind::Guard;	// monster_types.csv aggro_kind (열이 없으면 guard)

	int32		maxHp = 0;
	int32		attackPower = 0;
	float		attackSpeed = 1.f;
	float		attackRangeCm = 150.f;
	int32		armor = 0;
	int32		magicResist = 0;
	float		resistance = 0.f;

	float		moveSpeedCms = 0.f;
	float		radiusCm = 0.f;
	float		aggroRadiusCm = 0.f;
	float		leashRadiusCm = 0.f;
	float		searchS = 0.f;
	float		giveUpS = 0.f;
	float		assistRadiusCm = 0.f;
	bool		displaceImmune = false;

	uint32		skillId = 0;			// 0 = 없음. 몬스터 스킬은 9000번대 (skill-system Q5)

	// 원거리 (§ L, 2026-10-17 허깨비 궁수) - 0 = 근접. 목표가 kite 안이면 물러나고, keep 까지는 다가가 그 자리에서 쏜다
	float		kiteMinCm = 0.f;
	float		keepRangeCm = 0.f;

	bool		drops = false;			// drop_tier 가 none 이 아니다
	ChestTier	dropTier = ChestTier::Open;
	uint32		dropChancePct = 0;
	uint32		dropRolls = 0;
};

struct MonsterSpawnDef
{
	uint32		spawnId = 0;
	uint32		floorId = 0;
	uint32		typeId = 0;
	float		x = 0.f;				// 집(home) - 복귀 · 리스폰 자리
	float		y = 0.f;
	float		z = 0.f;
	float		yaw = 0.f;
	uint32		groupId = 0;			// 0 = 단독. 같은 값끼리 서로를 부른다
	uint32		respawnSeconds = 0;
};

// 순찰 길 (patrol_routes.csv) - 닫힌 고리. 마지막 점 → 첫 점이 이어진다. 가로 두 줄(2 ~ 4 m 떨어진)로 만들어 우두머리가 자기 줄을 거슬러 걷지 않게
struct PatrolRoute
{
	uint32						routeId = 0;
	uint32						floorId = 0;
	vector<pair<float, float>>	points;
};

// 무리 (monster_packs.csv) - 우두머리 1 + 기본 member_count. 한 길을 한 줄로 걷는다
struct MonsterPackDef
{
	uint32				packId = 0;
	uint32				floorId = 0;
	uint32				routeId = 0;
	uint32				startSeq = 0;
	uint32				leaderTypeId = 0;
	uint32				memberTypeId = 0;
	uint32				memberCount = 0;
	float				spacingCm = 0.f;		// 줄 간격 (앞 몸 중심 → 뒷 몸 중심)
	float				patrolSpeedCms = 0.f;
	uint32				respawnSeconds = 0;		// 무리가 전멸한 뒤 통째로
	MonsterAggroKind	kind = MonsterAggroKind::Detect;
	const PatrolRoute*	route = nullptr;
};

// 전역 노브 (monster-ai.md § Tuning Knobs). 종류별 값은 CSV 에 있다
namespace MonsterConfig
{
	constexpr float		BASE_PLAYER_SPEED_CMS	= 340.f;		// base_move_speed
	constexpr float		SPEED_CAP_RATIO			= 0.9f;			// monster_speed_cap_ratio - 이보다 빠르면 도망이 불가능하다
	constexpr float		MAX_SPEED_CMS			= BASE_PLAYER_SPEED_CMS * SPEED_CAP_RATIO;	// 306
	constexpr float		MAX_RADIUS_CM			= 150.f;		// 넘으면 문(3 m) · 기둥 틈(3.5 m)을 못 지난다
	constexpr float		ENTRY_MIN_DIST_CM		= 2000.f;		// monster_entry_min_dist
	constexpr float		TOWER_MIN_DIST_CM		= 3500.f;		// monster_tower_min_dist
	constexpr float		BOUNDARY_MIN_DIST_CM	= 1000.f;
	constexpr uint32	MIN_RESPAWN_S			= 60;
	constexpr uint32	SESSION_LENGTH_S		= 1800;			// 이보다 짧은 리스폰은 경고 - 한 런에 두 번 사냥

	constexpr uint64	AI_TICK_US				= 100'000;		// 판단 = UpdateTick
	constexpr uint64	MAX_STEP_US				= 100'000;		// 이동 한 걸음 상한 - 서버가 밀려도 튀지 않게
	constexpr float		RETARGET_RATIO			= 1.25f;
	constexpr uint64	LOS_LOSS_US				= 500'000;
	constexpr uint64	STUCK_US				= 2'000'000;
	constexpr float		STUCK_PROGRESS_CM		= 50.f;			// 1 초에 목표 쪽으로 이만큼은 가야 막힘이 아니다
	constexpr uint64	STUCK_WINDOW_US			= 1'000'000;
	constexpr uint64	REAGGRO_LOCKOUT_US		= 1'000'000;
	constexpr uint64	RETURN_TIMEOUT_US		= 10'000'000;
	constexpr uint64	CORPSE_LINGER_US		= 1'500'000;
	constexpr float		STOP_DIST_RATIO			= 0.8f;
	constexpr float		RETURN_SPEED_MULT		= 1.5f;
	constexpr float		ARRIVE_TOL_CM			= 50.f;
	constexpr float		SEARCH_ARRIVE_CM		= 100.f;
	constexpr float		SEARCH_REACQUIRE_MULT	= 1.5f;			// T6 - 감지 반경 × 1.5 안에서 다시 보면 추격
	constexpr int32		THREAT_MAX				= 32;
	constexpr uint64	SLIDE_STICKY_US			= 500'000;
	constexpr float		SKILL_TRIGGER_MARGIN_CM	= 50.f;			// 보스 광역 - 반경 - 50 안에 목표가 있을 때
	constexpr float		RESPAWN_DEFER_RADIUS_CM	= 1200.f;
	constexpr uint64	RESPAWN_DEFER_MAX_US	= 60'000'000;
	constexpr uint64	DROP_BAG_LIFETIME_US	= 600'000'000;
	constexpr float		DROP_FREE_RADIUS_CM		= 60.f;
	constexpr float		DROP_FREE_SEARCH_CM		= 300.f;

	// ── 무리 · 순찰 (2026-10-11 몬스터 개선) ──
	constexpr float		TRAIL_STEP_CM			= 25.f;			// 우두머리 발자국 간격 - 뒤따르는 몸이 이 점들을 밟는다
	constexpr float		WAYPOINT_ARRIVE_CM		= 50.f;			// 순찰 점에 이만큼 다가가면 다음 점 (멈추지 않는다)
	constexpr float		FOLLOW_GAIN_PER_S		= 2.f;			// 뒤처진 만큼 빨리 (자기 이동 속도까지)
	constexpr float		COHESION_SLOW_CM		= 300.f;		// 맨 뒤가 이만큼 처지면 우두머리가 반 속도
	constexpr float		COHESION_STOP_CM		= 600.f;		// 이만큼이면 우두머리가 기다린다
	constexpr uint64	COHESION_MAX_WAIT_US	= 5'000'000;	// 최대 기다림 - 넘으면 그냥 간다
	constexpr uint32	PACK_MAX_MEMBERS		= 5;			// 우두머리 뒤 기본 몬스터 최대 수
	constexpr float		ROUTE_TOWER_MIN_DIST_CM	= 2000.f;		// 순찰 길 ↔ 타워 (집 규칙 35 m 는 안쪽 줄이 지킬 수 없다)
	constexpr float		ROUTE_CLEARANCE_MARGIN_CM = 20.f;		// 길 검사 반지름 = 가장 큰 몸 + 이만큼
	constexpr float		PATROL_SPEED_MAX_RATIO	= 0.8f;			// 순찰 속도 ≤ 0.8 × 가장 느린 몸 - 뒤처진 몸이 따라잡을 여유
	constexpr uint64	PACK_LOS_LOSS_US		= 500'000;
	constexpr uint64	PACK_STUCK_GIVEUP_US	= 6'000'000;	// 무리 전원이 이만큼 막혀 있으면 돌아간다 (길찾기가 없다 - 벽을 영영 비비지 않게)
	constexpr uint64	RETURN_HARD_TIMEOUT_US	= 45'000'000;	// 돌아가는 길이 이보다 길면 (아무도 안 보는 때) 제자리로 옮긴다
	constexpr float		TOWER_NO_GO_CM			= 1200.f;		// 타워 둘레 - 무리는 이 안으로 들어가지 않는다 (목표가 안에 있으면 테두리까지)
	constexpr uint64	PACK_RETARGET_COOLDOWN_US = 0;			// 다른 사람이 때리면 바로 옮긴다 (사용자 결정 - 최신 공격자)

	// ── 길찾기 (§ K, 2026-10-17 - NavGrid A*) ──
	constexpr uint64	PATH_CHECK_US			= 200'000;		// 직선이 트였는지 다시 보는 주기 (5 Hz - 30 Hz 걸음마다 쓸지 않는다)
	constexpr float		PATH_GOAL_SLACK_CM		= 200.f;		// 목표가 길을 구한 때보다 이만큼 움직이면 다시 구한다
	constexpr uint64	PATH_MAX_AGE_US			= 2'000'000;	// 목표가 그대로여도 이만큼 지나면 다시 (임시 벽 · 밀려난 자리)
	constexpr uint64	PATH_RETRY_US			= 1'000'000;	// 실패한 뒤 다시 구하기까지 - 그동안은 예전처럼 직선 + 미끄러짐
	constexpr float		PATH_REACH_CM			= 40.f;			// 꺾는 점에 이만큼 다가가면 다음 점
	constexpr uint32	PATH_BUDGET_PER_STEP	= 4;			// 방마다 걸음(33 ms)당 길 계산 수 - 넘친 몸은 다음 걸음에

	// ── 몬스터 스킬 · 원거리 (§ L, 2026-10-17 - 멧돼지 돌진 · 허깨비 궁수) ──
	constexpr float		CHARGE_MIN_CM			= 250.f;		// 이보다 가까우면 돌진하지 않는다 (붙어 있으면 평타)
	constexpr float		RETREAT_STEP_CM			= 300.f;		// 원거리가 물러날 자리 - 지금 자리에서 이만큼
	constexpr float		RETREAT_LEASH_MARGIN_CM	= 100.f;		// 물러날 자리는 끈 반경보다 이만큼 안쪽 (물러나다 끈이 끊겨 집으로 가지 않게)
	constexpr float		RANGED_ENGAGE_RATIO		= 1.3f;			// 원거리는 keep × 이것 안에서 시야가 닿으면 교전 중 (T2 포기가 오지 않게)
	constexpr uint64	RETREAT_MAX_US			= 1'200'000;	// 한 번 물러나는 시간 - 플레이어(340)가 궁수(260)보다 빨라 무한히 물러나면 한 발도 못 쏜다 (2026-10-18 실측)
	constexpr uint64	RETREAT_COOLDOWN_US		= 2'500'000;	// 물러난 뒤 이만큼은 붙어 있어도 맞서 쏜다
}

struct RouteSet;		// PatrolRouteGen.h - 생성된 길 묶음

class MonsterTable
{
public:
	// 두 파일 모두 없어도 된다(몬스터 0). 자리 파일만 있으면 거부. 하나라도 잘못이면 이유를 error 에 남기고 false
	static bool		Load(const string& dataDir, uint32 floorCount, OUT string& error);

	// 생성 길 묶음 (2026-10-12 pack_rules.csv · PatrolRouteGen) - 규칙이 있는 층마다 route_sets 개를 만든다.
	// 그 층의 CSV 무리(monster_packs.csv)는 버린다 (로그). 집 · 규칙 · 벽 · 타워 · 진입 지점 · 종류를 먼저 읽어야 한다
	static bool		GenerateRouteSets(uint32 floorCount, uint64 bootSeed, OUT string& error);
	static size_t	RouteSetCount(uint32 floorId);
	static const RouteSet*	RouteSetAt(uint32 floorId, size_t index);
	static void		ClearRouteSets();
	static vector<MonsterTypeDef>	AllTypes();

	// nullptr = 그 파일이 없다
	static bool		Parse(const string* typesCsv, const string* spawnsCsv, uint32 floorCount, OUT string& error);

	// 순찰 길 · 무리 (Parse 다음에 - 종류를 본다). 둘 다 없으면 무리 0. 무리 파일만 있으면 거부
	static bool		ParsePacks(const string* routesCsv, const string* packsCsv, uint32 floorCount, OUT string& error);

	static const MonsterTypeDef*			FindType(uint32 typeId);
	static const vector<MonsterSpawnDef>&	Spawns() { return s_spawns; }
	static size_t							TypeCount() { return s_types.size(); }
	static const vector<MonsterPackDef>&	Packs() { return s_packs; }
	static const PatrolRoute*				FindRoute(uint32 routeId);
	static size_t							RouteCount() { return s_routes.size(); }

	static const char*	TierName(MonsterTier tier);
	static const char*	AggroKindName(MonsterAggroKind kind);

private:
	static unordered_map<uint32, MonsterTypeDef>	s_types;
	static vector<MonsterSpawnDef>					s_spawns;
	static unordered_map<uint32, PatrolRoute>		s_routes;		// 무리가 가리키므로 부팅 뒤 바뀌지 않는다
	static vector<MonsterPackDef>					s_packs;
	static unordered_map<uint32, vector<unique_ptr<RouteSet>>>	s_routeSets;	// 층 → 묶음들. 방이 가리키지 않고 복사한다
};
