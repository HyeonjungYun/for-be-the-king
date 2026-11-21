#include "pch.h"
#include "InstanceManager.h"

InstanceManager GInstances;
atomic<bool> GMonstersOn = true;
atomic<int32> GRoomCap = 30;
atomic<int32> GLoginInflight = 64;

// 락 안에서 - 등록만. 초기화 잡은 락 밖에서 건다 (DoAsync 가 그 자리에서 방을 돌릴 수 있다)
RoomRef InstanceManager::CreateLocked(RoomKind kind, uint32 floorId, uint64 partyId, uint64 teamId, uint32 channel)
{
	if (_maxRooms > 0 && static_cast<int32>(_rooms.size()) >= _maxRooms)
		return nullptr;

	RoomRef room = make_shared<Room>();
	const uint64 instanceId = _nextId++;
	room->InitIdentity(floorId, kind, instanceId, partyId, teamId, channel);		// 만들 때 한 번 - 이후 읽기만 (Room 의 신원 필드는 private, 2026-10-21)
	_rooms[instanceId] = Entry{ room, 1 };		// 만든 사람의 입장 예약
	_created++;
	return room;
}

RoomRef InstanceManager::Create(RoomKind kind, uint32 floorId, uint64 partyId, uint64 teamId)
{
	RoomRef room;
	{
		WRITE_LOCK_ORDERED(LockLevel::REGISTRY);
		room = CreateLocked(kind, floorId, partyId, teamId, 0);
	}
	if (room == nullptr)
		return nullptr;

	// 방 큐로 - 이 스레드가 잡을 돌리고 있지 않으면 바로 실행된다(층 방 부팅과 같다). 몬스터 · 상자 · 타워가 루프보다 먼저
	room->DoAsync(&Room::InitInstance);
	room->DoAsync(&Room::StartLoops);
	return room;
}

RoomRef InstanceManager::PickWorld(uint32 floorId, int32 cap)
{
	RoomRef first = GetRoomForFloor(floorId);
	if (first == nullptr)
		return nullptr;

	RoomRef created;
	{
		WRITE_LOCK_ORDERED(LockLevel::REGISTRY);

		// 채널 1 - 층 방. 인원 = 방 안 플레이어 몸 + 들어오는 중(예약)
		if (cap <= 0 || first->PlayerCount() + _floorPending[floorId] < cap)
		{
			_floorPending[floorId]++;
			return first;
		}

		// 채널 2.. - 닫혀 빠진 것은 목록에서도 지운다
		vector<uint64>& channels = _channels[floorId];
		for (auto it = channels.begin(); it != channels.end();)
		{
			auto found = _rooms.find(*it);
			if (found == _rooms.end())
			{
				it = channels.erase(it);
				continue;
			}
			Entry& entry = found->second;
			if (entry.room->PlayerCount() + entry.pendingJoins < cap)
			{
				entry.pendingJoins++;
				return entry.room;
			}
			++it;
		}

		// 다 찼다 - 비어 있는 가장 낮은 번호로 새 채널
		uint32 number = 2;
		for (bool taken = true; taken; )
		{
			taken = false;
			for (uint64 id : channels)
			{
				auto found = _rooms.find(id);
				if (found != _rooms.end() && found->second.room->Channel() == number)
				{
					taken = true;
					number++;
					break;
				}
			}
		}

		created = CreateLocked(RoomKind::World, floorId, 0, 0, number);
		if (created == nullptr)
			return nullptr;
		channels.push_back(created->InstanceId());
		sort(channels.begin(), channels.end(), [this](uint64 a, uint64 b)
			{
				return _rooms[a].room->Channel() < _rooms[b].room->Channel();
			});
	}

	created->DoAsync(&Room::InitInstance);
	created->DoAsync(&Room::StartLoops);
	return created;
}

void InstanceManager::ReleaseJoinOf(const Room& room)
{
	if (room.InstanceId() != 0)
	{
		ReleaseJoin(room.InstanceId());
		return;
	}

	// 채널 1 (층 방) - 예약만 센다. 베이스캠프 등 다른 층 방 경로는 PickWorld 를 거치지 않으므로 0 아래로 내려가지 않게
	if (room.Kind() != RoomKind::World || room.FloorId() >= FLOOR_COUNT)
		return;
	WRITE_LOCK_ORDERED(LockLevel::REGISTRY);
	if (_floorPending[room.FloorId()] > 0)
		_floorPending[room.FloorId()]--;
}

int32 InstanceManager::ChannelCount(uint32 floorId)
{
	if (floorId >= FLOOR_COUNT)
		return 0;
	WRITE_LOCK_ORDERED(LockLevel::REGISTRY);
	int32 live = 1;
	for (uint64 id : _channels[floorId])
		live += _rooms.contains(id) ? 1 : 0;
	return live;
}

RoomRef InstanceManager::ReserveJoin(uint64 instanceId)
{
	WRITE_LOCK_ORDERED(LockLevel::REGISTRY);
	auto it = _rooms.find(instanceId);
	if (it == _rooms.end())
		return nullptr;

	it->second.pendingJoins++;
	return it->second.room;
}

void InstanceManager::ReleaseJoin(uint64 instanceId)
{
	WRITE_LOCK_ORDERED(LockLevel::REGISTRY);
	auto it = _rooms.find(instanceId);
	if (it != _rooms.end() && it->second.pendingJoins > 0)
		it->second.pendingJoins--;
}

bool InstanceManager::TryRetire(uint64 instanceId)
{
	WRITE_LOCK_ORDERED(LockLevel::REGISTRY);
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
	WRITE_LOCK_ORDERED(LockLevel::REGISTRY);
	return static_cast<int32>(_rooms.size());
}

uint64 InstanceManager::CreatedCount()
{
	WRITE_LOCK_ORDERED(LockLevel::REGISTRY);
	return _created;
}

void InstanceManager::ForEachRoom(const function<void(const RoomRef&)>& fn)
{
	vector<RoomRef> rooms;
	{
		WRITE_LOCK_ORDERED(LockLevel::REGISTRY);
		rooms.reserve(_rooms.size());
		for (const auto& [instanceId, entry] : _rooms)
			rooms.push_back(entry.room);
	}

	for (const RoomRef& room : rooms)
		fn(room);
}
