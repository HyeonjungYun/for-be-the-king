#include "pch.h"
#include "PartyBook.h"
#include "InstanceRules.h"

// PVE 협동 파티 장부 · 인스턴스 규칙 (ADR-0014 · design/quick-specs/pve-modes-2026-10-11.md AC-PVE-*)

namespace
{
	constexpr uint64 SEC = 1'000'000;
	constexpr uint64 A = 101, B = 102, C = 103, D = 104;

	// A 가 B 를 초대하고 B 가 받는다 - A 가 파티장인 2인 파티
	uint64 PartyOfTwo(PartyBook& book, uint64 now = 0)
	{
		uint64 partyId = 0;
		uint64 inviter = 0;
		EXPECT_EQ(book.Invite(A, "a", B, now, OUT partyId), PartyBookResult::Ok);
		EXPECT_EQ(book.Respond(B, "b", partyId, true, now, OUT inviter), PartyBookResult::Ok);
		EXPECT_EQ(inviter, A);
		return partyId;
	}
}

TEST(PartyBook, 초대를_받으면_초대한_사람이_파티장인_파티가_된다)
{
	PartyBook book;
	const uint64 partyId = PartyOfTwo(book);

	const PartyEntry* party = book.Find(partyId);
	ASSERT_NE(party, nullptr);
	EXPECT_EQ(party->leaderId, A);
	ASSERT_EQ(party->members.size(), 2u);
	EXPECT_EQ(party->members[0].characterId, A);
	EXPECT_EQ(party->members[1].name, "b");
	EXPECT_TRUE(book.IsLeader(A));
	EXPECT_FALSE(book.IsLeader(B));
	EXPECT_EQ(book.SizeOf(B), 2u);
	EXPECT_EQ(book.InviteFor(B), nullptr);			// 받은 초대는 지워진다
}

TEST(PartyBook, 거절하거나_시간이_지나면_들어가지_않는다)
{
	PartyBook book;
	uint64 partyId = 0;
	uint64 inviter = 0;

	ASSERT_EQ(book.Invite(A, "a", B, 0, OUT partyId), PartyBookResult::Ok);
	EXPECT_EQ(book.Respond(B, "b", partyId, false, 1 * SEC, OUT inviter), PartyBookResult::Declined);
	EXPECT_EQ(inviter, A);
	EXPECT_EQ(book.SizeOf(B), 0u);
	EXPECT_EQ(book.SizeOf(A), 1u);					// 초대한 사람은 1인 파티로 남는다 (아무 모드나 들어갈 수 있다)

	ASSERT_EQ(book.Invite(A, "a", C, 10 * SEC, OUT partyId), PartyBookResult::Ok);
	EXPECT_EQ(book.Respond(C, "c", partyId, true, 10 * SEC + PartyBook::INVITE_LIFETIME_US, OUT inviter), PartyBookResult::Expired);
	EXPECT_EQ(book.SizeOf(C), 0u);

	// 만료 정리는 (받는 사람, 보낸 사람) 을 알려 준다
	ASSERT_EQ(book.Invite(A, "a", D, 100 * SEC, OUT partyId), PartyBookResult::Ok);
	vector<pair<uint64, uint64>> expired;
	book.ExpireInvites(100 * SEC + PartyBook::INVITE_LIFETIME_US - 1, OUT expired);
	EXPECT_TRUE(expired.empty());
	book.ExpireInvites(100 * SEC + PartyBook::INVITE_LIFETIME_US, OUT expired);
	ASSERT_EQ(expired.size(), 1u);
	EXPECT_EQ(expired[0], make_pair(D, A));
}

TEST(PartyBook, 자기_자신_가득_찬_파티_다른_파티원_파티장_아님은_거절한다)
{
	PartyBook book;
	uint64 partyId = 0;
	uint64 inviter = 0;

	EXPECT_EQ(book.Invite(A, "a", A, 0, OUT partyId), PartyBookResult::Self);

	partyId = PartyOfTwo(book);
	EXPECT_EQ(book.Invite(B, "b", C, 0, OUT partyId), PartyBookResult::NotLeader);	// 파티장만 초대한다
	EXPECT_EQ(book.Invite(A, "a", B, 0, OUT partyId), PartyBookResult::AlreadyInParty);

	ASSERT_EQ(book.Invite(A, "a", C, 0, OUT partyId), PartyBookResult::Ok);
	ASSERT_EQ(book.Respond(C, "c", partyId, true, 0, OUT inviter), PartyBookResult::Ok);
	EXPECT_EQ(book.SizeOf(A), PartyBook::MAX_MEMBERS);
	EXPECT_EQ(book.Invite(A, "a", D, 0, OUT partyId), PartyBookResult::Full);		// 최대 3명

	// 다른 2인 파티에 있는 사람은 초대할 수 없다
	PartyBook other;
	uint64 p = 0;
	ASSERT_EQ(other.Invite(C, "c", D, 0, OUT p), PartyBookResult::Ok);
	ASSERT_EQ(other.Respond(D, "d", p, true, 0, OUT inviter), PartyBookResult::Ok);
	EXPECT_EQ(other.Invite(A, "a", D, 0, OUT p), PartyBookResult::AlreadyInParty);
}

TEST(PartyBook, 살아_있는_다른_초대가_있으면_덮지_않는다)
{
	PartyBook book;
	uint64 p1 = 0;
	uint64 p2 = 0;
	ASSERT_EQ(book.Invite(A, "a", C, 0, OUT p1), PartyBookResult::Ok);
	EXPECT_EQ(book.Invite(B, "b", C, 1 * SEC, OUT p2), PartyBookResult::Busy);
	EXPECT_EQ(book.InviteFor(C)->partyId, p1);

	// 같은 파티의 다시 초대는 시간만 늘린다
	EXPECT_EQ(book.Invite(A, "a", C, 5 * SEC, OUT p1), PartyBookResult::Ok);
	EXPECT_EQ(book.InviteFor(C)->expiresUs, 5 * SEC + PartyBook::INVITE_LIFETIME_US);
}

TEST(PartyBook, 혼자인_파티는_다른_초대를_받으면_풀린다)
{
	PartyBook book;
	const uint64 solo = book.EnsureParty(B, "b");		// B 가 협동에 혼자 들어갔던 1인 파티
	EXPECT_EQ(book.SizeOf(B), 1u);
	EXPECT_EQ(book.EnsureParty(B, "b"), solo);		// 이미 있으면 그 파티

	uint64 partyId = 0;
	uint64 inviter = 0;
	ASSERT_EQ(book.Invite(A, "a", B, 0, OUT partyId), PartyBookResult::Ok);
	ASSERT_EQ(book.Respond(B, "b", partyId, true, 0, OUT inviter), PartyBookResult::Ok);
	EXPECT_EQ(book.Find(solo), nullptr);			// 빈 파티는 사라진다
	EXPECT_EQ(book.SizeOf(B), 2u);
}

TEST(PartyBook, 파티장이_나가면_먼저_들어온_사람이_이어받고_마지막이_나가면_사라진다)
{
	PartyBook book;
	const uint64 partyId = PartyOfTwo(book);
	uint64 inviter = 0;
	uint64 p = 0;
	ASSERT_EQ(book.Invite(A, "a", C, 0, OUT p), PartyBookResult::Ok);
	ASSERT_EQ(book.Respond(C, "c", partyId, true, 0, OUT inviter), PartyBookResult::Ok);

	uint64 left = 0;
	EXPECT_EQ(book.Leave(B, C, OUT left), PartyBookResult::NotLeader);	// 파티장이 아니면 내보낼 수 없다
	EXPECT_EQ(book.Leave(A, A, OUT left), PartyBookResult::Ok);
	EXPECT_EQ(left, partyId);
	EXPECT_EQ(book.Find(partyId)->leaderId, B);

	EXPECT_EQ(book.Leave(B, C, OUT left), PartyBookResult::Ok);			// 새 파티장이 내보낸다
	EXPECT_EQ(book.SizeOf(C), 0u);
	EXPECT_EQ(book.Leave(B, B, OUT left), PartyBookResult::Ok);
	EXPECT_EQ(book.Find(partyId), nullptr);
	EXPECT_EQ(book.PartyCount(), 0u);
	EXPECT_EQ(book.Leave(B, B, OUT left), PartyBookResult::NotFound);
}

TEST(PartyBook, 런_중에_끊기면_남고_인스턴스가_닫히면_빠진다)
{
	PartyBook book;
	const uint64 partyId = PartyOfTwo(book);
	book.BindInstance(partyId, 17);
	book.SetInRun(A, true);
	book.SetInRun(B, true);

	// B 가 던전에서 끊겼다 - 몸이 남아 "접속 끊김"
	EXPECT_EQ(book.OnDisconnected(B), partyId);
	ASSERT_NE(book.Find(partyId)->Member(B), nullptr);
	EXPECT_FALSE(book.Find(partyId)->Member(B)->online);

	// 다른 인스턴스의 닫힘은 상관없다
	EXPECT_TRUE(book.OnInstanceClosed(partyId, 99));
	EXPECT_EQ(book.Find(partyId)->instanceId, 17u);

	EXPECT_TRUE(book.OnInstanceClosed(partyId, 17));
	EXPECT_EQ(book.Find(partyId)->instanceId, 0u);
	EXPECT_EQ(book.SizeOf(A), 1u);					// 끊긴 B 는 빠지고 A 만 남았다
	EXPECT_FALSE(book.Find(partyId)->Member(A)->inRun);

	// 베이스캠프에서 끊기면 바로 빠진다 - 혼자 남은 파티는 사라진다
	EXPECT_EQ(book.OnDisconnected(A), partyId);
	EXPECT_EQ(book.Find(partyId), nullptr);
}

TEST(PartyBook, 끊겼다_돌아오면_접속_끊김이_풀린다)
{
	PartyBook book;
	const uint64 partyId = PartyOfTwo(book);
	book.SetInRun(B, true);
	book.OnDisconnected(B);
	EXPECT_EQ(book.OnLogin(B), partyId);
	EXPECT_TRUE(book.Find(partyId)->Member(B)->online);
	EXPECT_EQ(book.OnLogin(D), 0u);					// 파티가 없는 사람
}

TEST(InstanceRules, 같은_팀은_0이_아닌_같은_값이다)
{
	EXPECT_FALSE(InstanceRules::Allied(0, 0));		// 팀 없음끼리는 적 (PvP)
	EXPECT_TRUE(InstanceRules::Allied(5, 5));
	EXPECT_FALSE(InstanceRules::Allied(5, 6));
	EXPECT_FALSE(InstanceRules::Allied(5, 0));
}

TEST(InstanceRules, 사람과_사망_커밋이_없고_유예가_지나야_닫는다)
{
	using InstanceRules::CloseVerdict;
	using InstanceRules::JudgeClose;
	constexpr uint64 G = InstanceRules::EMPTY_GRACE_US;

	EXPECT_EQ(JudgeClose(1, 0, 0, 10 * SEC), CloseVerdict::Keep);
	EXPECT_EQ(JudgeClose(0, 1, 5 * SEC, 100 * SEC), CloseVerdict::Keep);		// 가방이 아직 안 생겼다
	EXPECT_EQ(JudgeClose(0, 0, 0, 10 * SEC), CloseVerdict::StartGrace);
	EXPECT_EQ(JudgeClose(0, 0, 10 * SEC, 10 * SEC + G - 1), CloseVerdict::Keep);
	EXPECT_EQ(JudgeClose(0, 0, 10 * SEC, 10 * SEC + G), CloseVerdict::Close);
}

TEST(InstanceRules, 인스턴스_가방_층은_1000_이상이다)
{
	EXPECT_FALSE(InstanceRules::IsInstanceBagFloor(0));
	EXPECT_FALSE(InstanceRules::IsInstanceBagFloor(3));
	EXPECT_TRUE(InstanceRules::IsInstanceBagFloor(InstanceRules::INSTANCE_BAG_FLOOR_BASE + 0));
}

// 던전 파티 (party-betrayal-2026-10-12 AC-PB-03 · 05): 방 키를 기억하고, 한 명이 나가도 남은 파티에 남는다. 새 결과 값은 와이어와 같다 (PartyManager static_assert)
TEST(PartyBook, 던전_파티는_방_키를_기억하고_나가도_남은_파티에_남는다_AC_PB_03)
{
	PartyBook book;
	uint64 pid = 0;
	EXPECT_EQ(book.Invite(1, "a", 2, 0, OUT pid), PartyBookResult::Ok);
	uint64 inviter = 0;
	EXPECT_EQ(book.Respond(2, "b", pid, true, 0, OUT inviter), PartyBookResult::Ok);
	book.BindDungeonRoom(pid, 777);
	ASSERT_NE(book.Find(pid), nullptr);
	EXPECT_EQ(book.Find(pid)->dungeonRoom, 777u);
	uint64 left = 0;
	EXPECT_EQ(book.Leave(2, 2, OUT left), PartyBookResult::Ok);
	EXPECT_EQ(left, pid);
	ASSERT_NE(book.Find(pid), nullptr);
	EXPECT_EQ(book.Find(pid)->dungeonRoom, 777u);
	EXPECT_EQ(book.SizeOf(1), 1u);
	EXPECT_EQ(book.SizeOf(2), 0u);
	EXPECT_EQ(static_cast<int32>(PartyBookResult::NotVisible), 12);
	EXPECT_EQ(static_cast<int32>(PartyBookResult::NoKickInRun), 13);
}

// 혼자 남은 던전 파티 (R5 · 검증 r1 수정 1): 2 명이면 나가기 = 배신 대상, 1 명이면 아니다. 베이스캠프 파티는 인원과 무관하게 아니다
TEST(PartyBook, 혼자_남은_던전_파티만_IsSoloDungeonParty_가_참이다_R5)
{
	PartyBook book;
	uint64 pid = 0;
	EXPECT_EQ(book.Invite(1, "a", 2, 0, OUT pid), PartyBookResult::Ok);
	uint64 inviter = 0;
	EXPECT_EQ(book.Respond(2, "b", pid, true, 0, OUT inviter), PartyBookResult::Ok);
	EXPECT_FALSE(book.IsSoloDungeonParty(1));		// 베이스캠프 파티 2 명
	book.BindDungeonRoom(pid, 777);
	EXPECT_FALSE(book.IsSoloDungeonParty(1));		// 던전 파티 2 명 - 나가면 배신
	EXPECT_FALSE(book.IsSoloDungeonParty(2));
	uint64 left = 0;
	EXPECT_EQ(book.Leave(2, 2, OUT left), PartyBookResult::Ok);
	EXPECT_TRUE(book.IsSoloDungeonParty(1));		// 배신당해 혼자 남았다 - 나가도 배신이 아니다
	EXPECT_FALSE(book.IsSoloDungeonParty(2));		// 파티 없음
	EXPECT_FALSE(book.IsSoloDungeonParty(3));		// 처음부터 없음

	// 초대만 보내고 수락 전인 파티장 (파티 1 명)
	uint64 pid2 = 0;
	EXPECT_EQ(book.Invite(5, "e", 6, 0, OUT pid2), PartyBookResult::Ok);
	EXPECT_FALSE(book.IsSoloDungeonParty(5));		// 베이스캠프 파티는 아니다
	book.BindDungeonRoom(pid2, 777);
	EXPECT_TRUE(book.IsSoloDungeonParty(5));
}
