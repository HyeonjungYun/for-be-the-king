#pragma once
#include "JobQueue.h"
#include "SkillTable.h"

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

	void	CollectSkillTargets(const CreatureRef& caster, const SkillDef& def, const float originX, float originY, uint64 castTargetId, float aimX, float aimY, OUT vector<CreatureRef>& outTargets);
	void	ApplySkillEffects(const CreatureRef& caster, const SkillDef& def, const vector<CreatureRef>& targets, uint64 nowUs);
	void	ApplyMovement(const CreatureRef& caster, const SkillEffect& effect, uint64 nowUs);

	void	SavePlayer(PlayerRef player);
	void	UpdateSaves(uint64 nowUs);
	
	void	FindSpawnPosition(float baseX, float baseY, float& outX, float& outY);

public:
	uint32								_floorId = 0;

private:
	unordered_map<uint64, ObjectRef>	_objects;
	unordered_set<uint64>				_dirtyMovers;

	vector<uint64>						_pendingSpawns;
	vector<PendingTelegraph>			_pendingTelegraphs;

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
