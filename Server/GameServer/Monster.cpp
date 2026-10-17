#include "pch.h"
#include "Monster.h"

atomic<uint64> GNextMonsterId = MONSTER_ID_BASE;
MonsterStats GMonsterStats;

void MonsterStats::RecordAi(uint64 micros)
{
	aiTicks.fetch_add(1);
	aiTotalUs.fetch_add(micros);

	uint64 prev = aiMaxUs.load();
	while (micros > prev && aiMaxUs.compare_exchange_weak(prev, micros) == false)
	{
	}
}

void MonsterStats::RecordPath(bool found, uint64 micros)
{
	paths.fetch_add(1);
	if (found == false)
		pathFails.fetch_add(1);
	pathTotalUs.fetch_add(micros);

	uint64 prev = pathMaxUs.load();
	while (micros > prev && pathMaxUs.compare_exchange_weak(prev, micros) == false)
	{
	}
}

void MonsterStats::Dump(uint64 windowSeconds)
{
	const uint64 ticks = aiTicks.exchange(0);
	const uint64 totalUs = aiTotalUs.exchange(0);
	const uint64 maxUs = aiMaxUs.exchange(0);
	const uint64 entries = moveEntries.exchange(0);

	// 길찾기 (§ K) - 몇 번 구했고 얼마나 실패했나 · 한 번의 비용
	const uint64 pathCount = paths.exchange(0);
	const uint64 pathUs = pathTotalUs.exchange(0);

	if (ticks == 0 && alive.load() == 0)
		return;

	LOG_LINE << "[MON] alive=" << alive.load()
		<< " ai_avg=" << (ticks > 0 ? totalUs / ticks : 0) << "us ai_max=" << maxUs << "us"
		<< " move_entries/s=" << (windowSeconds > 0 ? entries / windowSeconds : entries)
		<< " hits=" << hits.exchange(0) << " player_kills=" << playerKills.exchange(0)
		<< " deaths=" << deaths.exchange(0) << " reset_snap=" << resetSnaps.exchange(0)
		<< " paths=" << pathCount << " path_fail=" << pathFails.exchange(0)
		<< " path_avg=" << (pathCount > 0 ? pathUs / pathCount : 0) << "us path_max=" << pathMaxUs.exchange(0) << "us"
		<< " on_path=" << onPathSteps.exchange(0) << " casts=" << casts.exchange(0) << '\n';
}

Monster::Monster(uint64 objectId, const MonsterTypeDef& typeDef, const MonsterSpawnDef& spawnDef)
	: type(&typeDef), spawn(&spawnDef)
{
	_isMonster = true;

	maxHp = typeDef.maxHp;
	hp = typeDef.maxHp;
	attackPower = typeDef.attackPower;
	attackSpeed = typeDef.attackSpeed;
	attackRange = typeDef.attackRangeCm;
	armor = typeDef.armor;
	magicResist = typeDef.magicResist;
	resistance = typeDef.resistance;

	objectInfo->set_object_id(objectId);
	objectInfo->set_creature_type(Protocol::CREATE_TYPE_MONSTER);
	objectInfo->set_monster_type_id(typeDef.typeId);
	objectInfo->set_hp(hp);
	objectInfo->set_max_hp(maxHp);

	posInfo->set_object_id(objectId);
	posInfo->set_x(spawnDef.x);
	posInfo->set_y(spawnDef.y);
	posInfo->set_z(spawnDef.z);
	posInfo->set_yaw(spawnDef.yaw);
	posInfo->set_state(Protocol::MOVE_STATE_IDLE);
}

Monster::~Monster()
{
}

void Monster::RestoreFull()
{
	hp = maxHp;
	ClearCombatState();
}
