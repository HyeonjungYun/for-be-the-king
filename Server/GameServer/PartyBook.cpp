#include "pch.h"
#include "PartyBook.h"

const PartyMemberEntry* PartyEntry::Member(uint64 characterId) const
{
	for (const PartyMemberEntry& m : members)
	{
		if (m.characterId == characterId)
			return &m;
	}
	return nullptr;
}

PartyMemberEntry* PartyEntry::Member(uint64 characterId)
{
	return const_cast<PartyMemberEntry*>(static_cast<const PartyEntry*>(this)->Member(characterId));
}

PartyBookResult PartyBook::Invite(uint64 from, const string& fromName, uint64 to, uint64 nowUs, OUT uint64& outPartyId)
{
	outPartyId = 0;
	if (from == to)
		return PartyBookResult::Self;

	PartyEntry* party = MutablePartyOf(from);
	if (party != nullptr && party->leaderId != from)
		return PartyBookResult::NotLeader;
	if (party != nullptr && party->members.size() >= MAX_MEMBERS)
		return PartyBookResult::Full;

	// 받는 사람이 이미 2명 이상 파티에 있다 - 혼자인 파티(협동 혼자 입장)는 수락할 때 풀린다
	if (const PartyEntry* theirs = PartyOf(to); theirs != nullptr && (theirs == party || theirs->members.size() >= 2))
		return PartyBookResult::AlreadyInParty;

	// 한 사람에게 초대는 하나 - 다른 파티의 살아 있는 초대가 있으면 덮지 않는다
	if (auto it = _invites.find(to); it != _invites.end() && it->second.expiresUs > nowUs
		&& (party == nullptr || it->second.partyId != party->partyId))
		return PartyBookResult::Busy;

	if (party == nullptr)
	{
		const uint64 partyId = _nextPartyId++;
		PartyEntry& created = _parties[partyId];
		created.partyId = partyId;
		created.leaderId = from;
		created.members.push_back({ from, fromName, true, false });
		_partyOf[from] = partyId;
		party = &created;
	}

	_invites[to] = PartyInvite{ party->partyId, from, fromName, nowUs + INVITE_LIFETIME_US };
	outPartyId = party->partyId;
	return PartyBookResult::Ok;
}

PartyBookResult PartyBook::Respond(uint64 to, const string& toName, uint64 partyId, bool accept, uint64 nowUs, OUT uint64& outInviterId)
{
	outInviterId = 0;

	auto inviteIt = _invites.find(to);
	if (inviteIt == _invites.end() || inviteIt->second.partyId != partyId)
		return PartyBookResult::Expired;

	const PartyInvite invite = inviteIt->second;
	_invites.erase(inviteIt);
	outInviterId = invite.fromId;

	if (invite.expiresUs <= nowUs)
		return PartyBookResult::Expired;

	auto partyIt = _parties.find(partyId);
	if (partyIt == _parties.end())
		return PartyBookResult::Expired;

	if (accept == false)
		return PartyBookResult::Declined;

	if (partyIt->second.members.size() >= MAX_MEMBERS)
		return PartyBookResult::Full;

	if (PartyEntry* theirs = MutablePartyOf(to))
	{
		if (theirs->members.size() >= 2)
			return PartyBookResult::AlreadyInParty;

		const PartyMemberEntry* self = theirs->Member(to);
		if (self != nullptr && self->inRun)
			return PartyBookResult::Busy;

		RemoveMember(*theirs, to);		// 혼자인 파티를 풀고 들어간다 (partyIt 는 다른 파티라 그대로 유효)
	}

	PartyEntry& party = _parties[partyId];
	party.members.push_back({ to, toName, true, false });
	_partyOf[to] = partyId;
	return PartyBookResult::Ok;
}

PartyBookResult PartyBook::Leave(uint64 by, uint64 who, OUT uint64& outPartyId)
{
	outPartyId = 0;

	PartyEntry* party = MutablePartyOf(who);
	if (party == nullptr)
		return PartyBookResult::NotFound;

	if (by != who && party->leaderId != by)
		return PartyBookResult::NotLeader;

	outPartyId = party->partyId;
	RemoveMember(*party, who);
	return PartyBookResult::Ok;
}

uint64 PartyBook::EnsureParty(uint64 who, const string& name)
{
	if (const PartyEntry* party = PartyOf(who))
		return party->partyId;

	const uint64 partyId = _nextPartyId++;
	PartyEntry& created = _parties[partyId];
	created.partyId = partyId;
	created.leaderId = who;
	created.members.push_back({ who, name, true, false });
	_partyOf[who] = partyId;
	return partyId;
}

void PartyBook::ExpireInvites(uint64 nowUs, OUT vector<pair<uint64, uint64>>& outExpired)
{
	outExpired.clear();
	for (auto it = _invites.begin(); it != _invites.end();)
	{
		if (it->second.expiresUs <= nowUs)
		{
			outExpired.push_back({ it->first, it->second.fromId });
			it = _invites.erase(it);
		}
		else
		{
			++it;
		}
	}
}

uint64 PartyBook::OnDisconnected(uint64 who)
{
	_invites.erase(who);

	PartyEntry* party = MutablePartyOf(who);
	if (party == nullptr)
		return 0;

	const uint64 partyId = party->partyId;
	PartyMemberEntry* member = party->Member(who);
	if (member != nullptr && member->inRun)
	{
		member->online = false;		// 몸이 던전에 남아 있다 (끊김 유예) - 인스턴스가 닫힐 때 뺀다
		return partyId;
	}

	RemoveMember(*party, who);
	return partyId;
}

uint64 PartyBook::OnLogin(uint64 who)
{
	PartyEntry* party = MutablePartyOf(who);
	if (party == nullptr)
		return 0;

	if (PartyMemberEntry* member = party->Member(who))
		member->online = true;
	return party->partyId;
}

void PartyBook::SetInRun(uint64 who, bool inRun)
{
	if (PartyEntry* party = MutablePartyOf(who))
	{
		if (PartyMemberEntry* member = party->Member(who))
			member->inRun = inRun;
	}
}

void PartyBook::BindInstance(uint64 partyId, uint64 instanceId)
{
	if (auto it = _parties.find(partyId); it != _parties.end())
		it->second.instanceId = instanceId;
}

bool PartyBook::OnInstanceClosed(uint64 partyId, uint64 instanceId)
{
	auto it = _parties.find(partyId);
	if (it == _parties.end())
		return false;

	PartyEntry& party = it->second;
	if (party.instanceId != instanceId)
		return true;		// 이미 다른 인스턴스 - 옛것의 닫힘은 상관없다

	party.instanceId = 0;
	vector<uint64> offline;
	for (PartyMemberEntry& m : party.members)
	{
		m.inRun = false;
		if (m.online == false)
			offline.push_back(m.characterId);
	}

	for (uint64 who : offline)
	{
		auto still = _parties.find(partyId);
		if (still == _parties.end())
			break;
		RemoveMember(still->second, who);
	}
	return _parties.find(partyId) != _parties.end();
}

const PartyEntry* PartyBook::PartyOf(uint64 who) const
{
	auto it = _partyOf.find(who);
	if (it == _partyOf.end())
		return nullptr;
	return Find(it->second);
}

const PartyEntry* PartyBook::Find(uint64 partyId) const
{
	auto it = _parties.find(partyId);
	return it != _parties.end() ? &it->second : nullptr;
}

const PartyInvite* PartyBook::InviteFor(uint64 to) const
{
	auto it = _invites.find(to);
	return it != _invites.end() ? &it->second : nullptr;
}

size_t PartyBook::SizeOf(uint64 who) const
{
	const PartyEntry* party = PartyOf(who);
	return party != nullptr ? party->members.size() : 0;
}

bool PartyBook::IsLeader(uint64 who) const
{
	const PartyEntry* party = PartyOf(who);
	return party != nullptr && party->leaderId == who;
}

PartyEntry* PartyBook::MutablePartyOf(uint64 who)
{
	return const_cast<PartyEntry*>(PartyOf(who));
}

void PartyBook::RemoveMember(PartyEntry& party, uint64 who)
{
	const uint64 partyId = party.partyId;
	party.members.erase(remove_if(party.members.begin(), party.members.end(),
		[who](const PartyMemberEntry& m) { return m.characterId == who; }), party.members.end());
	_partyOf.erase(who);

	if (party.members.empty())
	{
		// 이 파티가 낸 초대도 함께 사라진다
		for (auto it = _invites.begin(); it != _invites.end();)
			it = (it->second.partyId == partyId) ? _invites.erase(it) : next(it);

		_parties.erase(partyId);		// party 참조는 여기서 끝난다
		return;
	}

	if (party.leaderId == who)
		party.leaderId = party.members.front().characterId;		// 가장 먼저 들어온 사람이 이어받는다
}
