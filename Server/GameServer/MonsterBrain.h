#pragma once
#include "MonsterTable.h"
#include <functional>

/*-----------------
	MonsterBrain
-----------------*/

// 몬스터 하나의 상태기 · 목표 선택 · 이동 한 걸음 (design/gdd/monster-ai.md § B · C · D).
// Room 을 모르는 순수 로직이다 - 시계 · 플레이어 목록 · 벽 질의를 주입받아 결정만 돌려주고, Room 이 적용한다.
// 그래서 GoogleTest 가 주입 시계와 가짜 월드로 돌린다 (MonsterBrainTest.cpp, AC-MA-10 ~).

enum class MonsterState : uint8 { Idle, Chase, Search, Return, Dead };

const char* MonsterStateName(MonsterState state);

struct ThreatEntry
{
	uint64	id = 0;
	float	threat = 0.f;
};

// 몬스터가 들고 다니는 AI 상태. MonsterBrain · MonsterMover 만 바꾼다
struct MonsterBrainState
{
	MonsterState		state = MonsterState::Idle;
	uint64				stateSinceUs = 0;
	uint64				targetId = 0;
	vector<ThreatEntry>	threat;					// 최대 THREAT_MAX - 시드 1 + 받은 피해 누적, 감쇠 없음

	float				lastSeenX = 0.f;		// 추격 중 마지막으로 시야가 닿았을 때 목표 위치
	float				lastSeenY = 0.f;
	uint64				losLostSinceUs = 0;		// 0 = 지금 보고 있다
	uint64				lastEngagedUs = 0;		// 피해를 주고받았거나 목표가 사거리 안이던 마지막 시각 (T2)
	uint64				lockoutUntilUs = 0;		// 이때까지 감지 · 피격 어그로 · 호출을 무시한다
	uint64				returnStartUs = 0;
	uint64				searchArrivedUs = 0;
	uint64				skillReadyAtUs = 0;

	// 피격 (T9) - OnDamaged 가 남기고 다음 Think 가 쓴다
	uint64				pendingAttackerId = 0;
	float				pendingAttackerX = 0.f;
	float				pendingAttackerY = 0.f;

	// 이동 - MonsterMover 가 쓴다
	uint64				stuckAccumUs = 0;		// T3 - 막힌 시간 누적
	uint64				windowStartUs = 0;		// 진척 측정 창 (1 초)
	float				windowProgressCm = 0.f;	// 창 안에서 목표 방향으로 간 거리
	int8				slideSign = 1;			// 직전에 비껴간 쪽 - 잠시 그쪽을 먼저 시도한다
	uint64				slideUntilUs = 0;
	float				slideDirX = 0.f;		// 직전에 비껴간 월드 방향 - 벽면을 따라 그대로 미끄러진다
	float				slideDirY = 0.f;
};

// 유효 목표 - 방 안 · 살아 있는 플레이어만 (끊긴 몸 포함, § 12). Room 이 틱마다 한 번 만든다
struct BrainPlayer
{
	uint64	id = 0;
	float	x = 0.f;
	float	y = 0.f;
};

// a -> b 점 시선이 벽에 안 막히는가 (WallMap::SweepBlocked(r = 0) 의 반대)
using LineClearFn = function<bool(float ax, float ay, float bx, float by)>;

struct BrainInput
{
	uint64						nowUs = 0;
	float						x = 0.f;
	float						y = 0.f;
	float						homeX = 0.f;
	float						homeY = 0.f;
	bool						windingUp = false;	// 평타 windup 중 - 목표를 바꾸지 않는다
	bool						casting = false;
	const vector<BrainPlayer>*	players = nullptr;
	LineClearFn					lineClear;
};

struct BrainOutput
{
	bool	hasGoal = false;
	float	goalX = 0.f;
	float	goalY = 0.f;
	float	stopDistCm = 0.f;
	float	speedMult = 1.f;		// 복귀 1.5

	uint64	attackTargetId = 0;		// 0 이 아니면 Room 이 HandleAttack(몬스터, 이 id) 를 부른다
	bool	castSkill = false;		// 보스 광역 - Room 이 BeginMonsterCast
	bool	aggroed = false;		// 이번 틱에 추격을 시작했다 - 무리 호출 (T10)
	bool	enteredReturn = false;
	bool	resetOnArrive = false;	// T11 · T12 - 만피 · CC 해제
	bool	snapHome = false;		// T12 - 집으로 순간 이동
};

class MonsterBrain
{
public:
	// 판단 틱 한 번 (100 ms). 전이표 § Core Rule 6 을 위에서부터 평가한다
	static BrainOutput	Think(MonsterBrainState& s, const MonsterTypeDef& def, const BrainInput& in);

	// 피해를 받은 순간 (평타 · 스킬 둘 다). 복귀 중에는 위협을 쌓지 않는다
	static void			OnDamaged(MonsterBrainState& s, uint64 attackerId, int32 damage, float attackerX, float attackerY, uint64 nowUs);

	// 이 몬스터가 피해를 줬다 (T2 교전 시각)
	static void			NoteEngaged(MonsterBrainState& s, uint64 nowUs);

	// 같은 무리 동료가 추격을 시작했다 (T10). Idle · 잠금 해제일 때만 따라간다
	static bool			Alert(MonsterBrainState& s, uint64 targetId, uint64 nowUs);

	static void			Kill(MonsterBrainState& s, uint64 nowUs);

	// 태어날 때 - 만피 Idle, 1 초 동안 감지하지 않는다 (스폰킬 방지 · Edge Cases)
	static void			Reset(MonsterBrainState& s, uint64 nowUs);

	// F3 거리 가중
	static float		DistanceWeight(const MonsterTypeDef& def, float distCm);

	static void			AddThreat(MonsterBrainState& s, uint64 id, float amount);
	static float		ThreatOf(const MonsterBrainState& s, uint64 id);
};

/*-----------------
	MonsterMover
-----------------*/

// 이동 한 걸음 (§ C-9) - 직선, 막히면 ±30 · 60 · 90° 로 비껴 가고, 모두 막히면 멈춘다. 길찾기는 없다.
// 막힘 = 1 초 창 안에서 목표 방향으로 간 거리(이동을 목표 방향에 투영)가 50 cm 미만. 2 초 쌓이면 T3.
// 🔴 "목표까지 거리가 줄었나" 로 재면 더 빠른 플레이어를 쫓을 때 매번 막힘이 된다 - 투영으로 잰다.

// a -> b 로 반지름 r 원을 밀 때 벽에 닿는가 (WallMap::SweepBlocked)
using SweepBlockedFn = function<bool(float ax, float ay, float bx, float by, float r)>;

struct StepInput
{
	uint64			nowUs = 0;
	uint64			dtUs = 0;			// 직전 걸음부터 - MAX_STEP_US 로 자른다
	float			x = 0.f;
	float			y = 0.f;
	float			goalX = 0.f;
	float			goalY = 0.f;
	float			stopDistCm = 0.f;
	float			speedCms = 0.f;		// 0 이면 서 있다 (CC · windup · 시전) - 막힘으로 세지 않는다
	float			radiusCm = 0.f;
	SweepBlockedFn	blocked;
};

struct StepOutput
{
	float	x = 0.f;
	float	y = 0.f;
	float	yawDeg = 0.f;
	bool	moved = false;
	bool	arrived = false;
};

class MonsterMover
{
public:
	static StepOutput	Step(MonsterBrainState& s, const StepInput& in);
};

/*----------------
	MonsterDrop
----------------*/

// 사망 드랍 (Core Rule 20) - drop_chance_pct 를 통과하면 그 풀로 RollChest 를 drop_rolls 번 부르고 합친다. 전부 isNew.
// 못 통과하거나 풀이 없으면 빈 목록 (가방을 만들지 않는다). RNG · id 발급은 주입 - AC-MA-50 이 시드를 고정해 센다
namespace MonsterDrop
{
	vector<ItemEntry>	Roll(const MonsterTypeDef& type, const ChestPool* pool, mt19937& rng, const InstanceIdIssuer& nextId);
}
