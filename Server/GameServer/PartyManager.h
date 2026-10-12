#pragma once
#include "PartyBook.h"
#include "GameSession.h"
#include "Enum.pb.h"

class Room;
using RoomRef = shared_ptr<Room>;

/*----------------
	PartyManager
----------------*/

// PVE 협동 파티 (ADR-0014). PartyBook(순수 장부)을 락으로 감싸고, 세션을 찾아 S_PARTY_* 를 보낸다.
// 규칙: 락 안에서는 장부를 계산하고, 송신은 락 밖에서. 방에는 동기로 들어가지 않는다 (ADR-0007 지침 1).
// 락 순서: 파티 → (세션 관리자 · 인스턴스 등록부 · 사망 커밋 · 끊긴 몸). 이들은 파티 락을 잡지 않는다 - 반대로 잡는 길이 없다
class PartyManager
{
public:
	// C_PARTY_INVITE - 초대하는 사람은 베이스캠프, 또는 자기 협동 인스턴스 안. 받는 사람은 베이스캠프 (이름으로 찾는다)
	void	Invite(const GameSessionRef& from, const string& targetName);
	void	Respond(const GameSessionRef& to, uint64 partyId, bool accept);
	void	Leave(const GameSessionRef& by, uint64 who);

	void	OnLogin(const GameSessionRef& session);
	void	OnDisconnected(uint64 characterId, SessionPlace place);
	// 멤버가 런에 들어가거나 나왔다 - 파티 상태를 다시 보낸다
	void	NotifyChanged(uint64 characterId);
	void	OnInstanceClosed(uint64 partyId, uint64 instanceId);
	// 초대 만료 - 메인 루프가 주기적으로
	void	Tick(uint64 nowUs);

	// 한 사람에게만 결과 안내 (S_PARTY_STATE.result) - 입장 거절 사유 등
	void	SendNotice(uint64 characterId, Protocol::PartyResult result, const string& subject = string());

	// 던전 파티 (party-betrayal-2026-10-12): Invite 가 PvP 층 방의 상대를 찾으면 Room::CheckPartyInvite 에 넘기고, 방 잡이 시야 · 생존을 본 뒤 여기로 돌아온다
	void	FinishDungeonInvite(uint64 fromId, uint64 toId, bool visible, uint64 roomKey);

	// C_ENTER_GAME mode=PVE_COOP. 파티장이면 인스턴스를 만들고(없으면) 베이스캠프의 멤버를 함께 데려간다 (각자 입장 예약을 쥔다).
	// 인스턴스가 살아 있으면 합류. room == nullptr 이면 result 가 거절 이유
	struct CoopEnter
	{
		RoomRef						room;
		Protocol::PartyResult		result = Protocol::PARTY_OK;
		vector<GameSessionRef>		pulls;
		uint64						partyId = 0;
	};
	CoopEnter	BeginCoopEnter(const GameSessionRef& requester, uint32 floorId);

	size_t	SizeOf(uint64 characterId);
	uint64	PartyOf(uint64 characterId);

private:
	struct Snapshot
	{
		PartyEntry	party;			// partyId 0 = 파티가 없다
	};

	// 락 안에서 - 보낼 파티 상태를 복사한다
	Snapshot	TakeSnapshot(uint64 partyId);
	// 락 밖에서 - 멤버 전원(또는 한 사람)에게 상태를 보낸다
	void		Broadcast(const Snapshot& snap, uint64 resultTo = 0, Protocol::PartyResult result = Protocol::PARTY_OK, const string& subject = string());
	void		SendTo(const GameSessionRef& session, const Snapshot& snap, Protocol::PartyResult result, const string& subject);

	USE_LOCK;
	PartyBook	_book;
};

extern PartyManager GParties;
