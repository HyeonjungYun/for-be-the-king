#pragma once

struct DeathSnapshot;
struct EscapeSnapshot;
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

bool CommitDeath(DBConnection* conn, const DeathSnapshot& snap);

// 베이스캠프 인벤토리(보관 · 반입 · 착용)를 DB 에서 읽어 S_INVENTORY_SYNC 로 보낸다 (UI 4단계 · S3).
// 베이스캠프에 들어오는 세 지점 - 로그인 · 탈출 커밋 완료 · 던전 퇴장 - 에서 DB 스레드가 부른다.
// 클라는 베이스캠프에 있을 때 받은 동기화를 전체 교체로 다룬다(던전 동기화에는 보관이 없다).
void SendBaseCampInventory(DBConnection* conn, GameSessionRef session, uint64 characterId);

void CommitEscape(DBConnection* conn, const EscapeSnapshot& snap);

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

// 맨몸 부위를 채우고 채운 개수를 돌려준다. 이름이 bot_ 으로 시작하지 않으면 0
int32 RegearBot(Player& player, const string& characterName);

// 송신 통계의 가상 패킷 칸 - S_SPAWN · S_DESPAWN 중 가방 · 상자 몫을 따로 센다 (SendBuffer::SetStatId)
constexpr uint16 STAT_ID_BAG_SPAWN = 1900;
constexpr uint16 STAT_ID_BAG_DESPAWN = 1901;

extern atomic<uint64> GDisconnectGraceUs;

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
