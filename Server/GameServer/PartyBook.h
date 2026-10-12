#pragma once

/*-------------
	PartyBook
-------------*/

// PVE 협동 파티의 장부 (ADR-0014 · design/quick-specs/pve-modes-2026-10-11.md). 순수 로직 - 세션 · 패킷 · 락이 없다.
// PartyManager 가 락으로 감싸고 패킷을 보낸다. GameServerTests 가 이것만 따로 검사한다.
//
//   - 최대 3명. 초대는 파티장만 (파티가 없는 사람이 초대하면 그 사람이 파티장인 새 파티)
//   - 초대받는 사람은 2명 이상 파티에 있으면 안 된다. 혼자인 파티(협동 혼자 입장)는 수락하면 풀린다
//   - 초대는 INVITE_LIFETIME_US 뒤 사라진다. 한 사람에게 걸린 초대는 하나 - 새 초대가 옛것을 덮지 않는다(BUSY)
//   - 파티장이 나가면 가장 먼저 들어온 사람이 이어받는다. 아무도 없으면 파티가 사라진다
//   - 런 중에 끊긴 사람은 "접속 끊김" 으로 남는다 - 인스턴스가 닫히면 빠진다. 베이스캠프에서 끊기면 바로 빠진다

// Protocol::PartyResult 와 같은 값 (PartyManager.cpp 의 static_assert)
enum class PartyBookResult : uint8
{
	Ok = 0,
	NotFound = 1,
	Full = 2,
	AlreadyInParty = 3,
	Declined = 4,
	Expired = 5,
	NotLeader = 6,
	Busy = 7,
	Self = 8,
	NoInstance = 9,
	AlreadyEntered = 10,
	CoopOnly = 11,
	NotVisible = 12,		// 던전 초대 - 같은 방이 아니거나 시야 밖 (party-betrayal 2026-10-12)
	NoKickInRun = 13,		// 던전 파티는 추방이 없다 - 나가는 것만 (= 배신)
};

struct PartyMemberEntry
{
	uint64	characterId = 0;
	string	name;
	bool	online = true;
	bool	inRun = false;		// 파티 인스턴스 안에 있다 (입장 ~ 퇴장 · 탈출 · 사망 퇴장)
};

struct PartyEntry
{
	uint64						partyId = 0;
	uint64						leaderId = 0;
	vector<PartyMemberEntry>	members;			// 들어온 순서 - 파티장 이어받기 순서
	uint64						instanceId = 0;		// 살아 있는 파티 인스턴스 (0 = 없음)
	uint64						dungeonRoom = 0;	// PvP 층 방 안에서 맺은 던전 파티 (Room::Key) - 0 = 베이스캠프 파티 (party-betrayal 2026-10-12)

	const PartyMemberEntry*	Member(uint64 characterId) const;
	PartyMemberEntry*		Member(uint64 characterId);
};

struct PartyInvite
{
	uint64	partyId = 0;
	uint64	fromId = 0;
	string	fromName;
	uint64	expiresUs = 0;
};

class PartyBook
{
public:
	static constexpr size_t	MAX_MEMBERS = 3;
	static constexpr uint64	INVITE_LIFETIME_US = 30'000'000;		// 30 s (Tuning Knob party_invite_s)

	// from 의 파티로 to 를 초대한다. from 이 파티가 없으면 from 이 파티장인 새 파티를 만든다. outPartyId = 초대한 파티
	PartyBookResult	Invite(uint64 from, const string& fromName, uint64 to, uint64 nowUs, OUT uint64& outPartyId);

	// to 가 partyId 의 초대에 답한다. outInviterId = 초대한 사람 (거절 안내용)
	PartyBookResult	Respond(uint64 to, const string& toName, uint64 partyId, bool accept, uint64 nowUs, OUT uint64& outInviterId);

	// by == who 면 스스로 나간다, 아니면 파티장 by 가 who 를 내보낸다. outPartyId = 나간 파티
	PartyBookResult	Leave(uint64 by, uint64 who, OUT uint64& outPartyId);

	// 혼자 협동에 들어갈 때 - 파티가 없으면 1인 파티를 만든다. 있으면 그 파티
	uint64			EnsureParty(uint64 who, const string& name);

	// 만료된 초대를 지운다. (초대받은 사람, 초대한 사람) 목록
	void			ExpireInvites(uint64 nowUs, OUT vector<pair<uint64, uint64>>& outExpired);

	// 끊김 - 런 중이면 접속 끊김으로 남기고, 아니면 파티에서 뺀다. 그 사람이 받은 초대도 지운다. 바뀐 파티 id (0 = 없음)
	uint64			OnDisconnected(uint64 who);
	// 다시 로그인 - 접속 끊김으로 남아 있었으면 되살린다. 그 파티 id (0 = 없음)
	uint64			OnLogin(uint64 who);

	void			SetInRun(uint64 who, bool inRun);
	void			BindInstance(uint64 partyId, uint64 instanceId);
	void			BindDungeonRoom(uint64 partyId, uint64 roomKey);	// 던전 파티 표시 - 나가면 배신, 런이 끝나면 조용히 빠진다
	// 던전 파티인데 혼자 남았다 (배신당한 쪽 · 수락 전 파티장) - 나가도 피해자가 없으니 배신이 아니다 (R5 "1 명이면 해산")
	bool			IsSoloDungeonParty(uint64 who) const;
	// 인스턴스가 닫혔다 - 묶음을 풀고 모두 베이스캠프로, 접속 끊김은 뺀다. 파티가 남아 있으면 true
	bool			OnInstanceClosed(uint64 partyId, uint64 instanceId);

	const PartyEntry*	PartyOf(uint64 who) const;
	const PartyEntry*	Find(uint64 partyId) const;
	const PartyInvite*	InviteFor(uint64 to) const;
	size_t				SizeOf(uint64 who) const;		// 0 = 파티 없음
	bool				IsLeader(uint64 who) const;
	size_t				PartyCount() const { return _parties.size(); }

private:
	PartyEntry*		MutablePartyOf(uint64 who);
	void			RemoveMember(PartyEntry& party, uint64 who);		// 파티장 이어받기 · 빈 파티 삭제까지

	unordered_map<uint64, PartyEntry>	_parties;
	unordered_map<uint64, uint64>		_partyOf;		// 캐릭터 → 파티
	unordered_map<uint64, PartyInvite>	_invites;		// 초대받은 캐릭터 → 초대
	uint64								_nextPartyId = 1;
};
