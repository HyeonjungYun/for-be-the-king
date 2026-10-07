#pragma once
#include "JobQueue.h"
#include "SkillTable.h"
#include "MonsterBrain.h"
#include "EscapeLogic.h"
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
	void	ApplySkillEffects(const CreatureRef& caster, const SkillDef& def, const vector<CreatureRef>& targets, uint64 nowUs);
	void	ApplyMovement(const CreatureRef& caster, const SkillEffect& effect, uint64 nowUs);

	void	SavePlayer(PlayerRef player);
	void	UpdateSaves(uint64 nowUs);
	void	UpdateBagExpiry(uint64 nowUs);
	void	UpdateChests(uint64 nowUs);
	
	void	FindSpawnPosition(float baseX, float baseY, float& outX, float& outY);

	// 사망 처리 공용 - S_DIED · 원인(가해자 기준) · 시전 취소, 그다음 몬스터면 드랍 · 플레이어면 사망 커밋
	void	OnCreatureKilled(const CreatureRef& victim, const CreatureRef& instigator, uint64 instigatorId, uint64 nowUs);

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

	unordered_map<uint64, MonsterRef>	_monsters;			// 시체 유예 중인 것 포함
	vector<MonsterSlot>					_monsterSlots;
	vector<BrainPlayer>					_brainPlayers;		// 판단 틱마다 다시 채운다 - 몬스터마다 _objects 를 훑지 않게

	vector<EscapeTowerState>			_towers;
	uint64								_lastEscapeTickUs = 0;

	unordered_set<uint64>				_ccTargets;
	vector<Protocol::CcEventInfo>		_pendingCcApplied;
	vector<Protocol::CcEventInfo>		_pendingCcExpired;
	int32 _ccStateTickCounter = 0;
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
