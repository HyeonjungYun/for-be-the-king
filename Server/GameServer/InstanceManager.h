#pragma once
#include "Room.h"

/*-------------------
	InstanceManager
-------------------*/

// PVE 인스턴스 방 등록부 (ADR-0014). 층 방(GRooms)과 따로 둔다 - FLOOR_COUNT · GetRoomForFloor 를 더럽히지 않는다 (ADR-0005).
//
// 락은 등록부만 지킨다. 방 상태는 방 큐 안에서만 (ADR-0007) - 여기서는 DoAsync 로만 들어간다.
// "입장 예약" 이 닫힘과 입장의 경주를 막는다: 입장(DB 적재 중)이 예약을 쥔 동안 방은 TryRetire 에 실패해 닫히지 않는다.
class InstanceManager
{
public:
	// 새 인스턴스 - 층 floorId 의 레벨 · 몬스터 · 상자 · 타워를 그대로 들고 4 루프를 돌린다. 입장 예약 하나를 쥔 채 돌려준다
	RoomRef	Create(RoomKind kind, uint32 floorId, uint64 partyId, uint64 teamId);

	// 살아 있는 인스턴스에 입장 예약 - 없거나 닫혔으면 nullptr
	RoomRef	ReserveJoin(uint64 instanceId);
	void	ReleaseJoin(uint64 instanceId);

	// 방 스레드에서 - 예약이 하나도 없으면 등록부에서 빼고 true. 그 뒤로는 ReserveJoin 이 nullptr 이다
	bool	TryRetire(uint64 instanceId);

	int32	LiveCount();
	uint64	CreatedCount();

	// 정상 종료 (2026-10-21) - 살아 있는 인스턴스 전부에. 락 안에서 사본을 뜨고 락 밖에서 부른다 (fn 이 DoAsync 만 하게)
	void	ForEachRoom(const function<void(const RoomRef&)>& fn);

private:
	USE_LOCK;

	struct Entry
	{
		RoomRef	room;
		int32	pendingJoins = 0;
	};

	unordered_map<uint64, Entry>	_rooms;
	uint64							_nextId = 1;
	uint64							_created = 0;
};

extern InstanceManager GInstances;

// 인스턴스에도 몬스터를 낳을까 - FBTK_MONSTERS=0 (G3 비교) 이면 층 방처럼 끈다. 부팅이 정한다
extern atomic<bool> GMonstersOn;
