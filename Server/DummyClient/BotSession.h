#pragma once
#include "Session.h"

/*-----------------
	 BotSession
-----------------*/

class BotSession : public PacketSession
{
public:
	virtual void OnConnected() override;
	virtual void OnRecvPacket(BYTE* buffer, int32 len) override;
	virtual void OnDisconnected() override;

public:
	atomic<bool>		inGame = false;
	uint32				floorId = 0;
	atomic<uint64>		objectId = 0;

	uint32				botIndex = 0;

	atomic<uint64>		characterId = 0;

	atomic<bool>		alive = true;

	float				x = 0.f;
	float				y = 0.f;
	float				z = 100.f;
	float				destX = 0.f;
	float				destY = 0.f;

	uint64				nextAttackAtUs = 0;

	uint64				nextSkillAtUs = 0;
	uint64				castUnitlUs = 0;

	atomic<uint64>		hardCcUntilUs = 0;

	atomic<uint64>		slowUntilUs = 0;
	atomic<float>		slowMagnitude = 0.f;

	uint64				nextDashAtUs = 0;
	uint64				nextAreaAtUs = 0;
	atomic<uint64>		dashUntilUs = 0;
	float				dashDirX = 0.f;
	float				dashDirY = 0.f;

	// ── 아이템 행동 (-items · G2) ───────────────────────────────────────────
	// 패킷 스레드(IOCP)와 틱 스레드가 같이 만진다 - itemLock 아래에서만 읽고 쓴다
	mutex											itemLock;
	unordered_map<uint64, pair<float, float>>		bags;			// 아는 가방 · 상자 위치 (S_SPAWN · S_DESPAWN)
	unordered_map<uint64, Protocol::ItemInstance>	items;			// 내 소지 · 착용 (S_INVENTORY_SYNC · S_ITEM_RESULT)
	uint64											openBagId = 0;	// 열기를 보내고 내용을 기다리는 가방. 0 = 없음
	uint64											nextLootAtUs = 0;
	uint64											nextEquipAtUs = 0;
	uint64											escapeAtUs = 0;	// 탈출을 보낼 시각. 0 = 예약 없음(보냈거나 아직 입장 전)
	atomic<uint64>									reenterAtUs = 0;	// 탈출 뒤 다시 입장할 시각. 0 = 없음
	atomic<uint32>									requestSeq = 0;

	// -burst 동안 쥐고 있는 패킷 - 보낼 순서 그대로 (틱 스레드 전용)
	vector<SendBufferRef>							heldPackets;

	// -monsterpull - 아는 몬스터 위치 (S_SPAWN · S_MOVE · S_DESPAWN · S_DIED). itemLock 아래에서만
	unordered_map<uint64, pair<float, float>>		monsters;

	// -swap - 입장 3 초 뒤 갑옷을 벗고 2 초 뒤 다시 입는다 (장비 스탯 · 체력 비율 유지 시험). 틱 스레드 전용, swapAtUs 만 패킷 스레드가 쓴다
	atomic<uint64>									swapAtUs = 0;
	int32											swapStep = 0;
	uint64											swapItemId = 0;

	// -chat + -chatafter <ms> - 입장 뒤 이 시각에 -chat 명령을 보낸다. 0 = 없음
	atomic<uint64>									chatAtUs = 0;

	// -tower 길찾기 (BotPath) - 입장마다 패킷 스레드가 replanPath 를 켜고, 틱 스레드가 길을 다시 찾는다. path 는 틱 스레드 전용
	atomic<bool>									replanPath = false;
	vector<pair<float, float>>						path;
	size_t											pathIndex = 0;

	// -mode coop (PVE 협동, ADR-0014) - 파티 상태 (S_PARTY_STATE). 번호로 나눈 첫 봇이 나머지를 초대하고, 서버가 말하는 파티장이 입장한다
	mutex											partyLock;
	vector<uint64>									partyMembers;		// 내 파티의 캐릭터 id (나 포함)
	atomic<uint64>									partyId = 0;
	atomic<bool>									partyLeader = false;	// 서버가 말한 파티장
	atomic<bool>									partyInRun = false;
	atomic<bool>									coopEnterSent = false;
	atomic<uint64>									nextInviteAtUs = 0;	// 번호 파티장이 빠진 멤버를 다시 초대할 시각. 0 = 초대하지 않는다

	bool			IsPartyMember(uint64 characterId)
	{
		lock_guard<mutex> guard(partyLock);
		return find(partyMembers.begin(), partyMembers.end(), characterId) != partyMembers.end();
	}

	// 서버가 세션별로 같은 요청 id 를 한 번만 받는다 (GameSession::TryBeginRequest)
	string			NextRequestId() { return "b" + to_string(requestSeq.fetch_add(1)); }
};

constexpr float					BOT_SLOW_CAP = 0.4f;
constexpr float					DASH_SPEED = 1200.f;
constexpr float					DASH_DIST = 400.f;
constexpr uint64				DASH_DURATION_US = static_cast<uint64>(DASH_DIST / DASH_SPEED * 1'000'000.0);

// 입장 후 이 범위 안 무작위 시각에 탈출한다. 실제 런(수십 분)보다 훨씬 잦다 - 탈출 커밋 부하를
// 상한 쪽으로 보려는 값이다. 결과를 쓸 때는 보고 줄의 escapes/s 로 실제 빈도에 맞춰 환산할 것
constexpr float					BOT_ESCAPE_MIN_SEC = 20.f;
constexpr float					BOT_ESCAPE_MAX_SEC = 60.f;
constexpr uint64				BOT_REENTER_DELAY_US = 2'000'000;	// 베이스캠프(S3)에 머무는 시간 흉내

using							BotSessionRef = std::shared_ptr<BotSession>;

extern mutex					GBotsLock;
extern vector<BotSessionRef>	GBots;
extern vector<uint32>			GFreeBotIndices;

// -items 일 때만 켜진다. 꺼져 있으면 봇은 예전처럼 싸우기만 한다 (G2 의 A 조건)
extern atomic<bool> GItemBehavior;

// -noclientms: C_MOVE.client_ms 를 비운다(예전 이동 검증과 비교용) · -burst: 이동을 주기적으로 쥐었다 몰아 보낸다
extern atomic<bool> GNoClientMs;
extern atomic<bool> GBurst;

// -monsterpull: 봇이 다른 봇 대신 가장 가까운 몬스터를 쫓아 때린다 (monster-ai.md AC-MA-70 · 71)
// -entry <id>: 입장 지점을 고른다. 0 = 서버가 무작위 (봇은 벽을 모르므로 몬스터까지 직선이 트인 지점을 준다)
extern atomic<bool> GMonsterPull;
extern atomic<uint32> GEntryPointId;
extern atomic<uint64> GMonsterKills;
extern atomic<uint64> GMonsterAttacksSent;

// -tower <x> <y>: 그 지점으로 걸어가 서 있는다 (탈출 게이지 시험, escape-timer.md AC-ET-26 · 30) · -gold <N>: 입장하자마자 /gold N
// -idleone: 1 번 봇은 제자리에 서 있기만 한다(이동 · 공격 · 스킬 없음) - 진입 무적 시험 (AC-10)
extern atomic<bool> GIdleOne;
extern atomic<bool> GIdleAll;			// -idleall - 모든 봇이 제자리 (FoW · 테스트 모드 실기동)
extern atomic<int32> GForceFloor;		// -floor N - 모든 봇을 그 층으로 (기본 -1 = 봇 번호로 나눈다)
extern atomic<bool> GTowerMode;
extern float GTowerX;
extern float GTowerY;
extern atomic<uint64> GRunGold;
extern string GEnterChat;		// -chat - 스레드 시작 전에 한 번 쓰고 이후 읽기만
extern atomic<uint32> GChatAfterMs;	// -chatafter - 0 이면 입장하자마자 (저장 재시도 시험: 입장 뒤 DB 를 건드린 다음 /die)
extern atomic<bool> GSwapArmor;	// -swap
extern atomic<bool> GStayDead;	// -staydead - 죽어도 접속을 끊지 않는다
extern atomic<int32> GTowerCount;	// -towercount N - -tower 에서 botIndex <= N 만 타워로, 나머지는 제자리에 서서 지켜본다 (완충 방송 수신자). 0 = 전원
extern atomic<bool> GHoldOnReject;	// -holdonreject - 입장 거절에 끊지 않고 베이스캠프에서 기다렸다 다시 입장 (UE 클라처럼, ADR-0007 R-3 시험)

// PVE (ADR-0014) - -mode pvp|solo|coop · -party N (협동 파티 크기 2~3, 기본 3) · -allyfire (일부러 파티원을 때린다 - 서버 거부 시험)
// PVE 에서는 봇끼리 싸우지 않고 몬스터를 쫓는다 (-monsterpull 과 같다)
extern atomic<int32> GRunMode;		// 0 PvP · 1 솔로 · 2 협동 (Protocol::RunMode 값)
extern atomic<int32> GPartySize;
extern atomic<bool> GAllyFire;
extern atomic<uint64> GLeaveEscaped;
extern atomic<uint64> GLeaveExpired;

// 10초 창마다 보고하고 0 으로 돌린다
extern atomic<uint64> GBagOpenSent;
extern atomic<uint64> GLootSent;
extern atomic<uint64> GEquipSent;
extern atomic<uint64> GEscapeSent;
extern atomic<uint64> GReenterSent;
extern atomic<uint64> GItemOk;
extern atomic<uint64> GItemFail;

constexpr int32 BOT_INVENTORY_SLOTS = 25;	// Player::INVENTORY_SLOT_COUNT

extern atomic<uint64> GGrantTargetId;
extern atomic<uint64> GGrantOk;
extern atomic<uint64> GGrantAlready;
extern atomic<uint64> GGrantNoTarget;
extern atomic<uint64> GGrantError;
extern atomic<uint64> GGrantBad;
extern atomic<uint64> GGrantReplies;
