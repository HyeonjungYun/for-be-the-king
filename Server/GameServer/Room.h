#pragma once
#include "JobQueue.h"
#include "SkillTable.h"
#include "StatusEffects.h"
#include "WallMap.h"
#include "Vision.h"
#include "MonsterBrain.h"
#include "MonsterPack.h"
#include "EscapeLogic.h"
#include "KillBatch.h"
#include <random>

struct DeathSnapshot;

// 층의 탈출 타워 하나 (design/gdd/escape-timer.md § A · B)
struct EscapeTowerState
{
	string						towerId;			// CSV 문자열 (Ta …)
	uint32						wireId = 0;			// S_ESCAPE_GAUGE.tower_id - 층 안 순번 1..
	float						x = 0.f;
	float						y = 0.f;
	EscapeLogic::Gauge			gauge;
	Protocol::EscapeGaugeState	alertState = Protocol::ESCAPE_GAUGE_IDLE;	// 경보 집합이 마지막으로 받은 것 (FILLING / IDLE)
	vector<uint64>				lastOccupants;		// 직전 틱 점유자 - 역행(REVERSAL) 텔레메트리
};

// 테스트 모드 /t gear 한 줄 (design/quick-specs/test-mode-2026-10-11.md). 종류 0 = 그 칸은 그대로 둔다.
// 칸에 아이템이 있으면 그 아이템을 바꾸고(DB 행은 그대로 - 커밋은 종류 · 스킬을 쓰지 않는다), 없으면 isTest 아이템을 만든다
struct TestGearRequest
{
	uint32	weaponType = 0;
	uint32	weaponSkill1 = 0;		// Shift
	uint32	weaponSkill2 = 0;		// Q
	uint32	types[4] = {};			// 모자 · 갑옷 · 신발 · 장신구 (착용 칸 3 ~ 6)
	uint32	skills[4] = {};
	int32	grade = 0;				// 0 = 그대로, 1 ~ 4 일반 · 희귀 · 영웅 · 전설
	uint32	level = 0;				// 강화 레벨 (스킬 per_level)
};

// 몬스터 자리 하나 (monster-ai.md § States - Alive(id) ↔ DeadWaiting(respawnAtUs))
struct MonsterSlot
{
	const MonsterSpawnDef*	spawn = nullptr;
	uint64					monsterId = 0;		// 0 = 죽어서 기다린다
	uint64					respawnAtUs = 0;
	uint64					deferStartUs = 0;	// 곁에 플레이어가 있어 리스폰을 미룬 시작 (F9)
};

// 무리 하나 (2026-10-11 몬스터 개선 · RoomPacks.cpp). 무리 정의 · 길은 복사해 든다 - 테스트 모드 무리(/t pack)는 표에 없다.
// spawns 는 한 번 잡고 크기를 바꾸지 않는다 (Monster::spawn 이 가리킨다)
struct PackSlot
{
	MonsterPackDef			def;
	PatrolRoute				route;
	vector<MonsterSpawnDef>	spawns;
	PackState				state;
	uint64					respawnAtUs = 0;	// 전멸 뒤 통째로
	uint64					deferStartUs = 0;
	bool					wiped = false;
	bool					isTest = false;		// /t pack - 리스폰 없음, 주인이 떠나면 치운다
	uint64					testOwnerId = 0;
};

struct PendingTelegraph
{
	uint64 casterId = 0;
	uint32 skillId = 0;
	uint64 fireAtUs = 0;

	uint64 targetId = 0;
	float originX = 0.f;
	float originY = 0.f;
	float aimX = 0.f;
	float aimY = 0.f;
	uint32 itemLevel = 0;
	bool targetsOnly = false;		// 벽력일창 - 도약은 이미 시작했고 내리찍기(대상 효과)만 뒤에
	float bonusDamageMod = 0.f;		// 건곤일척 - 발동 때 소모한 배율 (예고 뒤에도 그대로)
};

// 핏자국 (4 단계) - 과다 출혈 중인 적이 남긴다. 주인이 밟으면 이속 버프, 아니면 지속 뒤 사라진다. 클라는 S_ZONE "blood" 로 그린다
struct TrailPoint
{
	uint64 zoneId = 0;
	uint64 ownerId = 0;
	float x = 0.f;
	float y = 0.f;
	uint64 expiresUs = 0;
	float buffMagnitude = 0.f;
	uint32 skillId = 0;
};

// 다단 히트의 남은 타 - hits × interval_ms (skill-roster 1 단계). 첫 타는 발동 순간, 나머지는 간격마다 FlushCombat 이 넣는다.
// 사거리를 다시 재지 않는다 - 대상이 살아 있고 시전자가 칠 수 있으면 들어간다 (TEMP: 연타 중 이탈은 2 단계 투사체와 함께 본다)
struct PendingHit
{
	uint64 casterId = 0;
	uint64 targetId = 0;
	uint32 skillId = 0;
	uint32 effectIndex = 0;
	uint32 remaining = 0;
	uint64 nextAtUs = 0;
	float bonusDamageMod = 0.f;		// 건곤일척 - 첫 타와 같은 배율
};

// 스킬 효과를 적용할 때의 문맥 - 발동 시점에 고정한 값들
struct SkillContext
{
	uint64 casterId = 0;
	float originX = 0.f;
	float originY = 0.f;
	float aimX = 0.f;
	float aimY = 0.f;
	uint32 itemLevel = 0;		// 버프 크기 - 시전 슬롯 아이템의 강화 레벨
	bool recast = false;		// 재입력의 두 번째 입력 - requires=recast 효과만 나간다
	bool targetsOnly = false;	// 투사체 적중 - 자기 효과는 발사 때 이미 나갔다, 대상 효과만
	Protocol::EquipSlot slot = Protocol::SLOT_NONE;		// 시전 슬롯 - 유지형이 끝날 때 쿨다운을 건다
	uint64 targetId = 0;		// 대상 지정 스킬의 대상 (그림자 밟기 - 등 뒤로)
	float bonusDamageMod = 0.f;	// 건곤일척 - 제자리에서 쓴 망치 스킬 한 번의 피해 배율 (발동 때 버프를 소모하고 여기 싣는다)
};

// 장판 (3 단계) - 바닥 원. 틱마다 안에 있는 적에게 틱 피해 · 소프트 CC · (스킬의) 지속 피해 갱신. 겹치면 각각 틱 = 피해 중첩 (사용자 결정)
struct Zone
{
	uint64				id = 0;
	uint64				casterId = 0;
	uint32				skillId = 0;
	uint32				effectIndex = 0;
	float				x = 0.f;
	float				y = 0.f;
	float				radiusCm = 0.f;
	uint64				endUs = 0;
	uint64				nextTickUs = 0;
	uint32				intervalMs = 0;
	float				tickBaseDamage = 0.f;		// 놓을 때 시전자 힘으로 고정 - 시전자가 떠나도 틱은 계속
	Protocol::DamageType tickType = Protocol::DAMAGE_TYPE_MAGIC;
	Protocol::CcType	ccType = Protocol::CC_TYPE_NONE;
	float				ccMagnitude = 0.f;
	vector<DotInstance>	dotTemplates;				// 스킬의 dot 효과 - 안에 있는 동안 틱마다 갱신 (중첩 아님)
	uint32				dotDurationMs = 0;
	string				param;
	uint64				groupId = 0;				// 같은 발동이 깐 장판 묶음 (열화 경로) - 한 틱에 한 대상은 묶음에서 한 번만 맞는다. 0 = 혼자
	uint64				teamId = 0;					// 놓은 사람의 팀 - 시전자가 떠난 뒤에도 아군을 맞히지 않는다 (PVE 협동, ADR-0014)

	// 부채꼴 장판 (네불라 아르카나 2026-10-11) - 원점에서 dir 쪽으로 반각 cos 안. 아니면 원
	bool				cone = false;
	float				dirX = 1.f;
	float				dirY = 0.f;
	float				halfCos = 1.f;
	float				angleDeg = 0.f;

	bool Contains(float px, float py) const
	{
		const float dx = px - x;
		const float dy = py - y;
		const float d2 = dx * dx + dy * dy;
		if (d2 > radiusCm * radiusCm)
			return false;
		if (cone == false || d2 < 1.f)
			return true;
		return (dx * dirX + dy * dirY) / ::sqrtf(d2) >= halfCos;
	}
};

// 효과가 끝날 때 거는 자기 CC (오버로드 · 니힐 "속성 없이 쓰면 끝날 때 기절", 2026-10-11 - 예전엔 즉시 걸렸다)
struct PendingSelfCc
{
	uint64				casterId = 0;
	uint32				skillId = 0;
	Protocol::CcType	type = Protocol::CC_TYPE_NONE;
	uint32				durationMs = 0;
	float				magnitude = 0.f;
	uint64				atUs = 0;
};

// 지나간 길에만 장판 (열화 2026-10-11) - 돌진하는 동안 시전자가 실제로 지나간 자리에 dist_cm 마다 작은 원. 적에 막혀 멈추면 거기까지
struct PathPainter
{
	uint64				casterId = 0;
	uint32				skillId = 0;
	uint32				effectIndex = 0;
	uint64				untilUs = 0;
	float				lastX = 0.f;
	float				lastY = 0.f;
	float				stepCm = 0.f;
	uint64				groupId = 0;		// 이 경로의 장판 묶음 (Zone::groupId)
	uint64				tickAtUs = 0;		// 묶음의 첫 틱 - 경로의 장판이 같은 Flush 에 틱해야 묶음 안 중복을 거를 수 있다
	SkillContext		ctx;
};

// 저 멀리 (1109, 2026-10-11) - 날아가던 적이 벽에 부딪히는 순간(넉백 시간 뒤) 기절 + 날아간 거리에 비례한 피해
struct PendingSlam
{
	uint64				casterId = 0;
	uint64				targetId = 0;
	uint32				skillId = 0;
	uint64				atUs = 0;
	float				flownCm = 0.f;
	float				adPer100Cm = 0.f;		// 넉백 효과의 ad_ratio - 100 cm 날아갈 때마다 이만큼 (합은 3.0 상한)
	float				bonusDamageMod = 0.f;	// 건곤일척 - 발동 때 소모한 배율
	float				x = 0.f;				// 부딪힌 자리 (연출)
	float				y = 0.f;
};

// 도착하면 거는 CC (추격 1165 "도착하면 그 적은 이동 속도가 느려진다", 2026-10-11 3차 로스터) - 질주 시간 뒤
struct PendingCc
{
	uint64				targetId = 0;
	uint64				instigatorId = 0;
	Protocol::CcType	type = Protocol::CC_TYPE_NONE;
	uint32				durationMs = 0;
	float				magnitude = 0.f;
	uint64				atUs = 0;
};

// 이동 스킬이 끝나는 때 (착지 충격 1182) - 그 자리 둘레 적에게 패시브 효과
struct PendingLanding
{
	uint64				casterId = 0;
	uint64				atUs = 0;
};

// 지연 폭발 (전염) - duration 뒤 대상에게 피해 + radius 안으로 스킬의 지속 피해
struct PendingBurst
{
	uint64				casterId = 0;
	uint64				targetId = 0;
	uint32				skillId = 0;
	uint32				effectIndex = 0;
	uint64				atUs = 0;
	float				baseDamage = 0.f;
	Protocol::DamageType type = Protocol::DAMAGE_TYPE_MAGIC;
	SkillContext		ctx;
	uint64				teamId = 0;					// 심은 사람의 팀 - 시전자가 떠난 뒤에도 아군에게 번지지 않는다 (ADR-0014)
};

// 날아가는 투사체 (skill-roster 2 단계). 스킬 투사체는 직선 · 피할 수 있다 · 벽에 막힌다 · 관통이면 지나간 적을 다 맞힌다.
// 평타 투사체(활 · 스태프)는 대상을 쫓아간다(필중) - 날아가는 중 벽에 막히면 끝 (사용자 결정 2026-10-10). FlushCombat 마다 StepProjectiles
constexpr uint64 PROJECTILE_ID_BASE = 5'000'000'000;
extern atomic<uint64> GNextProjectileId;

// 유도 스킬 화살 (포착 · 약속의 한 발, 2026-10-11) - 사거리 밖의 보이는 표식 대상까지 날아간다. 끝없이 쫓지 않게 이만큼만
constexpr float HOMING_SKILL_MAX_CM = 3000.f;

// 이동 사격 (1176, 2026-10-11) - 평타 windup 끝의 사거리 재검사 배율. 움직이며 때려도 끊기지 않는다
constexpr float MOVE_ATTACK_RANGE_SCALE = 1.5f;

struct Projectile
{
	uint64				id = 0;
	uint64				casterId = 0;
	uint32				skillId = 0;			// 0 = 평타
	uint32				effectId = 0;
	float				x = 0.f;
	float				y = 0.f;
	float				dirX = 1.f;
	float				dirY = 0.f;
	float				speedCms = 0.f;
	float				radiusCm = 0.f;
	float				remainingCm = 0.f;		// 직선 - 남은 사거리
	uint64				homingTargetId = 0;		// 0 이 아니면 유도
	bool				pierce = false;
	vector<uint64>		hitIds;					// 관통이 이미 맞힌 대상 - 두 번 맞지 않는다
	SkillContext		ctx;
	uint64				lastStepUs = 0;
	float				damageMult = 1.f;		// 평타 투사체 한 발의 몫 (두플렉스 두 발 = 각 0.6, 2026-10-11)
};

// 방 종류 (ADR-0014) - World = GRooms 의 층 방 (PvP). PVE 는 런마다 · 파티마다 새 인스턴스 (InstanceManager)
enum class RoomKind : uint8 { World = 0, PveSolo = 1, PveCoop = 2 };

class Room : public JobQueue
{
public:
			Room();
	virtual ~Room();

	// PVE 인스턴스 (ADR-0014) - InstanceManager::Create 가 부른다. 레벨 · 몬스터 · 상자 · 타워를 그 층 그대로 들고, 4 루프를 돌린다
	void	InitInstance();
	void	StartLoops();
	bool	IsInstance() const { return _instanceId != 0; }
	Protocol::RunMode RunModeOf() const;
	string	Tag() const;									// 로그용 - 층 방 "w0" · 인스턴스 "i17"

	bool	EnterRoom(ObjectRef object, bool randPos = true);
	bool	LeaveRoom(ObjectRef object);

	bool	HandleEnterPlayer(PlayerRef player);
	bool	HandleLeavePlayer(PlayerRef player);
	void	HandleMove(Protocol::C_MOVE pkt);
	void	HandleAttack(uint64 attackerId, uint64 targetId, float aimX = 0.f, float aimY = 0.f);	// aim = 커서 지점 (대상 없는 평타만 쓴다)
	void	HandleSkill(uint64 casterId, Protocol::C_SKILL pkt);
	void	HandleSkillCancel(uint64 casterId);
	void	ApplyCc(uint64 targetId, uint64 instigatorId, Protocol::CcType type, uint32 baseDurationMs, float magnitude);
	void	ResolveAttacks(uint64 nowUs);
	void	ResolveCasts(uint64 nowUs);
	void	ResolveTelegraphs(uint64 nowUs);
	void	UpdateCc(uint64 nowUs);
	void	FlushCcState(uint64 nowUs);

	void	HandleEquip(GameSessionRef session, string requestId, uint64 instanceId, Protocol::EquipSlot slot);
	void	HandleUnequip(GameSessionRef session, string requestId, uint64 instanceId);
	void	HandleBagOpen(GameSessionRef session, uint64 bagId);
	void	HandleLoot(GameSessionRef session, string requestId, uint64 bagId, uint64 instanceId);
	void	HandleDrop(GameSessionRef session, string requestId, uint64 instanceId);

	bool	AddBag(BagRef bag);
	bool	RemoveBag(uint64 bagId);
	void	DebugDropBag(uint64 playerId);
	void	DebugKill(uint64 playerId);

	void	BeginDeathCommit(const CreatureRef& victim);
	void	OnBagCreated(DeathSnapshot snap, bool ok);
	void	BeginEscapeCommit(uint64 playerId);
	// despawnBatch 가 있으면 S_DESPAWN 을 바로 방송하지 않고 id 만 모은다 - 완충 한 틱에 나가는 사람을 한 패킷으로 (AC-ET-26)
	// (오버로드로 두면 DoAsync(&Room::BeginEscapeCommit, …) 가 어느 쪽인지 못 고른다 - 이름을 따로 둔다)
	void	BeginEscapeCommitInto(uint64 playerId, vector<uint64>* despawnBatch);

	void	HandleDisconnect(PlayerRef player);
	void	HandleReattach(GameSessionRef session, PlayerRef body);

	// 부팅 때 한 번 - MonsterTable 의 이 층 자리를 전부 만피 Idle 로 낳는다 (monster-ai.md Core Rule 22)
	void	InitMonsters();

	// 부팅 때 한 번 - TowerTable 의 이 층 타워로 게이지를 만든다 (escape-timer.md). 타워 없는 층은 구역 없이 돈다
	void	InitEscape();

	// /gold N (FBTK_DEBUG_GOLD=1) - 런 골드를 더한다
	void	DebugAddRunGold(uint64 playerId, uint64 amount);

	// 테스트 모드 /t 명령 (FBTK_TEST_MODE=1 · design/quick-specs/test-mode-2026-10-11.md) - RoomTest.cpp
	void	TestGear(uint64 playerId, TestGearRequest req);
	void	TestSpawnDummies(uint64 playerId, uint32 monsterTypeId, int32 count, bool ai, float x, float y);
	void	TestSetDummyMode(uint64 playerId, bool ai);
	void	TestClearDummies(uint64 playerId);
	void	TestHeal(uint64 playerId);
	void	TestResetCooldowns(uint64 playerId);
	void	TestGod(uint64 playerId, bool on);
	void	TestFow(uint64 playerId, bool fowOn);			// 내 시야만 - 끄면 거리 · 벽과 상관없이 본다 (은신은 그대로)
	void	TestSpawnPack(uint64 playerId, bool reactive);	// 무리 하나 - 내 둘레 20 m 네모 길을 돈다 (2026-10-11 몬스터 개선)
	void	DropOrphanDummies();							// 주인이 층을 떠난 더미를 치운다 (UpdateMonsters 앞)

public:
	void	UpdateTick();
	void	FlushMoves();
	void	FlushCombat();
	void	FlushSpawns();

	RoomRef	GetRoomRef();

private:
	bool	AddObject(ObjectRef object);
	bool	RemoveObject(uint64 objectId);

private:
	void	Broadcast(SendBufferRef sendBuffer, uint64 exceptId = 0);
	void	Broadcast(SendBufferRef sendBuffer, const unordered_set<uint64>& exceptIds);
	
	bool	CancelCast(const CreatureRef& caster);
	void	SendEquipSync(const PlayerRef& player);
	void	SendInventorySync(const PlayerRef& player);

	// 털린 상자 전체 목록 - 입장 · 재접속 때 (radar.md OQ-1). 목록에 없으면 Sealed
	void	SendChestSnapshot(const PlayerRef& player);
	void	KillPlayer(const PlayerRef& victim, uint64 instigatorId, Protocol::DeathCause cause, const char* by);
	void	UpdateDetached(uint64 nowUs);
	void	ReleaseDetachedBody(uint64 characterId);
	void	PushBagContents(const BagRef& bag);

	void	CollectSkillTargets(const CreatureRef& caster, const SkillDef& def, const float originX, float originY, uint64 castTargetId, float aimX, float aimY, OUT vector<CreatureRef>& outTargets);
	void	ApplySkillEffects(const CreatureRef& caster, const SkillDef& def, const vector<CreatureRef>& targets, const SkillContext& ctx, uint64 nowUs);
	void	ApplyMovement(const CreatureRef& caster, const SkillEffect& effect, uint64 nowUs);

	// ── 스킬 로스터 1 단계 (design/quick-specs/skill-roster-2026-10-10.md) ──

	// 발동 공통 - 쿨다운 · S_SKILL_HIT · 판정 · 효과 (예고가 있으면 PendingTelegraph). recast = 재입력 두 번째 입력
	void	FireSkill(const CreatureRef& caster, const SkillDef& def, Protocol::EquipSlot slot, uint64 castTargetId, float aimX, float aimY, bool recast, uint64 nowUs);

	// 효과 하나의 requires 조건. 표식은 시전자 본인 것만 (사용자 결정)
	bool	MeetsCondition(const SkillEffect& e, const SkillContext& ctx, const CreatureRef& target, bool anyHit, bool anyKill, uint64 nowUs) const;
	void	ApplySelfEffect(const CreatureRef& caster, const SkillDef& def, const SkillEffect& e, const SkillContext& ctx, uint64 nowUs);
	void	ApplyTargetEffect(const CreatureRef& caster, const SkillDef& def, uint32 effectIndex, const CreatureRef& target, const SkillContext& ctx, uint64 nowUs);

	// 피해의 출처 - 모자 로스터의 배율 · 면역 · 트리거가 본다. skillId 0 = 평타 · 지속 피해 등, elem = fire · lightning · poison · bleed · frost
	struct DamageSource
	{
		uint32 skillId = 0;
		string elem;
		bool basic = false;
		float extraMult = 0.f;		// 건곤일척 - 발동 때 소모한 배율 (SkillContext::bonusDamageMod)
		bool tick = false;			// 지속 피해 · 장판 틱 - 맞은 순간이 아니다 (RevealOnHit 가 보지 않는다)
	};

	// 모든 피해는 여기를 지난다 - 배율 · 면역 · 첫 타 경감 → 치명상 방어(맹수의 본능) → 체력 차감 → S_DAMAGE → 몬스터 어그로 → 사망. 돌려주는 값 = 남은 체력.
	// attacker 는 없을 수 있다 (떠난 사람의 지속 피해) - attackerId 만 패킷에 실린다. 이미 죽은 대상은 건드리지 않는다
	int32	DealDamage(const CreatureRef& attacker, uint64 attackerId, const CreatureRef& target, int32 damage, Protocol::DamageType type, bool isCrit, uint64 nowUs, const DamageSource& src = DamageSource());

	// ── 모자 로스터 #78 ~ #104 (2026-10-10) ──
	int32	ApplyDamageMods(const CreatureRef& attacker, const CreatureRef& target, int32 damage, const DamageSource& src, uint64 nowUs);	// 일편단심 · damage_mod · 면역 · 염두
	bool	HasImmunity(const CreatureRef& creature, const string& elem, uint64 nowUs) const;		// 패시브(피뢰 · 방염) + 버프(만독불침)
	string	ElementOf(const string& param) const;														// 효과 param → 속성 이름
	bool	IgnoresWalls(const CreatureRef& creature) const;										// 유령 신부의 축복
	bool	IsAlone(const CreatureRef& creature, float radiusCm) const;								// 외로운 외침
	void	OnSkillDamageLanded(const CreatureRef& attacker, const CreatureRef& target, uint32 skillId, uint64 nowUs);	// 패시브 on_skill:<id> (월척)
	void	OnDotApplied(const CreatureRef& attacker, const string& kind, uint64 nowUs);					// 패시브 on_dot:<kind> (혈기)
	void	TriggerDashMarks(const CreatureRef& dasher, float dirX, float dirY, uint64 nowUs);			// 패시브 on_dash (간파)
	void	ApplyPassiveEffect(const CreatureRef& owner, const SkillDef& def, uint32 effectIndex, const CreatureRef& target, Protocol::EquipSlot slot, float scale, uint64 nowUs);
	void	Cleanse(const CreatureRef& creature, const string& what, uint64 nowUs);					// poison / burn / all
	void	Heal(const CreatureRef& creature, uint64 healerId, int32 amount, uint64 nowUs);
	void	RevealNearest(const CreatureRef& caster, const SkillDef& def, uint64 nowUs);				// 외로운 외침 - 시전자에게만
	void	RevealSkills(const CreatureRef& caster, const CreatureRef& target, uint32 durationMs);		// 총명 - S_SKILL_REVEAL
	void	MoveToMark(const CreatureRef& caster, const SkillDef& def, const SkillEffect& e, const string& markKind, uint64 nowUs);	// 집념
	void	StartDoom(const CreatureRef& caster, const SkillDef& def, const SkillEffect& e, uint64 nowUs);
	void	LiftDoom(const CreatureRef& caster, bool killed, uint64 nowUs);
	bool	TryLethalSave(const PlayerRef& victim, const CreatureRef& attacker, OUT int32& damage, uint64 nowUs);

	// 서버가 옮기는 변위 - 위치를 끝점으로 바로 놓고(S_MOVE) 날아가는 동안 넉백 CC. 클라는 KnockbackInfo 로 같은 끝점까지 밀려간다.
	// 그동안의 C_MOVE 는 HandleMove 가 무시한다 (서버가 위치를 소유하는 구간). 벽에 닿으면 wallStun 일 때 벽 기절
	void	PushCreature(const CreatureRef& target, uint64 instigatorId, float dirX, float dirY, float distCm, float speedCms, bool wallStun, uint64 nowUs,
				OUT float* movedCm = nullptr, OUT bool* hitWall = nullptr);		// 실제로 밀린 거리 · 벽에 부딪혔나 (저 멀리)
	void	PullCreature(const CreatureRef& target, const CreatureRef& caster, float speedCms, uint64 nowUs);
	void	BlinkCaster(const CreatureRef& caster, const SkillEffect& e, const SkillContext& ctx, uint64 nowUs);

	// 평타 적중 패시브 (on_hit) - 공격자의 패시브 슬롯마다 추가 피해
	void	ApplyOnHitPassives(const CreatureRef& attacker, const CreatureRef& target, uint64 nowUs);

	void	UpdateStatuses(uint64 nowUs);		// 지속 피해 틱 · 상태 만료 (FlushCombat)
	void	ResolvePendingHits(uint64 nowUs);	// 다단 히트의 남은 타

	// ── 3 단계 - 장판 · 유지 · 반격 · 받아넘기기 · 연쇄 · 지연 폭발 · 속성 · 던지기 · 경독 ──
	void	PlaceZone(const CreatureRef& caster, const SkillDef& def, uint32 effectIndex, float x, float y, const SkillContext& ctx, uint64 nowUs);
	void	PlaceZonesFor(const CreatureRef& caster, const SkillDef& def, uint32 effectIndex, const SkillContext& ctx, uint64 nowUs);	// 모양에 따라 지점 하나 · 경로 여러 개
	void	UpdateZones(uint64 nowUs);
	void	StartHold(const CreatureRef& caster, const SkillDef& def, uint32 effectIndex, Protocol::EquipSlot slot, const SkillContext& ctx, uint64 nowUs);
	void	StopHold(const CreatureRef& caster, uint64 nowUs, const char* why);
	void	UpdateHolds(uint64 nowUs);
	void	HoldTick(const CreatureRef& caster, const SkillDef& def, const SkillEffect& hold, uint64 nowUs);
	bool	TryCounter(const CreatureRef& target, const CreatureRef& attacker, uint64 attackerId, uint64 nowUs);	// true = 막고 반격했다
	bool	TryParry(const CreatureRef& target, float fromX, float fromY, bool projectile, uint64 nowUs);			// true = 받아넘겼다 (풍차는 사방의 투사체만)
	bool	TryReflect(Projectile& p, const CreatureRef& caster, const CreatureRef& target, uint64 nowUs);			// 자업자득 - true = 되돌려 보냈다 (새 투사체 발사)
	bool	MeetsWeaponRequirement(const CreatureRef& caster, const SkillDef& def) const;							// skills.csv requires_weapon (갑옷 #106 ~ #114)
	void	ApplyWallEffects(const CreatureRef& caster, const SkillDef& def, uint64 nowUs);							// requires=wall 효과 (회오리 · 질풍노도)
	void	ApplyChain(const CreatureRef& caster, const CreatureRef& first, const SkillEffect& e, uint32 effectId, uint64 nowUs);
	void	ResolveBursts(uint64 nowUs);
	void	UpdateWindows(uint64 nowUs);		// 경독 창 닫힘 · 반격 끝
	void	SetStance(const CreatureRef& caster, const string& element, float ratio, uint64 nowUs);
	void	ApplyThrow(const CreatureRef& caster, const CreatureRef& target, const SkillEffect& e, const SkillContext& ctx, uint64 nowUs);

	// ── 도발 (2026-10-10 사용자 요청) - 걸린 쪽을 서버가 시전자 쪽으로 taunt_speed_cms 로 옮긴다 (FlushMoves 마다). 플레이어면 보정 S_MOVE 로 본인에게도 알린다 ──
	void	StepTaunts(uint64 nowUs);

	// ── 4 단계 - 미끼 · 무적 · 핏자국 · 도약 추락 · 회오리 벽 ──
	void	SpawnDecoy(const CreatureRef& caster, const SkillEffect& e, const SkillContext& ctx, uint64 nowUs);
	void	DespawnDecoy(const CreatureRef& decoy, const char* why);
	void	UpdateDecoys(uint64 nowUs);
	void	DropTrail(const CreatureRef& bleeder, const DotInstance& dot, uint64 nowUs);
	void	UpdateTrails(uint64 nowUs);
	void	OnSpinTouchedWall(const CreatureRef& caster, const SkillDef& def, uint64 nowUs);

	// ── 투사체 (2 단계) ──
	void	LaunchProjectile(Projectile p);
	void	LaunchSkillProjectile(const CreatureRef& caster, const SkillDef& def, const SkillContext& ctx, uint64 nowUs);
	void	StepProjectiles(uint64 nowUs);
	bool	StepStraight(Projectile& p, const CreatureRef& caster, const WallMap& walls, float dtSec, uint64 nowUs);	// true = 끝났다
	bool	StepHoming(Projectile& p, const CreatureRef& caster, const WallMap& walls, float dtSec, uint64 nowUs);
	bool	CanProjectileHit(const CreatureRef& caster, const CreatureRef& target, uint64 nowUs) const;
	void	OnProjectileHit(Projectile& p, const CreatureRef& caster, const CreatureRef& target, uint64 nowUs);
	void	EndProjectile(const Projectile& p, Protocol::ProjectileEnd reason, uint64 targetId);
	void	PushStatus(bool applied, uint64 targetId, Protocol::StatusKind kind, const string& param, uint32 stacks, uint32 remainingMs, float magnitude, uint64 instigatorId);
	void	ApplyCcRaw(const CreatureRef& target, uint64 instigatorId, Protocol::CcType type, uint32 effectiveMs, float magnitude, uint64 nowUs);

	void	SavePlayer(PlayerRef player);
	void	UpdateSaves(uint64 nowUs);
	void	UpdateBagExpiry(uint64 nowUs);
	void	UpdateChests(uint64 nowUs);
	
	void	FindSpawnPosition(float baseX, float baseY, float& outX, float& outY);

	// 진입 무적을 푼다 (AC-10) - 이동 · 공격 · 스킬. 이미 없으면 아무것도 하지 않는다
	void	ClearSpawnInvulnerability(const PlayerRef& player, const char* by);

	// 던전 안에서 착용이 바뀌었다 - 슬롯 · 전투 스탯을 다시 만들고, 체력이 바뀌었으면 S_DAMAGE 0 으로 알린다
	void	OnEquipmentChanged(const PlayerRef& player, const char* tag);

	// 사망 처리 공용 - S_DIED · 원인(가해자 기준) · 시전 취소, 그다음 몬스터면 드랍 · 플레이어면 사망 커밋
	void	OnCreatureKilled(const CreatureRef& victim, const CreatureRef& instigator, uint64 instigatorId, uint64 nowUs);

	// 체력 0 이 된 순간 부른다. 전투 Flush 안이면 Flush 끝으로 미루고(동시 치사, KillBatch), 밖이면 바로 OnCreatureKilled
	void	KillCreature(const CreatureRef& victim, const CreatureRef& instigator, uint64 instigatorId, uint64 nowUs);

	// 살아 있거나, 이번 전투 Flush 에 막 죽어 아직 자기 히트가 남은 쪽 (TR-combat-022)
	bool	CanStillStrike(const CreatureRef& creature) const;

	// 몬스터 (design/gdd/monster-ai.md § I). 판단 = UpdateTick(10 Hz), 이동 = FlushMoves 앞(30 Hz)
	void	UpdateMonsters(uint64 nowUs);
	void	StepMonsters(uint64 nowUs);
	void	SpawnMonster(MonsterSlot& slot, uint64 nowUs);
	void	OnMonsterDamaged(const MonsterRef& monster, uint64 attackerId, int32 damage, uint64 nowUs);
	void	OnMonsterDied(const MonsterRef& monster, uint64 instigatorId, uint64 nowUs);
	bool	BeginMonsterCast(const MonsterRef& monster, const SkillDef& def, uint64 nowUs);

	// ── 무리 (2026-10-11 몬스터 개선) - RoomPacks.cpp ──
	void	InitPacks(uint64 nowUs);
	void	SpawnPack(uint32 slotIndex, uint64 nowUs);
	void	UpdatePacks(uint64 nowUs, const LineClearFn& lineClear);		// 판단 (UpdateMonsters 안, 10 Hz)
	void	StepPacks(uint64 nowUs);										// 걸음 (StepMonsters 안, 30 Hz)
	void	OnPackMemberDamaged(const MonsterRef& monster, uint64 attackerId, float ax, float ay, uint64 nowUs);
	void	OnPackMemberDied(const MonsterRef& monster, uint64 nowUs);
	void	SnapPackMember(const MonsterRef& monster, float x, float y, uint64 nowUs);
	bool	SeenByAnyone(uint64 objectId) const;							// 누군가의 클라에 스폰돼 있는가 (FoW) - 순간 이동은 아무도 안 볼 때만
	PackSlot* PackOf(const Monster& monster);
	void	DropTestPacks(uint64 ownerId);

	// 탈출 (escape-timer.md § Core Rules). UpdateTick 에서 게이지 → 타이머 → (기존) 끊긴 몸 순서
	void	UpdateEscapeTowers(uint64 nowUs);
	void	UpdateRunTimers(uint64 nowUs);
	void	BeginExpiryCommit(const PlayerRef& player, uint64 nowUs);
	void	SendGauge(const PlayerRef& player, uint32 towerWireId, Protocol::EscapeGaugeState state, uint32 permille, float ratePerS, bool selfInZone);

	// 런 하나가 끝났다 - runs.csv 한 줄 (§ G). reason: ESCAPED · EXPIRED · DIED · DISCONNECT_DEATH
	void	WriteRunTelemetry(const PlayerRef& player, const char* reason, uint64 killerId, uint64 goldKept, uint64 nowUs);

	// ── FoW (design/gdd/fog-of-war.md, 2026-10-11) - RoomVision.cpp ──
	// 서버 권위 시야. 플레이어마다 "이 클라가 아는 생물" 집합(Player::fowKnown)을 들고, 보이게 된 생물은 S_SPAWN + CC · 상태 스냅샷,
	// 안 보이게 된 생물(유예 뒤)은 S_DESPAWN 을 그 사람에게만 보낸다. 이동 · 전투 이벤트는 SendVisible 이 받는 사람별로 거른다.
	// 가방 · 상자 · 타워는 숨기지 않는다. Vision::Enabled() == false 면 전부 예전처럼 (전원에게 전부)
	void	UpdateVisibility(uint64 nowUs);															// FlushMoves 앞 (30 Hz)
	bool	CanSee(const PlayerRef& viewer, uint64 objectId) const;									// 그 생물을 이 클라가 아는가. 자기 · 생물이 아닌 것은 참, 0 · 없는 id 는 거짓
	bool	NearViewer(const PlayerRef& viewer, float x, float y, float extraCm) const;				// 내 시야 반경(+여유) 안의 지점 - 투사체 · 장판 · 범위 연출 (시선은 보지 않는다)
	float	VisionRadiusOf(const PlayerRef& player, uint64 nowUs) const;								// 기본 × (1 + 천리안) × (실명이면 blind_view_scale)
	float	VisionBonusOf(const CreatureRef& creature) const;											// 패시브 vision 효과 합 (천리안 1132)
	void	SendVisionIfChanged(const PlayerRef& viewer, float radiusCm, float cameraScale, uint64 nowUs);
	void	SendSnapshots(const PlayerRef& viewer, const vector<CreatureRef>& fresh, uint64 nowUs);	// 막 보이게 된 생물의 CC · 상태를 그 사람에게
	void	AppendCcState(const CreatureRef& target, uint64 nowUs, OUT Protocol::S_CC_STATE& pkt) const;
	void	AppendStatusSnapshot(const CreatureRef& target, uint64 nowUs, OUT Protocol::S_STATUS& pkt) const;
	void	ForgetObject(uint64 objectId);																// RemoveObject - 모든 플레이어의 아는 집합 · 추가 시야원에서 뺀다
	bool	CanTarget(const CreatureRef& attacker, const CreatureRef& target, uint64 nowUs) const;	// 은신 · 이 클라가 모르는 대상은 지정 불가 (안티치트)
	void	StartStealth(const CreatureRef& creature, uint32 skillId, uint32 durationMs, bool breaksOnDamage, uint64 nowUs);
	void	BreakStealth(const CreatureRef& creature, const char* why, uint64 nowUs);
	void	AddVisionExtra(const PlayerRef& owner, const VisionExtra& extra);							// 같은 sourceId 는 갱신
	void	RevealTo(const PlayerRef& viewer, uint64 subjectId, uint64 untilUs);						// 시야와 상관없이 보이게 (외로운 외침 · 종이매)

	// 받는 사람마다 볼 수 있는 항목만 보낸다. count(viewer) == fullCount 인 사람은 makeFull 버퍼를 함께 쓴다 (직렬화 1회),
	// 일부만 보이는 사람은 makeMine 으로 자기 몫을 따로 만든다. 0 이면 안 보낸다. FoW 를 끄면 count 가 늘 fullCount 라 예전 Broadcast 와 같다
	void	SendVisible(int32 fullCount, const function<int32(const PlayerRef&)>& count,
				const function<SendBufferRef()>& makeFull, const function<SendBufferRef(const PlayerRef&)>& makeMine);

	// 항목 목록 하나짜리 패킷용 - add(pkt, item) 으로 담고 keep(viewer, item) 으로 거른다. 정의는 Room.cpp (거기서만 쓴다)
	template <typename TPkt, typename TItem, typename AddFn, typename KeepFn>
	void	SendListVisible(const vector<TItem>& items, AddFn&& add, KeepFn&& keep, uint16 statId = 0);

	// ── 2차 로스터 (게임 ID 1115 ~ 1137 · FoW 의존 기존 스킬, 2026-10-11) - RoomSkills2.cpp ──
	const WallMap&	Walls() const;																			// 층 벽 + 지각변동 같은 임시 벽 (있으면 합친 지도)
	void	RaiseWall(const CreatureRef& caster, const SkillDef& def, const SkillEffect& e, const SkillContext& ctx, uint64 nowUs);	// 지각변동
	void	UpdateDynamicWalls(uint64 nowUs);
	void	RebuildWalls();
	void	SendDynamicWalls(const PlayerRef& player, uint64 nowUs);		// 입장 · 재접속 - 서 있는 임시 벽을 남은 시간으로 (지형이라 FoW 와 무관)
	void	StartVolley(const CreatureRef& caster, const SkillDef& def, const SkillEffect& e, const SkillContext& ctx, uint64 nowUs);	// 그림자 던지기 · 퍼붓기 · 천방지축
	void	UpdateVolleys(uint64 nowUs);
	void	LaunchVolleyShot(const CreatureRef& caster, const SkillDef& def, float dirX, float dirY, const SkillContext& ctx, uint64 nowUs);
	void	SpawnClone(const CreatureRef& caster, const SkillDef& def, const SkillEffect& e, const SkillContext& ctx, uint64 nowUs);	// 그림자 보내기
	void	StepClones(uint64 nowUs);																// FlushMoves - 분신이 걷는다
	void	CastHawk(const CreatureRef& caster, const SkillDef& def, const SkillEffect& e, uint64 nowUs);	// 종이매
	void	StartVisionArea(const CreatureRef& caster, const SkillDef& def, const SkillEffect& e, const SkillContext& ctx, uint64 nowUs);	// 맹수의 탐색 · 루멘(param point - 조준 지점)
	void	MoveBehindTarget(const CreatureRef& caster, const SkillDef& def, const SkillEffect& e, const SkillContext& ctx, uint64 nowUs);	// 그림자 밟기
	void	ApplyShield(const CreatureRef& caster, const SkillDef& def, const SkillEffect& e, uint64 nowUs);	// 전사의 다짐
	void	ApplyBasicProcs(const CreatureRef& attacker, const CreatureRef& target, uint64 nowUs);	// basic_proc 버프의 on_basic 효과 (혼원일체 · 여진)
	void	ApplyBasicSpin(const CreatureRef& attacker, const CreatureRef& mainTarget, float radiusCm, uint64 nowUs);	// 난무 - 주위 적 전부
	void	TryThorns(const CreatureRef& attacker, const CreatureRef& target, int32 damageTaken, uint64 nowUs);	// 고슴도치
	void	UpdatePuddle(Zone& z, uint64 nowUs);														// 명경지수 - 주인이 안에 있으면 사거리 버프
	void	PushVisualZone(uint64 casterId, uint32 effectId, float x, float y, float radiusCm, uint32 durationMs, const char* param);	// 연출만 (충격파 · 사방 베기)

	// ── 스킬 개선 (2026-10-11 저녁, 사용자 플레이 피드백) - RoomSkills3.cpp ──
	void	ScheduleSelfCcAtEnd(const CreatureRef& caster, const SkillDef& def, const SkillEffect& e, uint64 nowUs);	// 오버로드 · 니힐 - 효과가 끝날 때
	void	UpdatePendingSelfCcs(uint64 nowUs);
	void	StartPathPainter(const CreatureRef& caster, const SkillDef& def, uint32 effectIndex, const SkillContext& ctx, uint64 nowUs);	// 열화
	void	UpdatePathPainters(uint64 nowUs);
	void	PlaceConeZone(const CreatureRef& caster, const SkillDef& def, uint32 effectIndex, const SkillContext& ctx, uint64 nowUs);	// 네불라 아르카나
	uint64	FindHomingMarkTarget(const CreatureRef& caster, const string& markKind, float fromX, float fromY, float maxRangeCm, uint64 nowUs) const;	// 포착 · 약속의 한 발
	string	HomingMarkOf(const SkillDef& def) const;
	void	SendDetectionPings(const PlayerRef& owner, const SkillDef& def, float radiusCm, uint32 durationMs, uint64 nowUs);	// 맹수의 탐색 - 적 위치 표시
	void	NotifyPassiveTrigger(const CreatureRef& owner, uint32 skillId);		// 염두 등 - 쿨다운이 시작됐다고 주인 클라에
	void	RevealOnHit(const CreatureRef& attacker, const CreatureRef& target, uint64 nowUs);	// 안 보이는 적을 맞혔다 - 잠깐 보이게
	float	TakeStillBonus(const CreatureRef& caster, const SkillDef& def, uint64 nowUs);		// 건곤일척 - 제자리 망치 스킬이면 배율을 꺼내고 버프를 지운다
	void	BreakStillBuff(const CreatureRef& caster, const char* why, uint64 nowUs);			// 움직이면 건곤일척 피해 증가가 사라진다
	void	OnStillBuffApplied(const CreatureRef& caster, uint32 skillId);						// 건곤일척 버프가 걸렸다 - 지금 자리를 기억한다
	bool	StartSpecialBasic(const CreatureRef& attacker, float aimX, float aimY, uint64 nowUs);	// 일보일검 · 난무 - 대상 없는 평타. false = 그런 버프가 없다
	void	ResolveSpecialBasic(const CreatureRef& attacker, uint64 nowUs);
	bool	HasLineOfAttack(const CreatureRef& caster, float fromX, float fromY, const CreatureRef& target) const;	// 스킬도 벽에 막힌다 (유령 신부의 축복 예외)
	void	ScheduleSlam(const CreatureRef& caster, const CreatureRef& target, const SkillDef& def, const SkillEffect& e, float flownCm, uint32 travelMs, const SkillContext& ctx, uint64 nowUs);	// 저 멀리 - 벽 충돌
	void	UpdateSlams(uint64 nowUs);
	void	PushTargetVisual(uint64 casterId, uint32 effectId, const CreatureRef& target);		// 맞은 대상마다 연출 (낙뢰 벼락 · 혼원일체 원혼)

	// ── 3차 로스터 (게임 ID 1138 ~ 1189, 2026-10-11) - RoomSkills4.cpp ──
	void	SetAnchor(const CreatureRef& caster, const SkillDef& def, const SkillEffect& e, const SkillContext& ctx, uint64 nowUs);	// 포르타 문 · 비창(투사체가 멈추면)
	void	OnAnchorProjectileEnd(const Projectile& p);													// 비창 - 던진 창이 멈춘 자리
	void	TeleportToAnchor(const CreatureRef& caster, const SkillDef& def, const SkillEffect& e, uint64 nowUs);
	void	Rewind(const CreatureRef& caster, const SkillEffect& e, uint64 nowUs);							// 회귀
	bool	IsNearWall(const CreatureRef& creature) const;													// 벽 차기 - 붙어 있는가
	void	WallKick(const CreatureRef& caster, const SkillEffect& e, uint64 nowUs);						// 벽 차기 - 벽 반대쪽으로
	void	CutCooldowns(const CreatureRef& caster, const SkillDef& def, const SkillEffect& e, uint64 nowUs);	// 템푸스
	void	SendFootprints(const CreatureRef& caster, const SkillDef& def, const SkillEffect& e, uint64 nowUs);	// 발자국 추적 - 주인에게만
	void	ScheduleCcOnArrive(const CreatureRef& caster, const SkillDef& def, const SkillEffect& e, const CreatureRef& target, uint64 nowUs);	// 추격
	void	UpdatePendingCcs(uint64 nowUs);
	void	ScheduleLanding(const CreatureRef& caster, uint32 travelMs, uint64 nowUs);						// 착지 충격 - 이동이 끝나는 때
	void	UpdateLandings(uint64 nowUs);
	float	MoveDistScale(const CreatureRef& creature) const;												// 날개 - 이동 스킬 거리 × (1 + move_dist)
	void	TriggerSelfPassives(const CreatureRef& owner, SkillRequire condition, uint64 nowUs);			// on_damaged · on_kill 자기 효과 (반격의 걸음 · 사냥꾼의 걸음)
	void	TriggerStancePassives(const CreatureRef& owner, const string& element, uint64 nowUs);			// 플럭스 - 속성이 바뀌었다
	void	TryLowHpEscape(const CreatureRef& victim, const CreatureRef& attacker, uint64 nowUs);			// 줄행랑
	void	UpdateIdleCombat(uint64 nowUs);																	// 행군 - UpdateTick
	void	RecordHistories(uint64 nowUs);																	// 회귀 · 발자국 - UpdateTick 10 Hz
	void	BurstShield(const CreatureRef& owner, uint64 nowUs);											// 스쿠툼 - 속성이 깃든 채 깨졌다
	bool	DodgesProjectiles(const CreatureRef& target) const;												// 구르기
	bool	IsUnmovable(const CreatureRef& target) const;													// 철벽진 · 무거운 발
	float	IncomingDamageMult(const CreatureRef& attacker, const CreatureRef& target) const;				// 철벽진 앞 · 맨발 받는 피해
	void	PullToPoint(const CreatureRef& target, uint64 instigatorId, float x, float y, float speedCms, uint64 nowUs);	// 바쿠움
	void	NotifyCooldown(const CreatureRef& owner, uint32 skillId, uint32 remainingMs);					// 클라 슬롯 쿨다운을 서버 값으로 (템푸스 · 패시브)
	bool	IsSprinting(const CreatureRef& creature) const;													// 전력 질주 - 공격 · 스킬 불가

public:
	uint32								_floorId = 0;

	// PVE 인스턴스 (ADR-0014) - 만들 때 한 번 정하고 이후 읽기만 한다. 층 방은 0 / World
	RoomKind							_kind = RoomKind::World;
	uint64								_instanceId = 0;
	uint64								_partyId = 0;			// 협동 인스턴스를 가진 파티 (솔로 = 0)
	uint64								_teamId = 0;			// 들어오는 플레이어가 받는 팀 (협동 = 파티 id, 솔로 = 0)

private:
	// 인스턴스 수명 - 방 큐 안에서만
	void	TryCloseInstance(uint64 nowUs);
	void	CloseInstance();
	bool	RefuseInstanceJoin(const PlayerRef& player, const char* reason, Protocol::PartyResult notice);
	bool								_closing = false;		// 닫혔다 - 루프가 다시 걸리지 않고 입장을 받지 않는다
	int32								_pendingDeathCommits = 0;	// BeginDeathCommit ++ · OnBagCreated -- (가방이 아직 안 생겼다)
	uint64								_emptySinceUs = 0;
	unordered_set<uint64>				_enteredOnce;			// 이 인스턴스에 한 번 들어온 캐릭터 - 다시 못 들어온다 (D3)

	// 팀 (ADR-0014) - 자기 자신은 아군이 아니다 (자기 자신은 원래 따로 거른다)
	static bool	SameTeam(const Creature& a, const Creature& b);
	static bool	SameTeam(const CreatureRef& a, const CreatureRef& b) { return a != nullptr && b != nullptr && SameTeam(*a, *b); }

private:
	unordered_map<uint64, ObjectRef>	_objects;
	unordered_set<uint64>				_dirtyMovers;
	unordered_map<uint64, BagRef>		_bags;
	unordered_set<uint64>				_detached;

	vector<uint64>						_pendingSpawns;
	vector<PendingTelegraph>			_pendingTelegraphs;

	// 아이템 롤용. Room JobQueue 안에서만 쓰므로 잠그지 않는다.
	mt19937								_rng{ random_device{}() };

	int32 _enterCount = 0;
	uint64 _enterTotalUs = 0;
	uint64 _enterMaxUs = 0;

private:
	unordered_set<uint64>				_attackers;
	vector<Protocol::AttackInfo>		_pendingAttacks;
	vector<uint64>						_pendingAttackCancels;

	unordered_set<uint64>				_casters;
	vector<Protocol::SkillCastInfo>		_pendingCasts;
	vector<uint64>						_pendingCastCancels;
	vector<Protocol::SkillHitInfo>		_pendingSkillHits;

	vector<Protocol::DamageInfo>		_pendingDamages;
	vector<Protocol::DiedInfo>			_pendingDeaths;

	KillBatch<CreatureRef>				_kills;				// 전투 Flush 동안만 열린다 (FlushCombat)

	unordered_map<uint64, MonsterRef>	_monsters;			// 시체 유예 중인 것 포함
	vector<MonsterSlot>					_monsterSlots;
	vector<unique_ptr<PackSlot>>		_packs;				// 무리 - 주소가 바뀌지 않게 unique_ptr (Monster::spawn 이 안을 가리킨다)
	unordered_map<uint64, shared_ptr<MonsterSpawnDef>>	_dummySpawns;	// 테스트 모드 더미의 자리 - Monster::spawn 이 가리킨다. 몬스터를 지울 때 함께
	vector<BrainPlayer>					_brainPlayers;		// 판단 틱마다 다시 채운다 - 몬스터마다 _objects 를 훑지 않게

	vector<EscapeTowerState>			_towers;
	uint64								_lastEscapeTickUs = 0;

	unordered_set<uint64>				_ccTargets;
	vector<Protocol::CcEventInfo>		_pendingCcApplied;
	vector<Protocol::CcEventInfo>		_pendingCcExpired;
	vector<Protocol::KnockbackInfo>		_pendingKnockbacks;
	int32 _ccStateTickCounter = 0;

	// 스킬 로스터 1 단계 - 상태 효과가 있는 대상 · 다단 히트 · S_STATUS
	unordered_set<uint64>				_statusTargets;
	vector<PendingHit>					_pendingHits;
	vector<Protocol::StatusInfo>		_pendingStatusApplied;
	vector<Protocol::StatusInfo>		_pendingStatusExpired;

	// 2 단계 - 날아가는 투사체 · S_PROJECTILE
	vector<Projectile>					_projectiles;
	vector<Protocol::ProjectileInfo>	_pendingProjectileSpawns;
	vector<Protocol::ProjectileEndInfo>	_pendingProjectileEnds;

	// 3 단계 - 장판 · 유지 중인 시전자 · 지연 폭발 · 창(경독 · 반격) · S_ZONE
	vector<Zone>						_zones;
	uint64								_nextZoneId = 1;
	unordered_set<uint64>				_holders;
	vector<PendingBurst>				_pendingBursts;
	unordered_set<uint64>				_windowOwners;
	vector<Protocol::ZoneInfo>			_pendingZoneSpawns;
	vector<uint64>						_pendingZoneEnds;

	// 4 단계 - 미끼 · 핏자국
	unordered_set<uint64>				_decoys;
	vector<TrailPoint>					_trails;

	// FoW - 갱신마다 다시 채운다 (생물만). 매번 _objects 를 형변환하며 훑지 않게
	vector<CreatureRef>					_fowSubjects;
	vector<Vision::Source>				_fowSources;

	// 2차 로스터 - 임시 벽 (지각변동) · 연사 (그림자 던지기 · 퍼붓기 · 천방지축) · 분신
	struct DynamicWall
	{
		WallBox		box;
		uint64		zoneId = 0;
		uint64		endUs = 0;
		uint64		casterId = 0;
		uint32		effectId = 0;
	};
	struct PendingVolley
	{
		uint64		casterId = 0;
		uint32		skillId = 0;
		uint32		remaining = 0;		// 남은 발사 수 (한 번에 다 쏘면 1)
		uint32		perShot = 1;		// 한 번에 나가는 발 수 (부채꼴)
		float		spreadDeg = 0.f;
		uint32		intervalMs = 0;		// 0 = 한 번에, 아니면 그 간격으로 한 발씩 (바라보는 방향)
		uint64		nextAtUs = 0;
		float		aimDirX = 1.f;		// 부채꼴의 가운데 방향 (발사 순간에 고정)
		float		aimDirY = 0.f;
		bool		followFacing = false;
		SkillContext ctx;
	};
	vector<DynamicWall>					_dynWalls;
	shared_ptr<WallMap>					_wallsWithDynamic;		// 임시 벽이 있을 때만 - 층 벽 + 임시 벽으로 다시 지은 지도
	vector<shared_ptr<WallMap>>			_retiredWalls;			// 바뀌기 전 지도 - 같은 잡 안에서 Walls() 참조를 쥔 쪽이 있을 수 있어 다음 Flush 까지 살려 둔다
	vector<PendingSelfCc>				_pendingSelfCcs;		// 끝날 때 거는 자기 CC
	vector<PathPainter>					_pathPainters;			// 지나간 길 장판
	vector<PendingSlam>					_pendingSlams;			// 저 멀리 - 벽에 부딪히는 순간
	unordered_map<uint64, uint64>		_trailBuffPushedUs;		// 핏자국 이속 버프 - 밟는 동안 S_STATUS 를 0.5 s 에 한 번만
	vector<PendingVolley>				_volleys;
	unordered_set<uint64>				_clones;

	// 3차 로스터 (2026-10-11) - 도착 CC (추격) · 착지 (착지 충격)
	vector<PendingCc>					_pendingCcs;
	vector<PendingLanding>				_pendingLandings;
};

constexpr int32 FLOOR_COUNT = 4;

struct SpawnPoint { float x; float y; };

constexpr SpawnPoint FLOOR_SPAWN[FLOOR_COUNT] =
{
	{0.f, 0.f},
	{0.f, 0.f},
	{0.f, 0.f},
	{0.f, 0.f}
};

extern RoomRef GRooms[FLOOR_COUNT];

RoomRef GetRoomForFloor(uint32 floorId);
