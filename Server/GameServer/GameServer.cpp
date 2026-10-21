#include "pch.h"
#include <iostream>
#include "ThreadManager.h"
#include "Service.h"
#include "Session.h"
#include "GameSession.h"
#include "GameSessionManager.h"
#include <tchar.h>
#include "Job.h"
#include "Room.h"
#include "ServerStats.h"
#include "SkillTable.h"
#include "ItemTable.h"
#include "ItemRoller.h"
#include "ChestSpawnTable.h"
#include "EntryPointTable.h"
#include "StarterKitTable.h"
#include "BotLoadoutTable.h"
#include "WallTable.h"
#include "Vision.h"
#include "TowerTable.h"
#include "MonsterTable.h"
#include "HouseTable.h"
#include "PatrolRouteGen.h"
#include <filesystem>
#include <fstream>
#include "Monster.h"
#include "EscapeLogic.h"
#include "LonePath.h"
#include "CheatGuard.h"
#include "Economy.h"
#include "BaseCampTable.h"
#include "Telemetry.h"
#include "Bag.h"
#include "DBJobQueue.h"
#include "ObjectUtils.h"
#include "InstanceManager.h"
#include "PartyManager.h"
#include "NavGrid.h"
#include "ServerConfig.h"
#include "CrashDump.h"
#include "GlobalQueue.h"
#include "JobTimer.h"

enum
{
	WORKER_TICK = 64
};

// FoW 로그 대상 (RoomVision.cpp) - FBTK_FOW_LOG
extern atomic<uint64> GFowLogViewer;

namespace
{
	// 송신 통계(SV-5 item · top 줄)에 찍을 이름. 서버가 보내는 패킷만 - 목록에 없는 id 는 숫자로 찍힌다
	const char* PacketName(uint16 id)
	{
		switch (id)
		{
		case PKT_S_LOGIN:			return "S_LOGIN";
		case PKT_S_ENTER_GAME:		return "S_ENTER_GAME";
		case PKT_S_LEAVE_GAME:		return "S_LEAVE_GAME";
		case PKT_S_SPAWN:			return "S_SPAWN";
		case PKT_S_DESPAWN:			return "S_DESPAWN";
		case PKT_S_MOVE:			return "S_MOVE";
		case PKT_S_ATTACK:			return "S_ATTACK";
		case PKT_S_ATTACK_CANCEL:	return "S_ATTACK_CANCEL";
		case PKT_S_DAMAGE:			return "S_DAMAGE";
		case PKT_S_CC:				return "S_CC";
		case PKT_S_CC_STATE:		return "S_CC_STATE";
		case PKT_S_DIED:			return "S_DIED";
		case PKT_S_CHAT:			return "S_CHAT";
		case PKT_S_SKILL_CAST:		return "S_SKILL_CAST";
		case PKT_S_SKILL_CANCEL:	return "S_SKILL_CANCEL";
		case PKT_S_SKILL_HIT:		return "S_SKILL_HIT";
		case PKT_S_EQUIP_SYNC:		return "S_EQUIP_SYNC";
		case PKT_S_GRANT_REWARD:	return "S_GRANT_REWARD";
		case PKT_S_ITEM_RESULT:		return "S_ITEM_RESULT";
		case PKT_S_INVENTORY_SYNC:	return "S_INVENTORY_SYNC";
		case PKT_S_BAG_CONTENTS:	return "S_BAG_CONTENTS";
		case PKT_S_ESCAPE_GAUGE:	return "S_ESCAPE_GAUGE";
		case PKT_S_CHEST_STATE:		return "S_CHEST_STATE";
		case PKT_S_PARTY_INVITED:	return "S_PARTY_INVITED";
		case PKT_S_PARTY_STATE:		return "S_PARTY_STATE";
		case STAT_ID_BAG_SPAWN:		return "S_SPAWN(bag)";
		case STAT_ID_BAG_DESPAWN:	return "S_DESPAWN(bag)";
		default:					return nullptr;
		}
	}

	// ── 정상 종료 (2026-10-21 리뷰 후속) ──
	// 예전엔 main 이 while (true) 였다 - Ctrl+C · 창 닫기 · Stop.bat 은 프로세스를 그 자리에서 끝냈고, DB 큐에 들어가 있던 저장은 사라졌다.
	// 이제 Ctrl+C / 창 닫기 / 로그오프 / 시스템 종료가 전부 여기로 들어와 RunShutdown 이 순서대로 정리한다
	atomic<bool> GShutdownRequested{ false };
	atomic<bool> GShutdownUrgent{ false };		// 창 닫기 · 로그오프 - OS 가 핸들러에 주는 시간이 짧다 (약 5 s)
	atomic<bool> GShutdownDone{ false };
	atomic<bool> GWorkersStop{ false };

	BOOL WINAPI OnConsoleCtrl(DWORD type)
	{
		switch (type)
		{
		case CTRL_C_EVENT:
		case CTRL_BREAK_EVENT:
			GShutdownRequested.store(true);
			return TRUE;

		case CTRL_CLOSE_EVENT:
		case CTRL_LOGOFF_EVENT:
		case CTRL_SHUTDOWN_EVENT:
			// 이 핸들러가 돌아가면 OS 가 프로세스를 끝낸다 - 주 스레드의 RunShutdown 이 끝날 때까지 여기서 기다린다 (최대 4.5 s)
			GShutdownUrgent.store(true);
			GShutdownRequested.store(true);
			for (int32 i = 0; i < 90 && GShutdownDone.load() == false; i++)
				::Sleep(50);
			return TRUE;

		default:
			return FALSE;
		}
	}

	template<typename Pred>
	bool WaitUntil(Pred pred, uint32 timeoutMs, uint32 pollMs = 50)
	{
		const uint64 deadline = Utils::NowMilliseconds() + timeoutMs;
		while (pred() == false)
		{
			if (Utils::NowMilliseconds() >= deadline)
				return false;
			this_thread::sleep_for(chrono::milliseconds(pollMs));
		}
		return true;
	}

	void RunShutdown(ServerServiceRef& service, const ServerConfig& config)
	{
		const bool urgent = GShutdownUrgent.load();
		const uint64 startUs = Utils::NowMicroseconds();
		const uint32 drainMs = urgent ? 2'500 : config.shutdownDrainMs;
		LOG_LINE << "[SHUTDOWN] begin - " << (urgent ? "console close" : "ctrl-c") << " (db drain budget " << drainMs << " ms)\n";

		// 1. 새 연결을 받지 않는다 · 방의 Flush 루프가 다시 예약되지 않는다 (JobTimer::Stop)
		service->CloseService();
		GJobTimer->Stop();

		// 2. 세션을 끊는다 - 끊기 직전에 S_KICK{SERVER_SHUTDOWN} 을 보내 클라가 이유를 보이게. 던전의 몸은 유예 몸이 되고(DB 없음),
		//    베이스캠프 몸은 사라진다. 진행 중이던 런 끝 저장은 DB 큐에 그대로 있다
		const int32 dropped = GSessionManager.DisconnectAll(L"server shutdown", [](const GameSessionRef& session)
			{
				Protocol::S_KICK kickPkt;
				kickPkt.set_reason(Protocol::KICK_REASON_SERVER_SHUTDOWN);
				session->Send(ServerPacketHandler::MakeSendBuffer(kickPkt));
			});

		// 3. 방 잡이 가라앉을 때까지 - 글로벌 큐가 비고 200 ms 그대로면 끝 (상한 2 s)
		int32 quiet = 0;
		WaitUntil([&quiet]() { quiet = (GGlobalQueue->Size() == 0) ? quiet + 1 : 0; return quiet >= 4; }, 2'000);

		// 4. DB 큐 - 남은 저장(사망 · 탈출 · 만료 · 안티치트)을 끝까지. 시간 안에 못 끝내도 런 끝 저장은 write-ahead 기록이 있어 다음 부팅이 끝낸다
		const bool drained = WaitUntil([]() { return GDBQueue.GetPendingCount() == 0 && GDBQueue.GetDelayedCount() == 0; }, drainMs);
		GDBQueue.Shutdown();		// 남은 작업을 한 번씩 돌리고 워커를 합친다

		// 5. IOCP 워커 종료 - Dispatch(10 ms) 에서 깨어나 깃발을 본다
		GWorkersStop.store(true);
		GThreadManager->Join();

		LOG_LINE << "[SHUTDOWN] done - sessions_dropped=" << dropped << " db_drained=" << (drained ? "yes" : "TIMEOUT")
			<< " took=" << (Utils::NowMicroseconds() - startUs) / 1000 << "ms\n";
		GetLogger().Flush();
		GShutdownDone.store(true);
	}
}

void DoWorkerJob(ServerServiceRef& service)
{
	while (GWorkersStop.load() == false)
	{
		const uint64 nowMs = Utils::NowMilliseconds();
		LEndTickCount = nowMs + WORKER_TICK;

		// 네트워크 입출력 처리 -> 인게임 로직까지 (패킷 핸들러에 의해).
		// 대기는 가장 이른 타이머까지만 (최대 10 ms) - 늘 10 ms 를 자면 33 ms Flush 예약이 그만큼 늦게 분배됐다 (2026-10-21)
		service->GetIocpCore()->Dispatch(GJobTimer->MsUntilNext(nowMs, 10));

		// 예약된 일감 처리
		ThreadManager::DistributeReservedJobs();

		// 글로벌 큐
		ThreadManager::DoGlobalQueueWork();
	}
}

int main()
{
	::setvbuf(stdout, nullptr, _IONBF, 0);

	// 설정 - 환경변수 FBTK_* 한 번에 (ServerConfig.h). 예전엔 main 이 블록 26 벌로 하나씩 읽었다
	const ServerConfig config = ServerConfig::FromEnv();

	// 크래시 덤프 - 처리되지 않은 예외 · ASSERT_CRASH · terminate 가 Logs/crash 에 .dmp 를 남긴다 (예전엔 덤프 없이 사라졌다)
	CrashDump::Install(config.crashDir, "GameServer");
	LOG_LINE << "[BOOT] crash dumps -> " << config.crashDir << '\n';
	config.LogSummary();

	ServerPacketHandler::Init();

	// G2 · SV-5 - 아이템 이벤트가 만드는 송신량을 따로 본다. 가방 · 상자의 생성 · 소멸은 가상 칸으로 센다
	GStats.SetPacketNames(PacketName);
	GStats.SetItemPackets({ STAT_ID_BAG_SPAWN, STAT_ID_BAG_DESPAWN, PKT_S_BAG_CONTENTS,
		PKT_S_ITEM_RESULT, PKT_S_INVENTORY_SYNC, PKT_S_EQUIP_SYNC });

	// Dump 의 db queue 줄 - 큐 깊이는 여기서 넣는다 (ServerStats 는 DB 모듈을 모른다), 콘텐츠 값은 진행 중인 사망 저장 · 살아 있는 인스턴스
	GStats.SetDbQueueProbe([](int32& pending, int32& delayed) { pending = GDBQueue.GetPendingCount(); delayed = GDBQueue.GetDelayedCount(); });
	GStats.SetExtraLine([]() { return "death_commits=" + to_string(GDeathCommits.Count()) + " instances=" + to_string(GInstances.LiveCount()); });

	// 통계 CSV - 사람이 읽는 블록과 같은 값을 행으로 (그래프 · 전후 비교). 못 열어도 서버는 돈다
	if (config.metricsDir.empty() == false)
	{
		if (GStats.EnableCsv(config.metricsDir))
			LOG_LINE << "[STATS] csv -> " << config.metricsDir << '\n';
		else
			LOG_LINE << "[STATS] cannot open " << config.metricsDir << " - csv off\n";
	}

	{
		const string& dataDir = config.dataDir;
		string error;

		// 스킬 로스터 - ItemTable 보다 먼저. 풀이 스킬 id 를 검사한다 (2026-10-10, src/스킬_목록.csv 76개 → skills.csv)
		if (SkillTable::Load(dataDir, OUT error) == false)
		{
			LOG_LINE << "[BOOT] skill data invalid - " << error << " (dir=" << dataDir << ")\n";
			return -1;
		}

		LOG_LINE << "[BOOT] skill data loaded - skills=" << SkillTable::All().size() << " dot_tick=" << SkillTable::Rules().dotTickMs
			<< "ms wall_stun=" << SkillTable::Rules().wallStunMs << "ms motions=" << SkillTable::Motions().size()
			<< " mapped=" << SkillTable::MappedMotionCount() << "/" << SkillTable::All().size() << '\n';

		if (ItemTable::Load(dataDir, OUT error) == false)
		{
			LOG_LINE << "[BOOT] item data invalid - " << error << " (dir=" << dataDir << ")\n";
			return -1;
		}

		LOG_LINE << "[BOOT] item data loaded - types=" << ItemTable::All().size() << " pools=" << ItemTable::PoolCount()
			<< " stat_lines=" << ItemTable::StatLineCount() << " grade_x=" << ItemTable::GradeMultiplier(Protocol::ITEM_GRADE_COMMON)
			<< "/" << ItemTable::GradeMultiplier(Protocol::ITEM_GRADE_LEGENDARY) << '\n';

		// 경제 1차 (2026-10-17) - 강화 비용 · 매입가 · 사망 차감 · 전리품 종류. 전리품 id 는 장비 표와 겹치면 안 된다
		if (EconomyRules::Get().Load(dataDir, OUT error) == false || LootTable::Load(dataDir, OUT error) == false)
		{
			LOG_LINE << "[BOOT] economy invalid - " << error << " (dir=" << dataDir << ")\n";
			return -1;
		}
		for (uint32 id = LootTable::ID_MIN; id <= LootTable::ID_MAX; id++)
		{
			if (LootTable::IsLoot(id) && ItemTable::Find(id) != nullptr)
			{
				LOG_LINE << "[BOOT] economy invalid - loot id " << id << " is also an item type\n";
				return -1;
			}
		}
		LOG_LINE << "[BOOT] economy - loot types=" << LootTable::TypeCount() << " drops=" << LootTable::Drops().size()
			<< " enhance=" << EconomyRules::Get().enhanceCostBase << "x" << EconomyRules::Get().enhanceCostGrowth
			<< "^L death_loss=" << EconomyRules::Get().deathGoldLossPct << "%\n";

		// 고독한 길 (2026-10-17) - 옵션 확률 · 다구대일 효과. 파일이 없으면 기본값
		if (LonePathRules::Get().Load(dataDir, OUT error) == false)
		{
			LOG_LINE << "[BOOT] lone_path invalid - " << error << " (dir=" << dataDir << ")\n";
			return -1;
		}
		LOG_LINE << "[BOOT] lone path - roll=" << LonePathRules::Get().rollPct << "% max_pieces=" << LonePathRules::Get().maxPieces
			<< " outnumber=" << LonePathRules::Get().outnumberEnemies << "@" << LonePathRules::Get().outnumberRadiusCm << "cm\n";

		// 안티치트 (2026-10-19) - 위반 가중치 · 경고 / 끊기 점수 · 정지 규칙. 파일이 없으면 기본값
		if (CheatRules::Get().Load(dataDir, OUT error) == false)
		{
			LOG_LINE << "[BOOT] anticheat invalid - " << error << " (dir=" << dataDir << ")\n";
			return -1;
		}
		{
			const CheatRules& ac = CheatRules::Get();
			LOG_LINE << "[BOOT] anticheat - " << (ac.enabled ? "on" : "OBSERVE ONLY") << " warn=" << ac.warnScore << " kick=" << ac.kickScore
				<< " decay=" << ac.decayPerSec << "/s ban=" << ac.strikesForBan << " strikes in " << ac.banWindowHours << "h -> " << ac.banMinutes << " min\n";
		}

		// 층 아이템 풀 - ItemTable 다음이어야 한다. 부위 · 물리/마법 행에 뽑을 종류가 있는지 ItemTable 로 검사한다.
		if (FloorItemPool::Load(dataDir, OUT error) == false)
		{
			LOG_LINE << "[BOOT] floor item pool invalid - " << error << " (dir=" << dataDir << ")\n";
			return -1;
		}

		LOG_LINE << "[BOOT] floor item pool loaded - pools=" << FloorItemPool::Count() << '\n';

		// 기본 장비 - ItemTable 다음이어야 한다. 종류가 그 부위에 맞는지, 스킬이 그 종류의 풀에 있는지 검사한다 (ADR-0011)
		if (StarterKitTable::Load(dataDir, OUT error) == false)
		{
			LOG_LINE << "[BOOT] starter kit invalid - " << error << " (dir=" << dataDir << ")\n";
			return -1;
		}

		LOG_LINE << "[BOOT] starter kit loaded - pieces=" << StarterKitTable::All().size() << '\n';

		// 봇 스킬 셋 - 기본 장비와 같은 검사 (ItemTable 다음). 파일이 없으면 빈 표 (FBTK_BOT_LOADOUTS 가 켜져도 기본 장비로)
		if (BotLoadoutTable::Load(dataDir, OUT error) == false)
		{
			LOG_LINE << "[BOOT] bot loadouts invalid - " << error << " (dir=" << dataDir << ")\n";
			return -1;
		}

		LOG_LINE << "[BOOT] bot loadouts loaded - builds=" << BotLoadoutTable::All().size() << '\n';

		// 상자 배치 - 층 풀 다음이어야 한다. 상자마다 그 층 · 등급의 풀이 있는지 검사한다.
		if (ChestSpawnTable::Load(dataDir, FLOOR_COUNT, OUT error) == false)
		{
			LOG_LINE << "[BOOT] chest spawns invalid - " << error << " (dir=" << dataDir << ")\n";
			return -1;
		}

		// 진입 지점 - 층마다 하나 이상이어야 입장할 곳이 있다 (design/ux/entry-point.md)
		if (EntryPointTable::Load(dataDir, FLOOR_COUNT, OUT error) == false)
		{
			LOG_LINE << "[BOOT] entry points invalid - " << error << " (dir=" << dataDir << ")\n";
			return -1;
		}

		LOG_LINE << "[BOOT] entry points loaded - " << EntryPointTable::All().size() << '\n';

		// 서버 벽 - 진입 지점 다음이어야 한다. 지점 9 m 안에 벽이 있는지 검사한다 (ADR-0006 · level-map.md Core Rule 15)
		if (WallTable::Load(dataDir, FLOOR_COUNT, EntryPointTable::All(), OUT error) == false)
		{
			LOG_LINE << "[BOOT] walls invalid - " << error << " (dir=" << dataDir << ")\n";
			return -1;
		}

		for (uint32 floor = 0; floor < FLOOR_COUNT; floor++)
		{
			const string& version = WallTable::DataVersion(floor);
			if (version.empty() && WallTable::Get(floor).Count() == 0)
				LOG_LINE << "[LEVEL] floor=" << floor << " walls=0 (TEMP)\n";
			else
				LOG_LINE << "[LEVEL] floor=" << floor << " walls=" << WallTable::Get(floor).Count()
					<< " version=" << (version.empty() ? "none" : version) << '\n';
		}

		// 몬스터 길찾기 격자 (monster-ai.md § K, 2026-10-17) - 벽 다음. 벽이 있는 층만 만든다 (나머지는 직선 이동 그대로)
		{
			const uint64 navStartUs = Utils::NowMicroseconds();
			const uint32 navFloors = NavGridTable::Build(FLOOR_COUNT);
			const uint64 navMs = (Utils::NowMicroseconds() - navStartUs) / 1000;
			for (uint32 floor = 0; floor < FLOOR_COUNT; floor++)
			{
				if (const NavGrid* nav = NavGridTable::Get(floor))
					LOG_LINE << "[NAV] floor=" << floor << " cell=" << NavGrid::CELL_CM << "cm walkable(r45)=" << nav->WalkableCells(45.f)
						<< " walkable(r110)=" << nav->WalkableCells(110.f) << '\n';
			}
			LOG_LINE << "[NAV] grids=" << navFloors << " built in " << navMs << "ms\n";
		}

		// FoW 시야 규칙 (design/gdd/fog-of-war.md, 2026-10-11) - 벽 다음. 시선 판정이 벽을 읽는다
		if (Vision::Load(dataDir, OUT error) == false)
		{
			LOG_LINE << "[BOOT] vision rules invalid - " << error << " (dir=" << dataDir << ")\n";
			return -1;
		}

		Vision::SetEnabled(config.fowEnabled);
		if (config.fowLogViewer != 0)
		{
			GFowLogViewer.store(config.fowLogViewer);
			LOG_LINE << "[BOOT] FBTK_FOW_LOG=" << GFowLogViewer.load() << " - that character's FoW spawns / despawns are logged (TEST ONLY)\n";
		}

		LOG_LINE << "[BOOT] fow " << (Vision::Enabled() ? "ON" : "OFF - FBTK_FOW=0, everyone sees everything (A/B COMPARISON ONLY)")
			<< " radius=" << Vision::Rules().radiusCm << "cm grace=" << Vision::Rules().graceMs << "ms\n";

		// 탈출 타워 - 몬스터 자리 검증(35 m)과 탈출 게이지가 읽는다. 파일이 없으면 0개
		if (TowerTable::Load(dataDir, FLOOR_COUNT, OUT error) == false)
		{
			LOG_LINE << "[BOOT] towers invalid - " << error << " (dir=" << dataDir << ")\n";
			return -1;
		}

		// 몬스터 - 스킬 · 층 풀 · 진입 지점 · 벽 · 타워 다음이어야 한다. 자리마다 벽 겹침 · 진입 20 m · 타워 35 m 를 본다
		if (MonsterTable::Load(dataDir, FLOOR_COUNT, OUT error) == false)
		{
			LOG_LINE << "[BOOT] monsters invalid - " << error << " (dir=" << dataDir << ")\n";
			return -1;
		}

		LOG_LINE << "[BOOT] towers=" << TowerTable::All().size() << " monster types=" << MonsterTable::TypeCount()
			<< " spawns=" << MonsterTable::Spawns().size() << '\n';

		// 전리품 드랍의 몬스터 종류가 있어야 한다 (2026-10-17 경제)
		for (const LootDropDef& d : LootTable::Drops())
		{
			if (MonsterTable::FindType(d.monsterTypeId) == nullptr)
			{
				LOG_LINE << "[BOOT] economy invalid - loot_drops monster_type_id " << d.monsterTypeId << " is not in monster_types.csv\n";
				return -1;
			}
		}

		// 베이스캠프 맵 (2026-10-17) - 개인 방의 배치. 섬 밖이어야 하고 허수아비 종류가 있어야 한다. 파일이 없으면 맵 없음 (UI 베이스캠프만)
		if (BaseCampTable::Load(dataDir, OUT error) == false || BaseCampTable::CheckClearOf(WallTable::Get(0), OUT error) == false)
		{
			LOG_LINE << "[BOOT] basecamp invalid - " << error << " (dir=" << dataDir << ")\n";
			return -1;
		}
		if (BaseCampTable::Loaded() && BaseCampTable::DummyTypeId() != 0 && MonsterTable::FindType(BaseCampTable::DummyTypeId()) == nullptr)
		{
			LOG_LINE << "[BOOT] basecamp invalid - dummy monster type " << BaseCampTable::DummyTypeId() << " is not in monster_types.csv\n";
			return -1;
		}
		LOG_LINE << "[BOOT] basecamp " << (BaseCampTable::Loaded() ? "map" : "off (no basecamp.csv)") << " npcs=" << BaseCampTable::Npcs().size()
			<< " blockers=" << BaseCampTable::Walls().Count() << " dummy=" << BaseCampTable::DummyTypeId() << '\n';

		// 집 볼륨 · 무리 규칙 · 순찰 길 생성 (2026-10-12 pack-routes) - 벽 · 타워 · 진입 지점 · 몬스터 종류 다음.
		// FBTK_ROUTE_SEED 가 있으면 그 시드(재현), 없으면 부팅 시각 - 부팅마다 다른 동선
		if (HouseTable::Load(dataDir, FLOOR_COUNT, OUT error) == false)
		{
			LOG_LINE << "[BOOT] houses invalid - " << error << " (dir=" << dataDir << ")\n";
			return -1;
		}
		if (PackRulesTable::Load(dataDir, FLOOR_COUNT, OUT error) == false)
		{
			LOG_LINE << "[BOOT] pack rules invalid - " << error << " (dir=" << dataDir << ")\n";
			return -1;
		}
		{
			uint64 routeSeed = Utils::NowMicroseconds();
			if (config.routeSeed != 0)
			{
				routeSeed = config.routeSeed;
				LOG_LINE << "[BOOT] FBTK_ROUTE_SEED=" << routeSeed << " - patrol routes are reproducible (TEST ONLY)\n";
			}
			if (MonsterTable::GenerateRouteSets(FLOOR_COUNT, routeSeed, OUT error) == false)
			{
				LOG_LINE << "[BOOT] patrol routes invalid - " << error << " (dir=" << dataDir << ")\n";
				return -1;
			}
			for (const PackRules& rules : PackRulesTable::All())
			{
				const string routeDir = dataDir + "\\..\\Logs\\routes";
				error_code ec;
				filesystem::create_directories(routeDir, ec);
				for (size_t k = 0; k < MonsterTable::RouteSetCount(rules.floorId); k++)
				{
					const RouteSet* set = MonsterTable::RouteSetAt(rules.floorId, k);
					const RouteGenStats& st = set->stats;
					LOG_LINE << "[ROUTE] floor=" << rules.floorId << " set=" << k << " seed=" << set->seed << " routes=" << st.routes
						<< " crossings=" << st.crossings << " same_side=" << st.sameSide << " pockets=" << st.pockets << " side=" << st.sideRoutes
						<< " coverage=" << ::lroundf(st.coveragePct) << "%" << " min_sep=" << ::lroundf(st.minSepCm / 100.f) << "m avg_len=" << ::lroundf(st.avgLengthM)
						<< "m components=" << st.components << " dropped_cells=" << st.droppedCells << " attempts=" << st.attempts << " retries=" << st.retries
						<< " took=" << st.elapsedUs / 1000 << "ms\n";
					// 사람이 보는 사본 (증거 · tools/level/draw_routes.py) - 못 써도 부팅은 계속, 로그만
					const string routePath = routeDir + "\\routes-floor" + to_string(rules.floorId) + "-set" + to_string(k) + ".csv";
					ofstream file(routePath, ios::binary | ios::trunc);
					if (file.is_open())
						file << PatrolRouteGen::ToCsv(*set);
					else
						LOG_LINE << "[ROUTE] export failed - cannot write " << routePath << (ec ? " (" + ec.message() + ")" : string()) << '\n';
				}
			}
		}

		// 탈출 & 체류 타이머 - escape_config.csv, 그다음 FBTK_RUN_LIMIT_S · FBTK_ESCAPE_FILL_S · FBTK_DEBUG_ESCAPE (AC-ET-28)
		{
			EscapeConfig& cfg = EscapeConfig::Get();
			if (cfg.Load(dataDir, OUT error) == false
				|| TowerTable::ValidateForEscape(cfg.zoneLeaveCm, cfg.alertCm, FLOOR_COUNT, OUT error) == false)
			{
				LOG_LINE << "[ESCAPE] reject " << error << " (dir=" << dataDir << ")\n";
				return -1;
			}

			LOG_LINE << "[ESCAPE] config zone=" << cfg.zoneEnterCm << "/" << cfg.zoneLeaveCm << "cm fill=" << cfg.fillUs / 1000 << "ms reverse=x"
				<< cfg.reverseMultX100 / 100.f << " visible=" << cfg.visibleCm << "cm alert=" << cfg.alertCm << "cm run_limit=" << cfg.runLimitUs / 1'000'000
				<< "s kept=" << cfg.expiryGoldKeptPct << "% debug_escape=" << (cfg.debugEscapeCommand ? "on" : "off") << '\n';

			for (int32 i = 0; i < FLOOR_COUNT; i++)
				GRooms[i]->DoAsync(&Room::InitEscape);
		}

		// 운영 스위치 (ServerConfig)
		GDisconnectGraceUs.store(config.disconnectGraceUs);
		LOG_LINE << "[BOOT] disconnect grace=" << GDisconnectGraceUs.load() / 1000 << "ms\n";

		GWallCheck.store(config.wallCheck);
		if (GWallCheck.load() == false)
			LOG_LINE << "[BOOT] FBTK_WALLS=0 - move validation skips the wall check (SOAK COMPARISON / EMERGENCY ONLY)\n";

		GBotRegear.store(config.botRegear);
		if (GBotRegear.load())
			LOG_LINE << "[BOOT] FBTK_BOT_REGEAR=1 - bare bot_ characters get the basic 3 pieces on enter (LOAD TEST ONLY)\n";

		GBotLoadouts.store(config.botLoadouts);
		if (GBotLoadouts.load())
			LOG_LINE << "[BOOT] FBTK_BOT_LOADOUTS=1 - bare bot_ slots get a random build from bot_loadouts.csv on enter\n";

#if FBTK_TEST_HOOKS
		// 시험 전용 훅 - 이 빌드에만 있다 (ServerConfig.h). 로그 문장은 하네스 · 증거 문서가 찾는 그대로
		GDebugGold.store(config.hooks.debugGold);
		if (GDebugGold.load())
			LOG_LINE << "[BOOT] FBTK_DEBUG_GOLD=1 - /gold N adds run gold (TEST ONLY)\n";

		GGrantEndpoint.store(config.hooks.grantEndpoint);
		LOG_LINE << "[BOOT] C_GRANT_REWARD " << (GGrantEndpoint.load() ? "ON - FBTK_GRANT_ENDPOINT=1 (TEST ONLY - any client can grant gold)" : "off") << '\n';

		GDebugCommands.store(config.hooks.debugCommands);

		// 테스트 모드 (design/quick-specs/test-mode-2026-10-11.md) - /t 명령: 장비 · 스킬 바꿔 끼우기 · 더미 적 · 무적. 디버그 명령도 켠다
		GTestMode.store(config.hooks.testMode);
		if (GTestMode.load())
		{
			GDebugCommands.store(true);
			LOG_LINE << "[BOOT] TEST MODE ON - FBTK_TEST_MODE=1: /t gear spawn mode clear heal cd god fow (TEST ONLY)\n";
		}

		LOG_LINE << "[BOOT] debug chat commands (/bag /unbag /die /stun /root /silence /slow) "
			<< (GDebugCommands.load() ? "ON - FBTK_DEBUG_COMMANDS=1 (TEST ONLY)" : "off") << '\n';

		GFailDeathCommit.store(config.hooks.failDeathCommit);
		if (GFailDeathCommit.load())
			LOG_LINE << "[BOOT] FBTK_FAIL_DEATH_COMMIT=1 - every death commit will fail (TEST ONLY)\n";

		GDeathCommitDelayMs.store(config.hooks.deathCommitDelayMs);
		if (GDeathCommitDelayMs.load() > 0)
			LOG_LINE << "[BOOT] FBTK_DEATH_COMMIT_DELAY_MS=" << GDeathCommitDelayMs.load() << " (TEST ONLY)\n";

		GBaseCampDelayMs.store(config.hooks.baseCampDelayMs);
		if (GBaseCampDelayMs.load() > 0)
			LOG_LINE << "[BOOT] FBTK_BASECAMP_DELAY_MS=" << GBaseCampDelayMs.load() << " (TEST ONLY)\n";

		GFailEscapeCommits.store(config.hooks.failEscapeCommits);
		if (GFailEscapeCommits.load() > 0)
			LOG_LINE << "[BOOT] FBTK_FAIL_ESCAPE_COMMIT=" << GFailEscapeCommits.load() << " - first N escape commits fail (TEST ONLY)\n";

		GFakeCommitAckLoss.store(config.hooks.fakeAckLoss);
		if (GFakeCommitAckLoss.load() > 0)
			LOG_LINE << "[BOOT] FBTK_FAKE_ACK_LOSS=" << GFakeCommitAckLoss.load() << " - first N run-end commits report a lost ack after committing (TEST ONLY)\n";

		GFakeLoginFail.store(config.hooks.fakeLoginFail);
		if (GFakeLoginFail.load() > 0)
			LOG_LINE << "[BOOT] FBTK_FAKE_LOGIN_FAIL=" << GFakeLoginFail.load() << " - first N logins fail their character query (TEST ONLY)\n";

		GLoginDelayMs.store(config.hooks.loginDelayMs);
		if (GLoginDelayMs.load() > 0)
			LOG_LINE << "[BOOT] FBTK_LOGIN_DELAY_MS=" << GLoginDelayMs.load() << " - every login waits this long in the DB queue (TEST ONLY)\n";

		if (config.hooks.logStallMs > 0)
		{
			constexpr uint32 STALL_PERIOD_MS = 10'000;
			GetLogger().SetTestStall(config.hooks.logStallMs, STALL_PERIOD_MS);
			LOG_LINE << "[BOOT] FBTK_LOG_STALL_MS=" << config.hooks.logStallMs << " - log writer stalls every " << STALL_PERIOD_MS << "ms (TEST ONLY)\n";
		}

		GEntryFromDb.store(config.hooks.entryFromDb);
		if (GEntryFromDb.load())
			LOG_LINE << "[BOOT] FBTK_ENTRY_FROM_DB=1 - enter at the saved DB position, not an entry point (LOAD TEST / FIXTURE ONLY)\n";
#endif

		// 측정 전용 - 못 열어도 서버는 돈다
		if (GTelemetry.Open(config.telemetryDir))
			LOG_LINE << "[TELEM] writing " << config.telemetryDir << " boot=" << GTelemetry.BootId() << '\n';
		else
			LOG_LINE << "[TELEM] cannot open " << config.telemetryDir << " - telemetry off\n";

		// G3 켬/끔 비교 - 같은 데이터로 몬스터만 끈다 (FBTK_MONSTERS=0)
		GMonstersOn.store(config.monstersOn);		// PVE 인스턴스도 같은 스위치를 따른다 (ADR-0014)
		if (config.monstersOn)
		{
			for (int32 i = 0; i < FLOOR_COUNT; i++)
				GRooms[i]->DoAsync(&Room::InitMonsters);
		}
		else
		{
			LOG_LINE << "[BOOT] FBTK_MONSTERS=0 - no monsters spawn (G3 COMPARISON ONLY)\n";
		}

		// 메모리 전용 - DB 행이 없다. 재시작하면 전층이 Sealed 로 돌아간다 (§ 8, ADR-0003).
		// 워커가 아직 없으므로 DoAsync 가 이 스레드에서 바로 실행된다 (LoadLiveBags 와 같음).
		for (const ChestSpawn& spawn : ChestSpawnTable::All())
			GRooms[spawn.floorId]->DoAsync(&Room::AddBag, ObjectUtils::CreateChest(spawn));

		LOG_LINE << "[BOOT] chests spawned - " << ChestSpawnTable::All().size() << '\n';
	}

	if (config.dbPassword.empty())
	{
		LOG_LINE << "[DB] FBTK_DB_PASSWORD not set" << '\n';
		return -1;
	}

	// 전용 계정 fbtk_app (schema_app_user.sql) - 예전엔 root 가 박혀 있었다. FBTK_DB_USER 로 바꿀 수 있다
	if (GDBPool.Connect(config.dbThreads, config.dbHost.c_str(), config.dbPort, config.dbUser.c_str(), config.dbPassword.c_str(), config.dbSchema.c_str()) == false)
	{
		LOG_LINE << "[DB] pool connect failed - " << config.dbUser << "@" << config.dbHost << ":" << config.dbPort << '\n';
		return -1;
	}
	LOG_LINE << "[DB] pool connected - " << config.dbUser << "@" << config.dbHost << ":" << config.dbPort << " threads=" << config.dbThreads << '\n';

	// 기다리는 런 끝 저장의 기록 (CommitJournal.h, 2026-10-08) - 그 사이 서버가 꺼지면 아래 재처리가 접속을 받기 전에 끝낸다
	GCommitJournalDir = config.journalDir;
	LOG_LINE << "[BOOT] commit journal dir=" << GCommitJournalDir << '\n';

	{
		DBConnection* conn = GDBPool.Pop();
		if (conn == nullptr)
		{
			LOG_LINE << "[BOOT] no DB connection for recovery" << '\n';
			return -1;
		}

		// 순서: id 카운터 → 남은 저장 기록 재처리(카운터를 기록 속 id 위로 올린 뒤) → 만료 가방 정리 → 행 검사 → 살아 있는 가방 적재.
		// 재처리가 만든 사망 가방은 아직 만료 전이라 정리에 걸리지 않고, LoadLiveBags 가 월드에 되살린다
		const bool ok = EnsureCommitJournalTable(conn) && RestoreIdCounters(conn) && ReplayCommitJournal(conn, GCommitJournalDir)
			&& PurgeExpiredBags(conn) && ValidateItemRows(conn) && LoadLiveBags(conn);
		GDBPool.Push(conn);

		if (ok == false)
		{
			LOG_LINE << "[BOOT] recovery failed - refusing to start" << '\n';
			return -1;
		}
	}

	if (GDBQueue.Init(config.dbThreads, &GDBPool) == false)
	{
		LOG_LINE << "[DB] queue init failed" << '\n';
		return -1;
	}

	LOG_LINE << "[DB] connected — " << config.dbThreads << " connections" << '\n';

	// 종료 신호 - Ctrl+C · 창 닫기 · 로그오프 · 시스템 종료 (RunShutdown)
	::SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);

	// 바인드 주소 · 포트 (2026-10-18 플레이테스트 접속 환경) - 기본은 이 PC 만(127.0.0.1:7777). 같은 공간 LAN 플레이테스트는
	// 호스트가 FBTK_BIND_ADDRESS=0.0.0.0 으로 켠다 (tools/playtest-host.ps1 · docs/playtest-network.md)
	wstring bindAddress;
	for (char c : config.bindAddress)
		bindAddress.push_back(static_cast<wchar_t>(c));

	ServerServiceRef service = make_shared<ServerService>(
		NetAddress(bindAddress, config.port),
		make_shared<IocpCore>(),
		[=]() { return make_shared<GameSession>(); },
		100);

	ASSERT_CRASH(service->Start());
	LOG_LINE << "[BOOT] listen port=" << config.port << " - accepting clients (bind=" << config.bindAddress << ")\n";		// 검증 체크리스트 E1 이 찾는 줄 (2026-10-12)

	for (int32 i = 0; i < config.workerThreads; i++)
	{
		GThreadManager->Launch([&service]()
			{
				DoWorkerJob(service);
			});
	}

	for (int32 i = 0; i < FLOOR_COUNT; i++)
	{
		GRooms[i]->DoAsync(&Room::UpdateTick);
		GRooms[i]->DoAsync(&Room::FlushMoves);
		GRooms[i]->DoAsync(&Room::FlushCombat);
		GRooms[i]->DoAsync(&Room::FlushSpawns);
	}

	// 주 루프 - 10 초마다 통계 · 정리. 종료 신호가 오면 빠져나와 RunShutdown
	constexpr uint64 HOUSEKEEPING_US = 10'000'000;
	uint64 lastHousekeepingUs = Utils::NowMicroseconds();

	while (GShutdownRequested.load() == false)
	{
		this_thread::sleep_for(chrono::milliseconds(100));

		const uint64 nowUs = Utils::NowMicroseconds();
		if (nowUs - lastHousekeepingUs < HOUSEKEEPING_US)
			continue;
		lastHousekeepingUs = nowUs;

		GStats.Dump(service->GetCurrentSessionCount());
		GMonsterStats.Dump(10);
		GAudienceStats.Dump();		// AC-3 방송 대상 수 (2026-10-18)

		// 자원 고갈 등으로 포기했던 Accept 슬롯을 다시 건다 (TD-014) - 예전엔 그 슬롯을 영영 잃었다
		service->RetryLostAccepts();

		// 로그인하지 않은 채 붙어 있는 연결 (2026-10-17 사용자 결정 - 10 초)
		if (const int32 dropped = GSessionManager.DropUnauthenticated(nowUs, GameSession::LOGIN_TIMEOUT_US); dropped > 0)
			LOG_LINE << "[RATE] login timeout - dropped " << dropped << " unauthenticated connection(s)\n";

		// 로그인한 뒤 아무 패킷도 없는 연결 (2026-10-21) - 하트비트가 5 s 마다 오므로 30 s 침묵은 멈춘 클라다
		if (const int32 dropped = GSessionManager.DropIdle(nowUs, config.idleTimeoutUs); dropped > 0)
			LOG_LINE << "[RATE] idle timeout - dropped " << dropped << " silent connection(s)\n";

		// PVE (ADR-0014) - 살아 있는 인스턴스 수 · 지금까지 만든 수 · 파티 초대 만료 (30 s, 10 s 단위로 알린다)
		GParties.Tick(nowUs);
		if (const uint64 created = GInstances.CreatedCount(); created > 0)
			LOG_LINE << "[INST] live=" << GInstances.LiveCount() << " created=" << created << '\n';
	}

	RunShutdown(service, config);
	return 0;
}
