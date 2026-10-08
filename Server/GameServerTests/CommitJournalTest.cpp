#include "pch.h"
#include "CommitJournal.h"
#include "ItemTable.h"
#include <filesystem>
#include <fstream>

// 기다리는 런 끝 저장의 디스크 기록 (ADR-0010 § ⑥, 사용자 결정 2026-10-08) - 형식 왕복 · 쓰다 끊긴 파일 · 파일 다루기.
// DB 쪽(부팅 재처리 · applied_commits)은 실서버에서 확인한다 (evidence commit-journal-2026-10-08.md)

namespace
{
	ItemEntry Item(uint64 id, uint32 type, Protocol::ItemGrade grade, uint32 skill, bool isNew)
	{
		ItemEntry e;
		e.instanceId = id;
		e.itemTypeId = type;
		e.grade = grade;
		e.level = 0;
		e.skillIdPrimary = skill;
		e.skillIdSecondary = 0;
		e.isNew = isNew;
		return e;
	}

	bool SameItem(const ItemEntry& a, const ItemEntry& b)
	{
		return a.instanceId == b.instanceId && a.itemTypeId == b.itemTypeId && a.grade == b.grade && a.level == b.level
			&& a.skillIdPrimary == b.skillIdPrimary && a.skillIdSecondary == b.skillIdSecondary && a.isNew == b.isNew;
	}

	JournalEntry DeathEntry()
	{
		JournalEntry e;
		e.id = "DEATH-128-1791459044123-0";
		e.kind = JournalKind::Death;
		e.characterId = 128;
		e.death.characterId = 128;
		e.death.bagId = 2000000042;
		e.death.floorId = 0;
		e.death.x = -778.347f;
		e.death.y = 1286.71f;
		e.death.z = 100.f;
		e.death.items = { Item(801281, 101, Protocol::ITEM_GRADE_COMMON, 1001, false), Item(900123, 401, Protocol::ITEM_GRADE_LEGENDARY, 3001, true) };
		e.death.droppedIds = { 801299 };
		return e;
	}

	string TestDir(const char* name)
	{
		const string dir = ItemTable::DefaultDataDir() + "\\..\\Logs\\" + name;
		std::error_code ec;
		std::filesystem::remove_all(dir, ec);
		return dir;
	}
}

TEST(CommitJournal, 사망_기록은_쓰고_읽으면_그대로다)
{
	const JournalEntry in = DeathEntry();

	JournalEntry out;
	string error;
	ASSERT_TRUE(CommitJournal::Parse(CommitJournal::Format(in), OUT out, OUT error)) << error;

	EXPECT_EQ(out.id, in.id);
	EXPECT_EQ(out.kind, JournalKind::Death);
	EXPECT_EQ(out.characterId, 128u);
	EXPECT_EQ(out.death.characterId, 128u) << "스냅샷 안의 캐릭터 id 도 채운다 - TryCommitDeath 가 그것을 쓴다";
	EXPECT_EQ(out.death.bagId, in.death.bagId);
	EXPECT_FLOAT_EQ(out.death.x, in.death.x) << "좌표는 비트 그대로 왕복해야 가방이 같은 자리에 생긴다";
	EXPECT_FLOAT_EQ(out.death.y, in.death.y);
	ASSERT_EQ(out.death.items.size(), 2u);
	EXPECT_TRUE(SameItem(out.death.items[0], in.death.items[0]));
	EXPECT_TRUE(SameItem(out.death.items[1], in.death.items[1])) << "런 중 생성(isNew) 표시가 살아야 재처리가 INSERT 한다";
	EXPECT_EQ(out.death.droppedIds, in.death.droppedIds);
}

TEST(CommitJournal, 탈출_기록은_슬롯_자리까지_그대로다)
{
	JournalEntry in;
	in.id = "ESCAPE-160-1791459044123-1";
	in.kind = JournalKind::Escape;
	in.characterId = 160;
	in.escape.gold = 500;
	in.escape.carried = { Item(801602, 401, Protocol::ITEM_GRADE_RARE, 3001, false) };
	in.escape.equipped[0] = Item(801601, 101, Protocol::ITEM_GRADE_COMMON, 1001, false);
	in.escape.equipped[4] = Item(900200, 501, Protocol::ITEM_GRADE_EPIC, 2001, true);
	in.escape.droppedIds = { 1, 2 };

	JournalEntry out;
	string error;
	ASSERT_TRUE(CommitJournal::Parse(CommitJournal::Format(in), OUT out, OUT error)) << error;

	EXPECT_EQ(out.escape.gold, 500u) << "골드가 틀리면 재처리가 다른 금액을 지급한다";
	ASSERT_EQ(out.escape.carried.size(), 1u);
	EXPECT_TRUE(SameItem(out.escape.carried[0], in.escape.carried[0]));
	EXPECT_TRUE(SameItem(out.escape.equipped[0], in.escape.equipped[0]));
	EXPECT_TRUE(SameItem(out.escape.equipped[4], in.escape.equipped[4]));
	EXPECT_EQ(out.escape.equipped[2].instanceId, 0u);
	EXPECT_EQ(out.escape.droppedIds, in.escape.droppedIds);
}

TEST(CommitJournal, 만료_기록은_그대로다)
{
	JournalEntry in;
	in.id = "EXPIRE-171-1791459044123-2";
	in.kind = JournalKind::Expiry;
	in.characterId = 171;
	in.expiry.goldKept = 300;
	in.expiry.itemsDestroyed = 4;
	in.expiry.rowIds = { 801711, 801712, 801713 };

	JournalEntry out;
	string error;
	ASSERT_TRUE(CommitJournal::Parse(CommitJournal::Format(in), OUT out, OUT error)) << error;
	EXPECT_EQ(out.expiry.goldKept, 300u);
	EXPECT_EQ(out.expiry.itemsDestroyed, 4u);
	EXPECT_EQ(out.expiry.rowIds, in.expiry.rowIds);
	EXPECT_EQ(out.expiry.characterId, 171u);
}

TEST(CommitJournal, 쓰다_끊기거나_깨진_기록은_읽지_않는다)
{
	const string good = CommitJournal::Format(DeathEntry());

	struct Case { const char* name; string text; };
	const Case cases[] =
	{
		{ "end 줄 없음 (쓰다 끊김)",	good.substr(0, good.rfind("end=1")) },
		{ "아이템 줄 중간에서 끊김",	good.substr(0, good.find("item=") + 12) },
		{ "end 뒤에 내용",			good + "item=1,101,1,0,0,0,0\n" },
		{ "모르는 키",				"v=1\nid=X\nkind=DEATH\nchar=1\nbag=2\nmystery=1\nend=1\n" },
		{ "모르는 종류",				"v=1\nid=X\nkind=TRADE\nchar=1\nend=1\n" },
		{ "다른 버전",				"v=2\nid=X\nkind=DEATH\nchar=1\nbag=2\nend=1\n" },
		{ "사망인데 가방 없음",		"v=1\nid=X\nkind=DEATH\nchar=1\nend=1\n" },
		{ "만료에 dropped",			"v=1\nid=X\nkind=EXPIRE\nchar=1\ndropped=5\nend=1\n" },
		{ "등급 범위 밖",			"v=1\nid=X\nkind=DEATH\nchar=1\nbag=2\nitem=1,101,9,0,0,0,0\nend=1\n" },
		{ "같은 슬롯 두 번",			"v=1\nid=X\nkind=ESCAPE\nchar=1\nequipped=0,1,101,1,0,0,0,0\nequipped=0,2,101,1,0,0,0,0\nend=1\n" },
		{ "캐릭터 0",				"v=1\nid=X\nkind=DEATH\nchar=0\nbag=2\nend=1\n" },
	};

	for (const Case& c : cases)
	{
		JournalEntry out;
		string error;
		EXPECT_FALSE(CommitJournal::Parse(c.text, OUT out, OUT error)) << c.name;
		EXPECT_FALSE(error.empty()) << c.name;
	}

	// CRLF 로 저장된 파일도 읽는다 (메모장으로 열어 저장한 경우)
	string crlf;
	for (char ch : good)
	{
		if (ch == '\n')
			crlf += '\r';
		crlf += ch;
	}
	JournalEntry out;
	string error;
	EXPECT_TRUE(CommitJournal::Parse(crlf, OUT out, OUT error)) << error;
}

TEST(CommitJournal, id_는_종류_캐릭터를_담고_매번_다르다)
{
	const string a = CommitJournal::NewId(JournalKind::Escape, 128);
	const string b = CommitJournal::NewId(JournalKind::Escape, 128);

	EXPECT_NE(a, b);
	EXPECT_EQ(a.rfind("ESCAPE-128-", 0), 0u) << a;
	for (char ch : a)
		EXPECT_TRUE(isalnum(static_cast<unsigned char>(ch)) || ch == '-') << "SQL · 파일 이름에 그대로 쓴다: " << a;
	EXPECT_LE(a.size(), 64u) << "applied_commits.journal_id VARCHAR(64)";
}

TEST(CommitJournal, 파일은_쓰기_목록_읽기_지우기_치우기가_된다)
{
	const string dir = TestDir("journal-test");
	JournalEntry entry = DeathEntry();

	string error;
	ASSERT_TRUE(CommitJournal::Write(dir, entry, OUT error)) << error;

	// 다른 프로세스가 쓰다 만 .tmp 는 목록에 없다
	{
		std::ofstream tmp(dir + "\\half-written.journal.tmp");
		tmp << "v=1\n";
	}

	const vector<string> ids = CommitJournal::ListIds(dir);
	ASSERT_EQ(ids.size(), 1u);
	EXPECT_EQ(ids[0], entry.id);

	JournalEntry read;
	ASSERT_TRUE(CommitJournal::Read(dir, entry.id, OUT read, OUT error)) << error;
	EXPECT_EQ(read.death.bagId, entry.death.bagId);

	ASSERT_TRUE(CommitJournal::MoveToAbandoned(dir, entry.id));
	EXPECT_TRUE(CommitJournal::ListIds(dir).empty()) << "치운 기록을 부팅이 또 붙잡지 않는다";
	EXPECT_TRUE(std::filesystem::exists(dir + "\\abandoned\\" + entry.id + ".journal")) << "원인을 볼 수 있게 남는다";

	ASSERT_TRUE(CommitJournal::Write(dir, entry, OUT error)) << error;
	EXPECT_TRUE(CommitJournal::Remove(dir, entry.id));
	EXPECT_TRUE(CommitJournal::ListIds(dir).empty());
	EXPECT_FALSE(CommitJournal::Remove(dir, entry.id)) << "이미 없으면 false";

	EXPECT_TRUE(CommitJournal::ListIds(dir + "\\no-such-dir").empty()) << "디렉터리가 없으면 기록도 없다";
}

TEST(CommitJournal, 기록_속_가장_큰_id_를_센다)
{
	uint64 maxInstance = 900000;
	uint64 maxBag = 2000000000;

	CommitJournal::MaxIds(DeathEntry(), OUT maxInstance, OUT maxBag);
	EXPECT_EQ(maxInstance, 900123u) << "런 중 생성 아이템 id - DB 에 없어 RestoreIdCounters 가 모른다";
	EXPECT_EQ(maxBag, 2000000042u);

	JournalEntry escape;
	escape.kind = JournalKind::Escape;
	escape.escape.equipped[3] = Item(950000, 101, Protocol::ITEM_GRADE_COMMON, 0, true);
	CommitJournal::MaxIds(escape, OUT maxInstance, OUT maxBag);
	EXPECT_EQ(maxInstance, 950000u);
	EXPECT_EQ(maxBag, 2000000042u) << "탈출에는 가방이 없다";
}
