#pragma once
#include "MonsterBrain.h"
#include <deque>

/*-----------------
	MonsterPack
-----------------*/

// 몬스터 무리 - 우두머리 1 + 기본 n 이 한 줄로 순찰 길을 걷고, 어그로를 함께 쓴다 (2026-10-11 사용자 지시 "몬스터 개선").
// MonsterBrain 처럼 Room 을 모르는 순수 로직이다 - 시계 · 플레이어 · 시선을 주입받아 결정만 돌려주고 Room(RoomPacks.cpp)이 적용한다.
// GameServerTests MonsterPackTest 가 주입 시계와 가짜 월드로 돌린다. 규칙 · 수치 근거: design/gdd/monster-ai.md § J.
//
//   순찰  - 머리(살아 있는 첫 몸)가 길의 점을 차례로 걷고 발자국을 남긴다. 뒤의 몸은 머리 발자국 위 rank × spacing 뒤를 밟는다 (한 줄)
//   교전  - 무리 전체가 한 목표를 쫓는다. 탐지형은 목표가 죽거나 떠날 때까지, 반응형은 처음 어그로된 자리에서 leash 를 벗어나면 끝
//   복귀  - 각자 어그로된 순간의 자리(앵커)로 돌아가 만피, 걷던 길을 마저 걷는다

// 머리 발자국 - 누적 호 길이(s)로 뒤의 몸이 "머리에서 d 뒤" 의 점을 찾는다
class PackTrail
{
public:
	void	Reset(float x, float y);
	void	Seed(const vector<pair<float, float>>& tailToHead);		// 태어날 때 - 줄 선 자리를 그대로 발자국으로
	void	Record(float x, float y);								// 머리의 지금 자리. TRAIL_STEP_CM 마다 점을 남긴다
	bool	PointAt(float s, OUT float& x, OUT float& y) const;		// [맨 앞 점, 머리] 로 자른다
	void	TruncateTo(float s);									// 머리가 바뀌었다 - 새 머리의 호 위치가 끝
	float	HeadS() const;
	void	SetMaxLength(float cm) { _maxLenCm = cm; }
	size_t	Size() const { return _pts.size(); }

private:
	struct P { float x = 0.f; float y = 0.f; float s = 0.f; };
	deque<P>	_pts;
	float		_headX = 0.f;
	float		_headY = 0.f;
	float		_maxLenCm = 1000.f;
};

enum class PackMode : uint8 { Patrol, Engage, Return };

const char* PackModeName(PackMode mode);

struct PackMember
{
	uint64	id = 0;				// 0 = 죽었다 (무리가 다시 태어날 때까지)
	float	anchorX = 0.f;		// 처음 어그로된 순간의 자리 - 복귀 지점
	float	anchorY = 0.f;
	bool	arrived = false;	// 복귀 도착
	uint64	returnDeadlineUs = 0;
};

struct PackState
{
	PackMode			mode = PackMode::Patrol;
	uint64				modeSinceUs = 0;
	uint64				lockoutUntilUs = 0;		// 태어난 뒤 · 복귀 뒤 잠깐 감지 · 피격 어그로를 무시한다
	uint64				targetId = 0;
	uint64				lastRetargetUs = 0;
	uint64				lastEngagedUs = 0;		// 누가 사거리 안이었거나 피해를 주고받은 마지막 시각
	uint64				allStuckSinceUs = 0;	// 0 = 누군가 움직이고 있다

	uint64				pendingAttackerId = 0;	// OnMemberDamaged 가 남기고 다음 Think 가 쓴다
	float				pendingAttackerX = 0.f;
	float				pendingAttackerY = 0.f;

	uint32				waypoint = 0;			// 머리가 향하는 길의 점
	uint64				cohesionWaitSinceUs = 0;
	float				spacingCm = 150.f;
	PackTrail			trail;
	vector<PackMember>	members;				// 줄 순서 - [0] 이 태어날 때의 우두머리
};

// Room 이 판단 틱마다 살아 있는 몸을 줄 순서로 넘긴다
struct PackMemberView
{
	uint32	index = 0;				// PackState::members 위치
	float	x = 0.f;
	float	y = 0.f;
	bool	windingUp = false;
	bool	casting = false;
	uint64	stuckAccumUs = 0;
	float	attackRangeCm = 150.f;
	float	aggroRadiusCm = 600.f;
	float	leashRadiusCm = 0.f;	// 0 = 끈 없음 (탐지형)
	float	giveUpS = 0.f;			// 0 = 포기하지 않는다
	float	moveSpeedCms = 300.f;
};

struct PackInput
{
	uint64									nowUs = 0;
	const MonsterPackDef*					def = nullptr;
	const vector<PackMemberView>*			living = nullptr;
	const vector<BrainPlayer>*				players = nullptr;
	LineClearFn								lineClear;
	const vector<pair<float, float>>*		noGo = nullptr;		// 타워 - 이 둘레(TOWER_NO_GO_CM) 안으로 쫓아 들어가지 않는다
};

struct PackOutput
{
	vector<BrainOutput>	perMember;					// *living 과 같은 순서 (교전 · 복귀 때만 의미 있다)
	uint64				aggroTargetId = 0;			// 이번 틱에 교전을 시작했다
	const char*			aggroBy = nullptr;			// "detect" · "attack"
	bool				retargeted = false;
	bool				enteredReturn = false;
	const char*			returnReason = nullptr;		// target_gone · leash · giveup · stuck
	bool				resumed = false;			// 복귀가 끝났다 - Room 이 살아 있는 몸을 만피로
	vector<uint32>		snapMembers;				// 복귀가 막혔다 - Room 이 (아무도 안 볼 때) 앵커로 옮긴다
};

class PackBrain
{
public:
	// 줄 선 자리 - 머리가 route[startSeq], 뒤의 몸은 길을 거슬러 spacing 간격. [0] = 머리
	static vector<pair<float, float>>	FilePositions(const PatrolRoute& route, uint32 startSeq, uint32 count, float spacingCm);

	static void			Spawn(PackState& p, const MonsterPackDef& def, const vector<uint64>& ids, const vector<pair<float, float>>& positions, uint64 nowUs);

	// 판단 틱 (100 ms)
	static PackOutput	Think(PackState& p, const PackInput& in);

	// 몸 하나가 맞았다 (평타 · 스킬) - 무리 전체의 일이다. 플레이어가 아닌 id 는 Think 가 버린다
	static void			OnMemberDamaged(PackState& p, uint64 attackerId, float attackerX, float attackerY, uint64 nowUs);

	// 몸 하나가 피해를 줬다
	static void			OnEngaged(PackState& p, uint64 nowUs);

	// 몸 하나가 죽었다. 머리였으면 다음 몸이 머리 - 발자국을 그 자리까지 자른다. 돌려주는 값 = 살아 있는 몸이 남았는가
	static bool			OnMemberDied(PackState& p, uint32 memberIndex, uint64 nowUs);

	// 순찰 한 걸음의 목표 (30 Hz). livingRank 0 = 머리. 머리면 길의 점이 바뀔 수 있다
	static BrainOutput	PatrolGoal(PackState& p, const MonsterPackDef& def, uint32 livingRank, float x, float y);

	static int32		HeadIndex(const PackState& p);		// 살아 있는 첫 몸, 없으면 -1
	static uint32		LivingCount(const PackState& p);
};
