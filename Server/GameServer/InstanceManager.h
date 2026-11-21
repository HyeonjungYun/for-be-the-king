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
	// 새 인스턴스 - 층 floorId 의 레벨 · 몬스터 · 상자 · 타워를 그대로 들고 4 루프를 돌린다. 입장 예약 하나를 쥔 채 돌려준다.
	// 동적 방 상한(SetMaxRooms)이 찼으면 nullptr - 부르는 쪽이 입장을 거절한다
	RoomRef	Create(RoomKind kind, uint32 floorId, uint64 partyId, uint64 teamId);

	// PvP 층 채널 (2026-11-21 동접 1만 - docs/server-scaling-10k.md § 2-2). 층 방(GRooms[floor]) = 채널 1, 차면 채널 2, 3 … 을
	// 이 등록부의 World 방으로 연다. 사람이 있는 낮은 번호부터 채운다(한 방에 사람이 모이게). 입장 예약 하나를 쥔 채 돌려준다 -
	// 입장이 끝나면(들어왔든 못 들어왔든) ReleaseJoinOf. cap 0 = 예전처럼 층 방 하나. 상한이 차 새 채널을 못 열면 nullptr
	RoomRef	PickWorld(uint32 floorId, int32 cap);
	void	ReleaseJoinOf(const Room& room);
	int32	ChannelCount(uint32 floorId);
	void	SetMaxRooms(int32 maxRooms) { _maxRooms = maxRooms; }

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
	int32							_maxRooms = 0;		// 0 = 상한 없음

	// 채널 (2026-11-21) - 층마다 채널 2.. 의 instanceId (번호 순서). 채널 1 = GRooms[floor] 는 _rooms 밖이라 입장 예약만 따로 센다
	vector<uint64>					_channels[FLOOR_COUNT];
	int32							_floorPending[FLOOR_COUNT] = {};

	RoomRef	CreateLocked(RoomKind kind, uint32 floorId, uint64 partyId, uint64 teamId, uint32 channel);
};

extern InstanceManager GInstances;

// PvP 층 채널 인원 (FBTK_ROOM_CAP, 2026-11-21). 0 = 층마다 방 하나 (예전). 부팅이 정한다
extern atomic<int32> GRoomCap;

// 로그인 대기열 상한 (FBTK_LOGIN_INFLIGHT, 2026-11-21) - 부팅이 정한다. 0 = 대기열 없음. 기다리는 수는 통계 줄에
extern atomic<int32> GLoginInflight;
int32 LoginQueueWaiting();

// 인스턴스에도 몬스터를 낳을까 - FBTK_MONSTERS=0 (G3 비교) 이면 층 방처럼 끈다. 부팅이 정한다
extern atomic<bool> GMonstersOn;
