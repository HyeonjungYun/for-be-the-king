#pragma once
#include "CommitRetry.h"
#include "CommitJournal.h"

struct DeathSnapshot;
struct EscapeSnapshot;
struct ExpirySnapshot;
struct ChestSpawn;

class ObjectUtils
{
public:
	static PlayerRef CreatePlayer(GameSessionRef session, uint64 characterId);
	static BagRef CreateBag(uint64 bagId, uint32 floorId, float x, float y, float z, uint64 expiresAtUs, uint64 ownerCharacterId);

	// 상자 = 만료 없고 주인 없는 가방. 처음 열 때 롤한다 (Room::HandleBagOpen)
	static BagRef CreateChest(const ChestSpawn& spawn);

private:
	static atomic<int64> s_idGenerator;
};

constexpr uint64 RUNTIME_ID_BASE = 1'000'000'000;
constexpr uint64 BAG_ID_BASE = 2'000'000'000;
constexpr uint64 CHEST_ID_BASE = 3'000'000'000;	// + chest_id 로 고정 - 로그에서 몇 번 상자인지 바로 보인다. 재시작해도 같다

extern atomic<uint64> GNextInstanceId;
extern atomic<uint64> GNextBagId;

bool RestoreIdCounters(DBConnection* conn);

bool PurgeExpiredBags(DBConnection* conn);

bool ValidateItemRows(DBConnection* conn);

bool LoadLiveBags(DBConnection* conn);

bool DeleteExpiredBags(DBConnection* conn, const vector<uint64>& bagIds, const vector<uint64>& instanceIds);

// ── 런 끝 저장 (사망 · 탈출 · 만료) ─ ADR-0010 · CommitRetry.h ──
// Try* = 한 번. 실패를 Transient(다시 하면 된다) / Permanent(다시 해도 같다)로 돌려준다.
// mode = Lenient 면 "그 순간 DB 에 실제로 남아 있는 그 캐릭터의 것만" 옮기고 어긋난 것은 🔴 로그로 남긴다.
// 사망 관대 단계는 snap 을 고친다 - 새 가방 id(그 단계에서 한 번) · 가방에 실제로 들어간 아이템만 남긴 목록.
// journalId 를 트랜잭션 맨 앞에서 applied_commits 에 남긴다 - 이미 있으면 전에 커밋됐다(응답을 잃었거나 부팅 재처리) → 롤백하고 Ok.
// QueueCommit 은 늘 id 를 준다. 빈 문자열이면 표시하지 않는다
CommitOutcome TryCommitDeath(DBConnection* conn, DeathSnapshot& snap, CommitMode mode, const string& journalId);
CommitOutcome TryCommitEscape(DBConnection* conn, const EscapeSnapshot& snap, CommitMode mode, const string& journalId);
CommitOutcome TryCommitExpiry(DBConnection* conn, const ExpirySnapshot& snap, CommitMode mode, const string& journalId);

// 큐에 넣고 끝날 때까지 다시 한다: 엄격 → (영구 실패) 관대 → (영구 실패) 포기, 일시 실패는 같은 단계를 100 ms → ×2 → 5 s 간격으로.
// 기다리는 동안 DB 스레드를 잡지 않는다(GDBQueue.PushAfter) · 연결이 끊겼으면 다시 붙인다.
// 첫 시도가 실패하면 journal(스냅샷)을 GCommitJournalDir 에 파일로 남긴다 - 끝나면 지우고, 포기면 abandoned/ 로.
// 기록 id 는 큐에 넣을 때 정한다(파일이 없어도 applied_commits 표시에 쓴다) · 끝나면 파일 → 표시 순으로 지운다.
// finish(conn, committed, mode) 는 DB 스레드에서 한 번 - committed = false 면 포기(DB 는 저장 전 그대로)
void QueueCommit(JournalEntry journal,
	function<CommitOutcome(DBConnection*, CommitMode, const string& journalId)> attempt,
	function<void(DBConnection*, bool committed, CommitMode mode)> finish);

// 저장 기록 디렉터리 - 부팅이 정한다 (Server/Logs/pending-commits · FBTK_JOURNAL_DIR). 비어 있으면 기록하지 않는다
extern string GCommitJournalDir;

// 검증 전용 (FBTK_FAKE_ACK_LOSS=N) - 처음 N 번의 런 끝 커밋은 DB 에선 됐는데 응답을 잃은 것처럼 일시 실패로 돌려준다
extern atomic<int32> GFakeCommitAckLoss;

// 부팅 - applied_commits 표가 없으면 만든다 (Server/Tools/schema_commit_journal.sql 과 같은 정의)
bool EnsureCommitJournalTable(DBConnection* conn);

// 부팅 - 접속을 받기 전에 남은 기록을 처리한다 (RestoreIdCounters 뒤, PurgeExpiredBags · LoadLiveBags 앞).
// 이미 적용된 기록(applied_commits 에 있음)은 지우기만 · 읽을 수 없는 기록 · 관대 단계도 영구 실패면 abandoned/ 로.
// 일시 실패가 끝내 안 풀리면 false - 서버를 띄우지 않는다 (사용자 결정 2026-10-08: 띄우면 그 캐릭터가 저장 전 상태로 들어간다)
bool ReplayCommitJournal(DBConnection* conn, const string& dir);

// 베이스캠프 인벤토리(보관 · 반입 · 착용)를 DB 에서 읽어 S_INVENTORY_SYNC 로 보낸다 (UI 4단계 · S3).
// 베이스캠프에 들어오는 세 지점 - 로그인 · 탈출 커밋 완료 · 던전 퇴장 - 에서 DB 스레드가 부른다.
// 클라는 베이스캠프에 있을 때 받은 동기화를 전체 교체로 다룬다(던전 동기화에는 보관이 없다).
void SendBaseCampInventory(DBConnection* conn, GameSessionRef session, uint64 characterId);

// 런 끝 저장(탈출 · 만료)을 기다리는 사이 끊고 다시 로그인해 베이스캠프에 있는 세션에 결과를 다시 보낸다.
// original 이 아직 붙어 있으면 아무것도 안 한다 (사망은 OnBagCreated 가 같은 일을 한다, 2026-10-08 리뷰 ③)
void ResendBaseCampIfRelogged(DBConnection* conn, const GameSessionRef& original, uint64 characterId);

extern atomic<bool> GFailDeathCommit;
extern atomic<int32> GFailEscapeCommits;

/*-----------------------
	DeathCommitTracker
------------------------*/

class DeathCommitTracker
{
public:
	bool TryBegin(uint64 characterId);
	void End(uint64 characterId);
	bool IsPending(uint64 characterId);

private:
	USE_LOCK;
	unordered_set<uint64> _pending;
};

extern DeathCommitTracker GDeathCommits;
extern atomic<uint32> GDeathCommitDelayMs;
extern atomic<uint32> GBaseCampDelayMs;

// 부하 측정 전용 (G2) - bot_ 캐릭터가 비어 있는 부위로 입장하면 기본 3점을 런 중 생성 아이템으로 채운다.
// 첫 사망에 장비를 잃은 봇이 맨몸으로 돌아와 스킬이 막히고 부하가 1/3 로 줄던 문제 (roadmap.md M2-6)
extern atomic<bool> GBotRegear;

// 부하 측정 · 테스트 픽스처 전용 - 진입 지점 대신 예전처럼 DB 저장 좌표에서 입장한다 (FBTK_ENTRY_FROM_DB=1).
// 봇이 진입 지점 몇 곳에 몰리면 이전 측정과 비교가 안 된다 (design/ux/entry-point.md)
extern atomic<bool> GEntryFromDb;

// 맨몸 부위를 채우고 채운 개수를 돌려준다. 이름이 bot_ 으로 시작하지 않으면 0
int32 RegearBot(Player& player, const string& characterName);

// 기본 장비(starter_kit.csv)로 비어 있는 부위를 채운다 - 런 중 생성(isNew) 아이템. 채운 개수.
// 입장 때 착용 · 소지 · 창고가 모두 빈 캐릭터에게 (account-login-mvp-2026-10-09.md Core Rule 8 · ADR-0011 Decision 4)
int32 EquipStarterKit(Player& player);

// 검증 전용 (FBTK_FAKE_LOGIN_FAIL=N) - 처음 N 번의 로그인은 캐릭터 조회가 실패한 것처럼 처리한다 (AC-AL-07)
extern atomic<int32> GFakeLoginFail;

// 검증 전용 (FBTK_LOGIN_DELAY_MS) - 로그인 DB 작업을 이만큼 늦춘다. 처리 중에 끊긴 접속이 계정을 잡아 두지 않는지 본다
extern atomic<uint32> GLoginDelayMs;

// 송신 통계의 가상 패킷 칸 - S_SPAWN · S_DESPAWN 중 가방 · 상자 몫을 따로 센다 (SendBuffer::SetStatId)
constexpr uint16 STAT_ID_BAG_SPAWN = 1900;
constexpr uint16 STAT_ID_BAG_DESPAWN = 1901;

extern atomic<uint64> GDisconnectGraceUs;

// /gold N - 런 골드 디버그 (FBTK_DEBUG_GOLD=1, escape-timer.md Core Rule 18). 꺼져 있으면 명령을 무시한다
extern atomic<bool> GDebugGold;

// /bag · /unbag · /die · /stun · /root · /silence · /slow - 디버그 채팅 명령 (FBTK_DEBUG_COMMANDS=1). 기본 꺼짐.
// 예전에는 접속한 누구나 남에게 CC 를 걸고 가방을 지울 수 있었다 (ADR-0007 · 2026-10-07)
extern atomic<bool> GDebugCommands;

// /t gear · spawn · mode · clear · heal · cd · god - 테스트 모드 (FBTK_TEST_MODE=1, 2026-10-11). 켜면 디버그 명령도 켜진다. 기본 꺼짐
extern atomic<bool> GTestMode;


/*------------------
	DetachedBodies
-------------------*/

// 접속이 끊겨 던전에 남은 몸. 재접속과 유예 만료가 같은 몸을 두고 경쟁하므로
// "표에서 먼저 꺼낸 쪽만 이긴다."로 판정한다.
class DetachedBodies
{
public:
	void		Add(uint64 characterId, PlayerRef body);
	PlayerRef	TryTake(uint64 characterId);
	bool		Contains(uint64 characterId);

private:
	USE_LOCK;
	unordered_map<uint64, PlayerRef> _bodies;
};

extern DetachedBodies GDetachedBodies;
