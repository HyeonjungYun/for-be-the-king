#include "pch.h"
#include "PartyManager.h"
#include "GameSessionManager.h"
#include "InstanceManager.h"
#include "ObjectUtils.h"
#include "Player.h"
#include "Room.h"
#include "ServerPacketHandler.h"

PartyManager GParties;

// 장부의 결과 = 와이어 값 (Enum.proto PartyResult)
static_assert(static_cast<int32>(PartyBookResult::Ok) == Protocol::PARTY_OK
	&& static_cast<int32>(PartyBookResult::NotFound) == Protocol::PARTY_NOT_FOUND
	&& static_cast<int32>(PartyBookResult::Full) == Protocol::PARTY_FULL
	&& static_cast<int32>(PartyBookResult::AlreadyInParty) == Protocol::PARTY_ALREADY_IN_PARTY
	&& static_cast<int32>(PartyBookResult::Declined) == Protocol::PARTY_DECLINED
	&& static_cast<int32>(PartyBookResult::Expired) == Protocol::PARTY_EXPIRED
	&& static_cast<int32>(PartyBookResult::NotLeader) == Protocol::PARTY_NOT_LEADER
	&& static_cast<int32>(PartyBookResult::Busy) == Protocol::PARTY_BUSY
	&& static_cast<int32>(PartyBookResult::Self) == Protocol::PARTY_SELF
	&& static_cast<int32>(PartyBookResult::NoInstance) == Protocol::PARTY_NO_INSTANCE
	&& static_cast<int32>(PartyBookResult::AlreadyEntered) == Protocol::PARTY_ALREADY_ENTERED
	&& static_cast<int32>(PartyBookResult::CoopOnly) == Protocol::PARTY_COOP_ONLY
	&& static_cast<int32>(PartyBookResult::NotVisible) == Protocol::PARTY_NOT_VISIBLE
	&& static_cast<int32>(PartyBookResult::NoKickInRun) == Protocol::PARTY_NO_KICK_IN_RUN, "PartyBookResult must match Protocol::PartyResult");

namespace
{
	Protocol::PartyResult ToWire(PartyBookResult r)
	{
		return static_cast<Protocol::PartyResult>(static_cast<int32>(r));
	}

	uint64 CharacterOf(const GameSessionRef& session)
	{
		if (session == nullptr || session->accountId.load() == 0 || session->characters.empty())
			return 0;
		return session->characters[0].object_info().object_id();
	}

	string NameOf(const GameSessionRef& session)
	{
		if (session == nullptr || session->accountId.load() == 0 || session->characters.empty())
			return string();
		return session->characters[0].name();
	}

	// 고독한 길 (2026-10-17) - 옵션 아이템을 하나라도 착용 중이면 파티를 맺을 수 없다 (벗으면 다시 된다)
	bool WearsLonePath(const GameSessionRef& session)
	{
		return session != nullptr && session->lonePieces.load() > 0;
	}

	// 런 종료 커밋 중 · 끊긴 몸이 던전에 남았다 - 베이스캠프 화면이어도 아직 자유롭지 않다
	bool IsBusy(uint64 characterId)
	{
		return GDeathCommits.IsPending(characterId) || GDetachedBodies.Contains(characterId);
	}

	// 자기 협동 인스턴스 안에 있다 (던전에서 초대 - D5 "내 서버에 초대")
	bool InOwnCoopInstance(const GameSessionRef& session, uint64 partyId)
	{
		if (partyId == 0 || session->place.load() != SessionPlace::Dungeon)
			return false;
		PlayerRef player = session->player.load();
		if (player == nullptr)
			return false;
		RoomRef room = player->room.load().lock();
		return room != nullptr && room->Kind() == RoomKind::PveCoop && room->PartyId() == partyId;		// 만들 때 정한 값 - 읽기만
	}

	// PvP 층 방 (던전 파티 R2) - 세션의 플레이어가 지금 있는 World 방. 아니면 nullptr
	RoomRef WorldRoomOf(const GameSessionRef& session)
	{
		if (session == nullptr || session->place.load() != SessionPlace::Dungeon)
			return nullptr;
		PlayerRef player = session->player.load();
		if (player == nullptr)
			return nullptr;
		RoomRef room = player->room.load().lock();
		return (room != nullptr && room->Kind() == RoomKind::World) ? room : nullptr;
	}

	Protocol::PartyMemberState StateOf(const PartyMemberEntry& m, const GameSessionRef& session)
	{
		if (m.online == false || session == nullptr)
			return Protocol::PARTY_MEMBER_OFFLINE;
		const SessionPlace place = session->place.load();
		return (place == SessionPlace::BaseCamp || place == SessionPlace::None) ? Protocol::PARTY_MEMBER_BASECAMP : Protocol::PARTY_MEMBER_IN_RUN;
	}
}

void PartyManager::Invite(const GameSessionRef& from, const string& targetName)
{
	const uint64 fromId = CharacterOf(from);
	if (fromId == 0)
		return;

	// 던전 파티 (party-betrayal-2026-10-12 R2): PvP 층 방 안에서 같은 방의 보이는 사람을 이름으로. 살아 있는지 · 시야 안인지는 방 잡이 본다
	if (RoomRef room = WorldRoomOf(from))
	{
		GameSessionRef target = GSessionManager.FindByName(targetName);
		const uint64 toId = CharacterOf(target);
		Protocol::PartyResult early = Protocol::PARTY_OK;
		if (toId == 0)
			early = Protocol::PARTY_NOT_FOUND;
		else if (toId == fromId)
			early = Protocol::PARTY_SELF;
		else if (WorldRoomOf(target) != room)
			early = Protocol::PARTY_NOT_VISIBLE;
		else if (WearsLonePath(from) || WearsLonePath(target))
			early = Protocol::PARTY_LONE_PATH;
		if (early != Protocol::PARTY_OK)
		{
			LOG_LINE << "[PARTY] dungeon invite from=" << fromId << " to='" << targetName << "' (" << toId << ") result=" << Protocol::PartyResult_Name(early) << '\n';
			SendNotice(fromId, early, targetName);
			return;
		}
		room->DoAsync(&Room::CheckPartyInvite, fromId, toId);
		return;
	}

	// 받는 사람 - 베이스캠프에 있어야 한다. 런 종료 커밋 · 끊긴 몸이 있으면 바쁘다
	GameSessionRef target = GSessionManager.FindByName(targetName);
	const uint64 toId = CharacterOf(target);

	Protocol::PartyResult result = Protocol::PARTY_OK;
	uint64 partyId = 0;
	Snapshot snap;
	{
		WRITE_LOCK;
		const uint64 fromParty = _book.PartyOf(fromId) != nullptr ? _book.PartyOf(fromId)->partyId : 0;
		const SessionPlace fromPlace = from->place.load();

		if (fromPlace != SessionPlace::BaseCamp && InOwnCoopInstance(from, fromParty) == false)
			result = Protocol::PARTY_BUSY;
		else if (toId == 0)
			result = Protocol::PARTY_NOT_FOUND;
		else if (toId == fromId)
			result = Protocol::PARTY_SELF;
		else if (target->place.load() != SessionPlace::BaseCamp || IsBusy(toId))
			result = Protocol::PARTY_BUSY;
		else if (WearsLonePath(from) || WearsLonePath(target))
			result = Protocol::PARTY_LONE_PATH;
		else
			result = ToWire(_book.Invite(fromId, NameOf(from), toId, Utils::NowMicroseconds(), OUT partyId));

		snap = TakeSnapshot(_book.PartyOf(fromId) != nullptr ? _book.PartyOf(fromId)->partyId : 0);
	}

	LOG_LINE << "[PARTY] invite from=" << fromId << " to='" << targetName << "' (" << toId << ") result=" << Protocol::PartyResult_Name(result)
		<< " party=" << partyId << '\n';

	if (result == Protocol::PARTY_OK)
	{
		Protocol::S_PARTY_INVITED invited;
		invited.set_party_id(partyId);
		invited.set_from_name(NameOf(from));
		invited.set_expires_ms(static_cast<uint32>(PartyBook::INVITE_LIFETIME_US / 1000));
		SEND_PACKET_DECLARATION(invited);
		target->Send(sendBuffer);
	}

	// 초대한 쪽 - 결과 + 파티 (파티가 막 생겼으면 1명)
	if (snap.party.partyId != 0)
		Broadcast(snap, fromId, result, targetName);
	else
		SendTo(from, snap, result, targetName);
}

void PartyManager::FinishDungeonInvite(uint64 fromId, uint64 toId, bool visible, uint64 roomKey)
{
	GameSessionRef from = GSessionManager.FindByCharacter(fromId);
	GameSessionRef target = GSessionManager.FindByCharacter(toId);
	if (from == nullptr)
		return;

	Protocol::PartyResult result = visible ? Protocol::PARTY_OK : Protocol::PARTY_NOT_VISIBLE;
	uint64 partyId = 0;
	Snapshot snap;
	{
		WRITE_LOCK;
		const PartyEntry* mine = _book.PartyOf(fromId);
		if (result == Protocol::PARTY_OK && target == nullptr)
			result = Protocol::PARTY_NOT_FOUND;
		else if (result == Protocol::PARTY_OK && mine != nullptr && mine->dungeonRoom == 0 && mine->members.size() > 1)
			result = Protocol::PARTY_BUSY;		// 베이스캠프 파티(협동용)를 던전 파티와 섞지 않는다
		else if (result == Protocol::PARTY_OK && mine != nullptr && mine->dungeonRoom != 0 && mine->dungeonRoom != roomKey)
			result = Protocol::PARTY_BUSY;		// 다른 방에서 맺은 던전 파티가 남아 있다 (런 끝에 빠지기 전)
		if (result == Protocol::PARTY_OK)
		{
			result = ToWire(_book.Invite(fromId, NameOf(from), toId, Utils::NowMicroseconds(), OUT partyId));
			if (result == Protocol::PARTY_OK)
				_book.BindDungeonRoom(partyId, roomKey);
		}
		snap = TakeSnapshot(_book.PartyOf(fromId) != nullptr ? _book.PartyOf(fromId)->partyId : 0);
	}

	LOG_LINE << "[PARTY] dungeon invite from=" << fromId << " to=" << toId << " visible=" << (visible ? 1 : 0) << " result=" << Protocol::PartyResult_Name(result)
		<< " party=" << partyId << '\n';

	if (result == Protocol::PARTY_OK)
	{
		Protocol::S_PARTY_INVITED invited;
		invited.set_party_id(partyId);
		invited.set_from_name(NameOf(from));
		invited.set_expires_ms(static_cast<uint32>(PartyBook::INVITE_LIFETIME_US / 1000));
		SEND_PACKET_DECLARATION(invited);
		target->Send(sendBuffer);
	}
	if (snap.party.partyId != 0)
		Broadcast(snap, fromId, result, NameOf(target));
	else
		SendTo(from, snap, result, NameOf(target));
}

void PartyManager::Respond(const GameSessionRef& to, uint64 partyId, bool accept)
{
	const uint64 toId = CharacterOf(to);
	if (toId == 0)
		return;

	// 던전 파티 초대면 받는 사람도 그 방 안에 있어야 한다 (베이스캠프 초대는 베이스캠프에서만 - 기존)
	RoomRef toRoom = WorldRoomOf(to);
	const uint64 toRoomKey = toRoom != nullptr ? toRoom->Key() : 0;

	Protocol::PartyResult result = Protocol::PARTY_OK;
	uint64 inviterId = 0;
	Snapshot joined;
	bool dungeonParty = false;
	{
		WRITE_LOCK;
		const PartyEntry* invitedTo = _book.Find(partyId);
		dungeonParty = invitedTo != nullptr && invitedTo->dungeonRoom != 0;
		const bool placeOk = dungeonParty ? (toRoomKey != 0 && invitedTo->dungeonRoom == toRoomKey)
			: (to->place.load() == SessionPlace::BaseCamp && IsBusy(toId) == false);
		if (accept && placeOk == false)
		{
			// 수락은 베이스캠프에서만 - 초대는 그대로 지운다
			uint64 ignored = 0;
			_book.Respond(toId, NameOf(to), partyId, false, Utils::NowMicroseconds(), OUT ignored);
			result = Protocol::PARTY_BUSY;
		}
		else if (accept && WearsLonePath(to))
		{
			// 고독한 길 - 초대 뒤에 낀 경우 (2026-10-17). 초대는 지운다. 초대한 쪽은 파티에 든 채라 끼는 것 자체가 막힌다
			uint64 ignored = 0;
			_book.Respond(toId, NameOf(to), partyId, false, Utils::NowMicroseconds(), OUT ignored);
			result = Protocol::PARTY_LONE_PATH;
		}
		else
		{
			result = ToWire(_book.Respond(toId, NameOf(to), partyId, accept, Utils::NowMicroseconds(), OUT inviterId));
		}
		joined = TakeSnapshot(partyId);
	}

	LOG_LINE << "[PARTY] " << (accept ? "accept" : "decline") << " char=" << toId << " party=" << partyId
		<< " result=" << Protocol::PartyResult_Name(result) << (dungeonParty ? " dungeon" : "") << '\n';

	if (result == Protocol::PARTY_OK)
	{
		// 던전 파티 - 방 안의 파티원 전원이 같은 팀 (피해 면역 · 시야 공유, R3). 방 잡에서
		if (dungeonParty && accept && toRoom != nullptr)
		{
			vector<uint64> ids;
			for (const PartyMemberEntry& m : joined.party.members)
				ids.push_back(m.characterId);
			toRoom->DoAsync(&Room::SetPartyTeam, partyId, ids);
		}
		Broadcast(joined, toId, Protocol::PARTY_OK, NameOf(to));
		return;
	}

	// 거절 - 초대한 사람에게 알린다. 받는 쪽은 결과 + 자기 파티 상태 (보통 파티 없음 - 이미 다른 파티면 그 상태를 지운다)
	if (result == Protocol::PARTY_DECLINED && joined.party.partyId != 0)
		Broadcast(joined, inviterId, Protocol::PARTY_DECLINED, NameOf(to));
	SendNotice(toId, result);
}

void PartyManager::Leave(const GameSessionRef& by, uint64 who)
{
	const uint64 byId = CharacterOf(by);
	if (byId == 0)
		return;
	if (who == 0)
		who = byId;

	Protocol::PartyResult result = Protocol::PARTY_OK;
	uint64 partyId = 0;
	Snapshot remaining;
	bool betrayal = false;
	RoomRef betrayRoom;
	vector<string> victims;
	uint64 dissolvedId = 0;		// 배신 뒤 혼자 남은 사람 - 파티를 풀고 팀 0 으로 (R5 "1 명이면 해산")
	{
		WRITE_LOCK;
		const PartyEntry* party = _book.PartyOf(who);
		if (party != nullptr && party->dungeonRoom != 0)
		{
			if (byId != who)
				result = Protocol::PARTY_NO_KICK_IN_RUN;		// 던전 파티는 추방이 없다 (R4)
			else if (_book.IsSoloDungeonParty(who) == false)
			{
				// 나가는 사람이 아직 그 방 안이면 배신 (R5). 이미 런이 끝났으면 조용히 빠진다 (R7 · R9).
				// 혼자 남은 파티(배신당한 쪽 · 수락 전 파티장)를 나가는 건 피해자가 없으니 배신이 아니다 - 검증 r1 수정 1
				betrayRoom = WorldRoomOf(by);
				betrayal = betrayRoom != nullptr && betrayRoom->Key() == party->dungeonRoom;
			}
		}
		if (result == Protocol::PARTY_OK)
		{
			result = ToWire(_book.Leave(byId, who, OUT partyId));
			remaining = TakeSnapshot(partyId);
			if (betrayal)
			{
				for (const PartyMemberEntry& m : remaining.party.members)
					victims.push_back(m.name);
				if (remaining.party.members.size() == 1)
				{
					dissolvedId = remaining.party.members.front().characterId;
					uint64 ignored = 0;
					_book.Leave(dissolvedId, dissolvedId, OUT ignored);
					remaining = Snapshot{};
				}
			}
		}
	}

	LOG_LINE << "[PARTY] leave by=" << byId << " who=" << who << " party=" << partyId << " result=" << Protocol::PartyResult_Name(result)
		<< (betrayal ? " BETRAYAL" : "") << (dissolvedId != 0 ? " dissolved" : "") << '\n';

	if (result != Protocol::PARTY_OK)
	{
		// 거절은 내 파티 상태를 그대로 실어 보낸다 - 빈 스냅샷을 보내면 클라가 "파티 없음" 으로 읽어 아군 표시까지 지웠다 (party-betrayal 실기동 08, 추방 거절)
		SendNotice(byId, result);
		return;
	}

	if (betrayal)
	{
		betrayRoom->DoAsync(&Room::PartyBetrayal, who, partyId, NameOf(by), victims);
		if (dissolvedId != 0)
			betrayRoom->DoAsync(&Room::SetPartyTeam, static_cast<uint64>(0), vector<uint64>{ dissolvedId });		// 혼자 남은 사람은 다시 모두의 적
	}

	// 남은 사람들 · 나간 사람 · 풀린 사람 (빈 파티 = 파티 없음)
	if (remaining.party.partyId != 0)
		Broadcast(remaining);
	if (GameSessionRef gone = (who == byId) ? by : GSessionManager.FindByCharacter(who))
		SendTo(gone, Snapshot{}, Protocol::PARTY_OK, string());
	if (dissolvedId != 0)
	{
		if (GameSessionRef last = GSessionManager.FindByCharacter(dissolvedId))
			SendTo(last, Snapshot{}, Protocol::PARTY_OK, string());
	}
}

void PartyManager::OnLogin(const GameSessionRef& session)
{
	const uint64 characterId = CharacterOf(session);
	if (characterId == 0)
		return;

	Snapshot snap;
	{
		WRITE_LOCK;
		snap = TakeSnapshot(_book.OnLogin(characterId));
	}
	if (snap.party.partyId != 0)
		Broadcast(snap);
}

void PartyManager::OnDisconnected(uint64 characterId, SessionPlace place)
{
	if (characterId == 0)
		return;

	Snapshot snap;
	{
		WRITE_LOCK;
		// 던전에서 끊겼다 - 몸이 끊김 유예로 남는다. 인스턴스가 닫힐 때까지 "접속 끊김" 으로 둔다
		if (place == SessionPlace::Dungeon || place == SessionPlace::Entering || place == SessionPlace::Escaping)
			_book.SetInRun(characterId, true);
		snap = TakeSnapshot(_book.OnDisconnected(characterId));
	}
	if (snap.party.partyId != 0)
		Broadcast(snap);
}

void PartyManager::NotifyChanged(uint64 characterId)
{
	Snapshot snap;
	bool dropped = false;
	{
		WRITE_LOCK;
		const PartyEntry* party = _book.PartyOf(characterId);
		if (party == nullptr)
			return;
		uint64 partyId = party->partyId;
		// 던전 파티는 런의 관계 (R9) - 탈출 · 만료 · 퇴장으로 방을 떠났으면 조용히 빠진다 (배신 아님, R7)
		if (party->dungeonRoom != 0)
		{
			GameSessionRef session = GSessionManager.FindByCharacter(characterId);
			const SessionPlace place = session != nullptr ? session->place.load() : SessionPlace::None;
			if (place != SessionPlace::Dungeon)
			{
				uint64 left = 0;
				_book.Leave(characterId, characterId, OUT left);
				dropped = true;
				LOG_LINE << "[PARTY] dungeon party member left the run - dropped char=" << characterId << " party=" << partyId << '\n';
			}
		}
		snap = TakeSnapshot(partyId);
	}
	if (snap.party.partyId != 0)
		Broadcast(snap);
	if (dropped)
	{
		if (GameSessionRef gone = GSessionManager.FindByCharacter(characterId))
			SendTo(gone, Snapshot{}, Protocol::PARTY_OK, string());
	}
}

void PartyManager::OnInstanceClosed(uint64 partyId, uint64 instanceId)
{
	if (partyId == 0)
		return;

	Snapshot snap;
	{
		WRITE_LOCK;
		_book.OnInstanceClosed(partyId, instanceId);
		snap = TakeSnapshot(partyId);
	}
	if (snap.party.partyId != 0)
		Broadcast(snap);
}

void PartyManager::Tick(uint64 nowUs)
{
	vector<pair<uint64, uint64>> expired;
	{
		WRITE_LOCK;
		_book.ExpireInvites(nowUs, OUT expired);
	}

	for (const auto& [toId, fromId] : expired)
	{
		LOG_LINE << "[PARTY] invite expired to=" << toId << " from=" << fromId << '\n';
		SendNotice(toId, Protocol::PARTY_EXPIRED);
		SendNotice(fromId, Protocol::PARTY_EXPIRED);
	}
}

void PartyManager::SendNotice(uint64 characterId, Protocol::PartyResult result, const string& subject)
{
	GameSessionRef session = GSessionManager.FindByCharacter(characterId);
	if (session == nullptr)
		return;

	Snapshot snap;
	{
		WRITE_LOCK;
		const PartyEntry* party = _book.PartyOf(characterId);
		snap = TakeSnapshot(party != nullptr ? party->partyId : 0);
	}
	SendTo(session, snap, result, subject);
}

PartyManager::CoopEnter PartyManager::BeginCoopEnter(const GameSessionRef& requester, uint32 floorId)
{
	CoopEnter out;
	const uint64 characterId = CharacterOf(requester);
	if (characterId == 0)
	{
		out.result = Protocol::PARTY_NOT_FOUND;
		return out;
	}

	vector<uint64> others;
	{
		WRITE_LOCK;
		out.partyId = _book.EnsureParty(characterId, NameOf(requester));
		const PartyEntry* party = _book.Find(out.partyId);

		// 살아 있는 파티 인스턴스 - 지금 파티원이 그 안에 있으면 합류 (한 번 들어간 사람인지는 방이 본다).
		// 아무도 없으면(모두 돌아왔는데 내보낸 사람만 남아 닫히지 않았다 등) 파티장은 새 판을 연다 - 옛 판은 비면 닫힌다
		if (party->instanceId != 0)
		{
			bool memberInside = false;
			for (const PartyMemberEntry& m : party->members)
			{
				if (m.characterId == characterId)
					continue;
				if (m.online == false)
				{
					memberInside = true;		// 런 중에 끊긴 몸이 그 판에 남아 있다
					continue;
				}
				GameSessionRef session = GSessionManager.FindByCharacter(m.characterId);
				const SessionPlace place = session != nullptr ? session->place.load() : SessionPlace::None;
				memberInside |= (place == SessionPlace::Dungeon || place == SessionPlace::Entering || place == SessionPlace::Escaping);
			}

			if (memberInside || party->leaderId != characterId)
			{
				if (RoomRef room = GInstances.ReserveJoin(party->instanceId))
				{
					out.room = room;
					return out;
				}
			}
			_book.BindInstance(out.partyId, 0);		// 닫히는 중이었거나 파티장이 새로 연다
			party = _book.Find(out.partyId);
		}

		if (party->leaderId != characterId)
		{
			out.result = Protocol::PARTY_NO_INSTANCE;
			return out;
		}

		// 파티장이 연다 - 팀 = 파티 id (그 런 동안 바뀌지 않는다, D6)
		out.room = GInstances.Create(RoomKind::PveCoop, floorId, out.partyId, out.partyId);
		_book.BindInstance(out.partyId, out.room->InstanceId());

		for (const PartyMemberEntry& m : party->members)
		{
			if (m.characterId != characterId && m.online)
				others.push_back(m.characterId);
		}
	}

	// 베이스캠프에 있는 멤버를 데려간다 - 자리를 비웠거나 바쁘면 건너뛴다 (나중에 [합류])
	for (uint64 memberId : others)
	{
		GameSessionRef member = GSessionManager.FindByCharacter(memberId);
		if (member == nullptr || member->place.load() != SessionPlace::BaseCamp || IsBusy(memberId))
			continue;
		if (GInstances.ReserveJoin(out.room->InstanceId()) == nullptr)
			break;
		out.pulls.push_back(member);
	}
	return out;
}

size_t PartyManager::SizeOf(uint64 characterId)
{
	WRITE_LOCK;
	return _book.SizeOf(characterId);
}

uint64 PartyManager::PartyOf(uint64 characterId)
{
	WRITE_LOCK;
	const PartyEntry* party = _book.PartyOf(characterId);
	return party != nullptr ? party->partyId : 0;
}

PartyManager::Snapshot PartyManager::TakeSnapshot(uint64 partyId)
{
	Snapshot snap;
	if (const PartyEntry* party = _book.Find(partyId))
		snap.party = *party;
	return snap;
}

void PartyManager::Broadcast(const Snapshot& snap, uint64 resultTo, Protocol::PartyResult result, const string& subject)
{
	for (const PartyMemberEntry& m : snap.party.members)
	{
		if (m.online == false)
			continue;
		if (GameSessionRef session = GSessionManager.FindByCharacter(m.characterId))
			SendTo(session, snap, (m.characterId == resultTo) ? result : Protocol::PARTY_OK, (m.characterId == resultTo) ? subject : string());
	}
}

void PartyManager::SendTo(const GameSessionRef& session, const Snapshot& snap, Protocol::PartyResult result, const string& subject)
{
	if (session == nullptr)
		return;

	Protocol::S_PARTY_STATE pkt;
	pkt.set_party_id(snap.party.partyId);
	pkt.set_in_run(snap.party.instanceId != 0);
	pkt.set_result(result);
	pkt.set_subject_name(subject);
	for (const PartyMemberEntry& m : snap.party.members)
	{
		Protocol::PartyMember* member = pkt.add_members();
		member->set_character_id(m.characterId);
		member->set_name(m.name);
		member->set_leader(m.characterId == snap.party.leaderId);
		member->set_state(StateOf(m, m.online ? GSessionManager.FindByCharacter(m.characterId) : nullptr));
	}

	SEND_PACKET_DECLARATION(pkt);
	session->Send(sendBuffer);
}
