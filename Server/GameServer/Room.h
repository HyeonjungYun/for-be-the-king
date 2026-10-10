#pragma once
#include "JobQueue.h"
#include "SkillTable.h"
#include "StatusEffects.h"
#include "WallMap.h"
#include "MonsterBrain.h"
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

// 몬스터 자리 하나 (monster-ai.md § States - Alive(id) ↔ DeadWaiting(respawnAtUs))
struct MonsterSlot
{
	const MonsterSpawnDef*	spawn = nullptr;
	uint64					monsterId = 0;		// 0 = 죽어서 기다린다
	uint64					respawnAtUs = 0;
	uint64					deferStartUs = 0;	// 곁에 플레이어가 있어 리스폰을 미룬 시작 (F9)
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
};

// 날아가는 투사체 (skill-roster 2 단계). 스킬 투사체는 직선 · 피할 수 있다 · 벽에 막힌다 · 관통이면 지나간 적을 다 맞힌다.
// 평타 투사체(활 · 스태프)는 대상을 쫓아간다(필중) - 날아가는 중 벽에 막히면 끝 (사용자 결정 2026-10-10). FlushCombat 마다 StepProjectiles
constexpr uint64 PROJECTILE_ID_BASE = 5'000'000'000;
extern atomic<uint64> GNextProjectileId;

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
};

class Room : public JobQueue
{
public:
			Room();
	virtual ~Room();

	bool	EnterRoom(ObjectRef object, bool randPos = true);
	bool	LeaveRoom(ObjectRef object);

	bool	HandleEnterPlayer(PlayerRef player);
	bool	HandleLeavePlayer(PlayerRef player);
	void	HandleMove(Protocol::C_MOVE pkt);
	void	HandleAttack(uint64 attackerId, uint64 targetId);
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

	// 모든 피해는 여기를 지난다 - 치명상 방어(맹수의 본능) → 체력 차감 → S_DAMAGE → 몬스터 어그로 → 사망. 돌려주는 값 = 남은 체력.
	// attacker 는 없을 수 있다 (떠난 사람의 지속 피해) - attackerId 만 패킷에 실린다. 이미 죽은 대상은 건드리지 않는다
	int32	DealDamage(const CreatureRef& attacker, uint64 attackerId, const CreatureRef& target, int32 damage, Protocol::DamageType type, bool isCrit, uint64 nowUs);
	bool	TryLethalSave(const PlayerRef& victim, const CreatureRef& attacker, OUT int32& damage, uint64 nowUs);

	// 서버가 옮기는 변위 - 위치를 끝점으로 바로 놓고(S_MOVE) 날아가는 동안 넉백 CC. 클라는 KnockbackInfo 로 같은 끝점까지 밀려간다.
	// 그동안의 C_MOVE 는 HandleMove 가 무시한다 (서버가 위치를 소유하는 구간). 벽에 닿으면 wallStun 일 때 벽 기절
	void	PushCreature(const CreatureRef& target, uint64 instigatorId, float dirX, float dirY, float distCm, float speedCms, bool wallStun, uint64 nowUs);
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
	bool	TryParry(const CreatureRef& target, float fromX, float fromY, uint64 nowUs);							// true = 받아넘겼다
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

	// 탈출 (escape-timer.md § Core Rules). UpdateTick 에서 게이지 → 타이머 → (기존) 끊긴 몸 순서
	void	UpdateEscapeTowers(uint64 nowUs);
	void	UpdateRunTimers(uint64 nowUs);
	void	BeginExpiryCommit(const PlayerRef& player, uint64 nowUs);
	void	SendGauge(const PlayerRef& player, uint32 towerWireId, Protocol::EscapeGaugeState state, uint32 permille, float ratePerS, bool selfInZone);

	// 런 하나가 끝났다 - runs.csv 한 줄 (§ G). reason: ESCAPED · EXPIRED · DIED · DISCONNECT_DEATH
	void	WriteRunTelemetry(const PlayerRef& player, const char* reason, uint64 killerId, uint64 goldKept, uint64 nowUs);

public:
	uint32								_floorId = 0;

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
