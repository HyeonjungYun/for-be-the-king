#pragma once
#include "JobQueue.h"
#include "SkillTable.h"
#include "MonsterBrain.h"
#include <random>

struct DeathSnapshot;

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
