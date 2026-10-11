#include "pch.h"
#include "InstanceManager.h"

InstanceManager GInstances;
atomic<bool> GMonstersOn = true;

RoomRef InstanceManager::Create(RoomKind kind, uint32 floorId, uint64 partyId, uint64 teamId)
{
	RoomRef room = make_shared<Room>();
	room->_floorId = floorId;
	room->_kind = kind;
	room->_partyId = partyId;
	room->_teamId = teamId;

	{
		WRITE_LOCK;
		const uint64 instanceId = _nextId++;
		room->_instanceId = instanceId;
		_rooms[instanceId] = Entry{ room, 1 };		// 만든 사람의 입장 예약
		_created++;
	}

	// 방 큐로 - 이 스레드가 잡을 돌리고 있지 않으면 바로 실행된다(층 방 부팅과 같다). 몬스터 · 상자 · 타워가 루프보다 먼저
	room->DoAsync(&Room::InitInstance);
	room->DoAsync(&Room::StartLoops);
	return room;
}

RoomRef InstanceManager::ReserveJoin(uint64 instanceId)
{
	WRITE_LOCK;
	auto it = _rooms.find(instanceId);
	if (it == _rooms.end())
		return nullptr;

	it->second.pendingJoins++;
	return it->second.room;
}

void InstanceManager::ReleaseJoin(uint64 instanceId)
{
	WRITE_LOCK;
	auto it = _rooms.find(instanceId);
	if (it != _rooms.end() && it->second.pendingJoins > 0)
		it->second.pendingJoins--;
}

bool InstanceManager::TryRetire(uint64 instanceId)
{
	WRITE_LOCK;
	auto it = _rooms.find(instanceId);
	if (it == _rooms.end())
		return true;		// 이미 빠졌다
	if (it->second.pendingJoins > 0)
		return false;

	_rooms.erase(it);
	return true;
}

int32 InstanceManager::LiveCount()
{
	WRITE_LOCK;
	return static_cast<int32>(_rooms.size());
}

uint64 InstanceManager::CreatedCount()
{
	WRITE_LOCK;
	return _created;
}
