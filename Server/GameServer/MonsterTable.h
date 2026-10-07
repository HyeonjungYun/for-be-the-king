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

enum class MonsterTier : uint8 { Field, House, Boss };

struct MonsterTypeDef
{
	uint32		typeId = 0;
	string		nameKey;
	MonsterTier	tier = MonsterTier::Field;

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
}

class MonsterTable
{
public:
	// 두 파일 모두 없어도 된다(몬스터 0). 자리 파일만 있으면 거부. 하나라도 잘못이면 이유를 error 에 남기고 false
	static bool		Load(const string& dataDir, uint32 floorCount, OUT string& error);

	// nullptr = 그 파일이 없다
	static bool		Parse(const string* typesCsv, const string* spawnsCsv, uint32 floorCount, OUT string& error);

	static const MonsterTypeDef*			FindType(uint32 typeId);
	static const vector<MonsterSpawnDef>&	Spawns() { return s_spawns; }
	static size_t							TypeCount() { return s_types.size(); }

	static const char*	TierName(MonsterTier tier);

private:
	static unordered_map<uint32, MonsterTypeDef>	s_types;
	static vector<MonsterSpawnDef>					s_spawns;
};
