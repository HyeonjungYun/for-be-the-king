#pragma once
#include "Bag.h"

/*-----------------
	CommitJournal
------------------*/

// 기다리는 런 끝 저장의 디스크 기록 (ADR-0010 § ⑥ Migration, 사용자 결정 2026-10-08).
//
// 저장의 첫 시도가 실패해 재시도로 들어가는 순간 스냅샷을 파일 하나로 남기고, 저장이 끝나면 지운다(포기면 abandoned/ 로).
// 서버가 그 사이에 꺼지면 다음 부팅이 접속을 받기 전에 같은 규칙(엄격 → 관대 → 포기)으로 처리한다 - ObjectUtils ReplayCommitJournal.
// 기다리는 동안 그 캐릭터는 잠겨 있었으므로(GDeathCommits - 재입장 · 베이스캠프 조작 불가) 스냅샷이 DB 와 어긋나지 않는다.
//
// 같은 기록을 두 번 적용하지 않는다: 기록된 저장은 커밋하는 트랜잭션 안에서 applied_commits 에 기록 id 를 남긴다.
// 커밋은 됐는데 파일을 지우기 전에 꺼졌으면, 부팅이 그 표시를 보고 건너뛴다 (탈출 골드 두 번 지급 같은 일을 막는다).
//
// 여기는 순수 부분(형식 · 읽기 · 파일)만 - DB 를 모른다. GameServerTests CommitJournalTest.

enum class JournalKind : uint8
{
	Death,
	Escape,
	Expiry,
};

struct JournalEntry
{
	string			id;							// 파일 이름(확장자 앞) = applied_commits.journal_id
	JournalKind		kind = JournalKind::Death;
	uint64			characterId = 0;

	DeathSnapshot	death;						// kind 에 맞는 것 하나만 쓴다
	EscapeSnapshot	escape;
	ExpirySnapshot	expiry;
};

namespace CommitJournal
{
	constexpr int32		VERSION = 1;
	constexpr const char* EXTENSION = ".journal";
	constexpr const char* ABANDONED_DIR = "abandoned";

	const char*		KindName(JournalKind kind);		// DEATH · ESCAPE · EXPIRE (CommitTask 태그와 같다)

	// DEATH-128-1791459044123-7 - 종류 · 캐릭터 · 유닉스 ms · 프로세스 안 순번. 영문 · 숫자 · '-' 만 (SQL · 파일 이름에 그대로)
	string			NewId(JournalKind kind, uint64 characterId);

	// 한 줄에 key=value. 마지막 줄 end=1 이 없으면 쓰다 만 파일이다
	string			Format(const JournalEntry& entry);
	bool			Parse(const string& text, OUT JournalEntry& out, OUT string& error);

	// 디렉터리가 없으면 만든다. .tmp 에 다 쓴 뒤 이름을 바꾼다 - 반쯤 쓴 기록이 *.journal 로 보이지 않게
	bool			Write(const string& dir, const JournalEntry& entry, OUT string& error);
	bool			Read(const string& dir, const string& id, OUT JournalEntry& out, OUT string& error);
	bool			Remove(const string& dir, const string& id);
	bool			MoveToAbandoned(const string& dir, const string& id);

	// *.journal 의 id, 이름순. 처리 순서는 상관없다 - 캐릭터마다 기다리는 저장은 하나뿐이다 (GDeathCommits)
	vector<string>	ListIds(const string& dir);

	// 기록 속 가장 큰 아이템 · 가방 id - 런 중 생성 아이템 · 가방 id 는 DB 에 없어 RestoreIdCounters 가 모른다. 재처리 전에 카운터를 그 위로
	void			MaxIds(const JournalEntry& entry, OUT uint64& maxInstanceId, OUT uint64& maxBagId);
}
