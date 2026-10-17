#include "pch.h"
#include <iostream>
#include "ThreadManager.h"
#include "Service.h"
#include "Session.h"
#include "GameSession.h"
#include "GameSessionManager.h"
//#include "ClientPacketHandler.h"
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
#include "Economy.h"
#include "BaseCampTable.h"
#include "Telemetry.h"
#include "Bag.h"
#include "DBJobQueue.h"
#include "ObjectUtils.h"
#include "InstanceManager.h"
#include "PartyManager.h"

enum
{
	WORKER_TICK = 64
};

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
}

void DoWorkerJob(ServerServiceRef& service)
{
	while (true)
	{
		LEndTickCount = ::GetTickCount64() + WORKER_TICK;

		// 네트워크 입출력 처리 -> 인게임 로직까지 (패킷 핸들러에 의해)
		service->GetIocpCore()->Dispatch(10);

		// 예약된 일감 처리
		ThreadManager::DistributeReservedJobs();

		// 글로벌 큐
		ThreadManager::DoGlobalQueueWork();
	}
}

int main()
{
	::setvbuf(stdout, nullptr, _IONBF, 0);

	ServerPacketHandler::Init();

	// G2 · SV-5 - 아이템 이벤트가 만드는 송신량을 따로 본다. 가방 · 상자의 생성 · 소멸은 가상 칸으로 센다
	GStats.SetPacketNames(PacketName);
	GStats.SetItemPackets({ STAT_ID_BAG_SPAWN, STAT_ID_BAG_DESPAWN, PKT_S_BAG_CONTENTS,
		PKT_S_ITEM_RESULT, PKT_S_INVENTORY_SYNC, PKT_S_EQUIP_SYNC });

	{
		const string dataDir = ItemTable::DefaultDataDir();
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

		// FoW 시야 규칙 (design/gdd/fog-of-war.md, 2026-10-11) - 벽 다음. 시선 판정이 벽을 읽는다
		if (Vision::Load(dataDir, OUT error) == false)
		{
			LOG_LINE << "[BOOT] vision rules invalid - " << error << " (dir=" << dataDir << ")\n";
			return -1;
		}

		{
			char* raw = nullptr;
			size_t length = 0;
			if (_dupenv_s(&raw, &length, "FBTK_FOW") == 0 && raw != nullptr)
			{
				Vision::SetEnabled(raw[0] != '0');
				::free(raw);
			}

			raw = nullptr;
			if (_dupenv_s(&raw, &length, "FBTK_FOW_LOG") == 0 && raw != nullptr)
			{
				extern atomic<uint64> GFowLogViewer;
				GFowLogViewer.store(::strtoull(raw, nullptr, 10));
				::free(raw);
				LOG_LINE << "[BOOT] FBTK_FOW_LOG=" << GFowLogViewer.load() << " - that character's FoW spawns / despawns are logged (TEST ONLY)\n";
			}
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
			char* raw = nullptr;
			size_t length = 0;
			if (_dupenv_s(&raw, &length, "FBTK_ROUTE_SEED") == 0 && raw != nullptr)
			{
				routeSeed = ::strtoull(raw, nullptr, 10);
				::free(raw);
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

			char* raw = nullptr;
			size_t length = 0;
			if (_dupenv_s(&raw, &length, "FBTK_DEBUG_GOLD") == 0 && raw != nullptr)
			{
				GDebugGold.store(raw[0] == '1');
				::free(raw);
			}
			if (GDebugGold.load())
				LOG_LINE << "[BOOT] FBTK_DEBUG_GOLD=1 - /gold N adds run gold (TEST ONLY)\n";

			if (_dupenv_s(&raw, &length, "FBTK_DEBUG_COMMANDS") == 0 && raw != nullptr)
			{
				GDebugCommands.store(raw[0] == '1');
				::free(raw);
			}

			// 테스트 모드 (design/quick-specs/test-mode-2026-10-11.md) - /t 명령: 장비 · 스킬 바꿔 끼우기 · 더미 적 · 무적. 디버그 명령도 켠다
			if (_dupenv_s(&raw, &length, "FBTK_TEST_MODE") == 0 && raw != nullptr)
			{
				GTestMode.store(raw[0] == '1');
				::free(raw);
			}
			if (GTestMode.load())
			{
				GDebugCommands.store(true);
				LOG_LINE << "[BOOT] TEST MODE ON - FBTK_TEST_MODE=1: /t gear spawn mode clear heal cd god fow (TEST ONLY)\n";
			}

			LOG_LINE << "[BOOT] debug chat commands (/bag /unbag /die /stun /root /silence /slow) "
				<< (GDebugCommands.load() ? "ON - FBTK_DEBUG_COMMANDS=1 (TEST ONLY)" : "off") << '\n';

			// 측정 전용 - 못 열어도 서버는 돈다
			const string telemetryDir = dataDir + "\\..\\Logs\\telemetry";
			if (GTelemetry.Open(telemetryDir))
				LOG_LINE << "[TELEM] writing " << telemetryDir << " boot=" << GTelemetry.BootId() << '\n';
			else
				LOG_LINE << "[TELEM] cannot open " << telemetryDir << " - telemetry off\n";
		}

		// G3 켬/끔 비교 - 같은 데이터로 몬스터만 끈다 (FBTK_MONSTERS=0)
		bool monstersOn = true;
		{
			char* raw = nullptr;
			size_t length = 0;
			if (_dupenv_s(&raw, &length, "FBTK_MONSTERS") == 0 && raw != nullptr)
			{
				monstersOn = (raw[0] != '0');
				::free(raw);
			}
		}

		GMonstersOn.store(monstersOn);		// PVE 인스턴스도 같은 스위치를 따른다 (ADR-0014)
		if (monstersOn)
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

	string dbPassword;
	{
		char* raw = nullptr;
		size_t length = 0;

		if (_dupenv_s(&raw, &length, "FBTK_DB_PASSWORD") == 0 && raw != nullptr)
		{
			dbPassword = raw;
			::free(raw);
		}
	}

	if (dbPassword.empty())
	{
		LOG_LINE << "[DB] FBTK_DB_PASSWORD not set" << '\n';
		return -1;
	}

	{
		char* raw = nullptr;
		size_t length = 0;

		if (_dupenv_s(&raw, &length, "FBTK_FAIL_DEATH_COMMIT") == 0 && raw != nullptr)
		{
			GFailDeathCommit.store(raw[0] == '1');
			::free(raw);
		}

		if (GFailDeathCommit.load())
			LOG_LINE << "[BOOT] FBTK_FAIL_DEATH_COMMIT=1 - every death commit will fail (TEST ONLY)\n";
	}

	{
		char* raw = nullptr;
		size_t length = 0;

		if (_dupenv_s(&raw, &length, "FBTK_DEATH_COMMIT_DELAY_MS") == 0 && raw != nullptr)
		{
			GDeathCommitDelayMs.store(static_cast<uint32>(::strtoul(raw, nullptr, 10)));
			::free(raw);
		}

		if (GDeathCommitDelayMs.load() > 0)
			LOG_LINE << "[BOOT] FBTK_DEATH_COMMIT_DELAY_MS=" << GDeathCommitDelayMs.load() << " (TEST ONLY)\n";
	}

	{
		char* raw = nullptr;
		size_t length = 0;

		if (_dupenv_s(&raw, &length, "FBTK_BASECAMP_DELAY_MS") == 0 && raw != nullptr)
		{
			GBaseCampDelayMs.store(static_cast<uint32>(::strtoul(raw, nullptr, 10)));
			::free(raw);
		}

		if (GBaseCampDelayMs.load() > 0)
			LOG_LINE << "[BOOT] FBTK_BASECAMP_DELAY_MS=" << GBaseCampDelayMs.load() << " (TEST ONLY)\n";
	}

	{
		char* raw = nullptr;
		size_t length = 0;

		if (_dupenv_s(&raw, &length, "FBTK_FAIL_ESCAPE_COMMIT") == 0 && raw != nullptr)
		{
			GFailEscapeCommits.store(static_cast<int32>(::strtoull(raw, nullptr, 10)));
			::free(raw);
		}

		if (GFailEscapeCommits.load() > 0)
			LOG_LINE << "[BOOT] FBTK_FAIL_ESCAPE_COMMIT=" << GFailEscapeCommits.load() << " - first N escape commits fail (TEST ONLY)\n";
	}

	{
		char* raw = nullptr;
		size_t length = 0;

		if (_dupenv_s(&raw, &length, "FBTK_FAKE_ACK_LOSS") == 0 && raw != nullptr)
		{
			GFakeCommitAckLoss.store(static_cast<int32>(::strtol(raw, nullptr, 10)));
			::free(raw);
		}

		if (GFakeCommitAckLoss.load() > 0)
			LOG_LINE << "[BOOT] FBTK_FAKE_ACK_LOSS=" << GFakeCommitAckLoss.load() << " - first N run-end commits report a lost ack after committing (TEST ONLY)\n";
	}

	{
		char* raw = nullptr;
		size_t length = 0;

		if (_dupenv_s(&raw, &length, "FBTK_FAKE_LOGIN_FAIL") == 0 && raw != nullptr)
		{
			GFakeLoginFail.store(static_cast<int32>(::strtol(raw, nullptr, 10)));
			::free(raw);
		}

		if (GFakeLoginFail.load() > 0)
			LOG_LINE << "[BOOT] FBTK_FAKE_LOGIN_FAIL=" << GFakeLoginFail.load() << " - first N logins fail their character query (TEST ONLY)\n";
	}

	{
		char* raw = nullptr;
		size_t length = 0;

		if (_dupenv_s(&raw, &length, "FBTK_LOGIN_DELAY_MS") == 0 && raw != nullptr)
		{
			GLoginDelayMs.store(static_cast<uint32>(::strtoul(raw, nullptr, 10)));
			::free(raw);
		}

		if (GLoginDelayMs.load() > 0)
			LOG_LINE << "[BOOT] FBTK_LOGIN_DELAY_MS=" << GLoginDelayMs.load() << " - every login waits this long in the DB queue (TEST ONLY)\n";
	}

	{
		char* raw = nullptr;
		size_t length = 0;

		if (_dupenv_s(&raw, &length, "FBTK_LOG_STALL_MS") == 0 && raw != nullptr)
		{
			const uint32 stallMs = static_cast<uint32>(::strtoul(raw, nullptr, 10));
			::free(raw);

			if (stallMs > 0)
			{
				constexpr uint32 STALL_PERIOD_MS = 10'000;
				GetLogger().SetTestStall(stallMs, STALL_PERIOD_MS);
				LOG_LINE << "[BOOT] FBTK_LOG_STALL_MS=" << stallMs << " - log writer stalls every " << STALL_PERIOD_MS << "ms (TEST ONLY)\n";
			}
		}
	}

	{
		char* raw = nullptr;
		size_t length = 0;

		if (_dupenv_s(&raw, &length, "FBTK_BOT_REGEAR") == 0 && raw != nullptr)
		{
			GBotRegear.store(raw[0] == '1');
			::free(raw);
		}

		if (GBotRegear.load())
			LOG_LINE << "[BOOT] FBTK_BOT_REGEAR=1 - bare bot_ characters get the basic 3 pieces on enter (LOAD TEST ONLY)\n";
	}

	{
		char* raw = nullptr;
		size_t length = 0;

		if (_dupenv_s(&raw, &length, "FBTK_ENTRY_FROM_DB") == 0 && raw != nullptr)
		{
			GEntryFromDb.store(raw[0] == '1');
			::free(raw);
		}

		if (GEntryFromDb.load())
			LOG_LINE << "[BOOT] FBTK_ENTRY_FROM_DB=1 - enter at the saved DB position, not an entry point (LOAD TEST / FIXTURE ONLY)\n";
	}

	{
		char* raw = nullptr;
		size_t length = 0;

		if (_dupenv_s(&raw, &length, "FBTK_DISCONNECT_GRACE_MS") == 0 && raw != nullptr)
		{
			GDisconnectGraceUs.store(::strtoull(raw, nullptr, 10) * 1000);
			::free(raw);
		}

		LOG_LINE << "[BOOT] disconnect grace=" << GDisconnectGraceUs.load() / 1000 << "ms\n";
	}

	{
		char* raw = nullptr;
		size_t length = 0;

		if (_dupenv_s(&raw, &length, "FBTK_WALLS") == 0 && raw != nullptr)
		{
			GWallCheck.store(raw[0] != '0');
			::free(raw);
		}

		if (GWallCheck.load() == false)
			LOG_LINE << "[BOOT] FBTK_WALLS=0 - move validation skips the wall check (SOAK COMPARISON / EMERGENCY ONLY)\n";
	}

	constexpr int32 DB_THREAD_COUNT = 4;

	if (GDBPool.Connect(DB_THREAD_COUNT, "127.0.0.1", 3306, "root", dbPassword.c_str(), "forbetheking") == false)
	{
		LOG_LINE << "[DB] pool connect failed" << '\n';
		return -1;
	}

	// 기다리는 런 끝 저장의 기록 (CommitJournal.h, 2026-10-08) - 그 사이 서버가 꺼지면 아래 재처리가 접속을 받기 전에 끝낸다
	{
		char* raw = nullptr;
		size_t length = 0;
		if (_dupenv_s(&raw, &length, "FBTK_JOURNAL_DIR") == 0 && raw != nullptr)
		{
			GCommitJournalDir = raw;
			::free(raw);
		}
		else
		{
			GCommitJournalDir = ItemTable::DefaultDataDir() + "\\..\\Logs\\pending-commits";
		}
		LOG_LINE << "[BOOT] commit journal dir=" << GCommitJournalDir << '\n';
	}

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

	if (GDBQueue.Init(DB_THREAD_COUNT, &GDBPool) == false)
	{
		LOG_LINE << "[DB] queue init failed" << '\n';
		return -1;
	}

	LOG_LINE << "[DB] connected — " << DB_THREAD_COUNT << " connections" << '\n';

	ServerServiceRef service = make_shared<ServerService>(
		NetAddress(L"127.0.0.1", 7777),
		make_shared<IocpCore>(),
		[=]() { return make_shared<GameSession>(); }, // TODO : SessionManager 등
		100);

	ASSERT_CRASH(service->Start());
	LOG_LINE << "[BOOT] listen port=7777 - accepting clients\n";		// 검증 체크리스트 E1 이 찾는 줄 (2026-10-12)

	for (int32 i = 0; i < 5; i++)
	{
		GThreadManager->Launch([&service]()
			{
				DoWorkerJob(service);
			});
	}

	// Main Thread
	//DoWorkerJob(service);

	for (int32 i = 0; i < FLOOR_COUNT; i++)
	{
		GRooms[i]->DoAsync(&Room::UpdateTick);
		GRooms[i]->DoAsync(&Room::FlushMoves);
		GRooms[i]->DoAsync(&Room::FlushCombat);
		GRooms[i]->DoAsync(&Room::FlushSpawns);
	}

	while (true)
	{
		this_thread::sleep_for(10s);
		GStats.Dump(service->GetCurrentSessionCount());
		GMonsterStats.Dump(10);

		// PVE (ADR-0014) - 살아 있는 인스턴스 수 · 지금까지 만든 수 · 파티 초대 만료 (30 s, 10 s 단위로 알린다)
		GParties.Tick(Utils::NowMicroseconds());
		if (const uint64 created = GInstances.CreatedCount(); created > 0)
			LOG_LINE << "[INST] live=" << GInstances.LiveCount() << " created=" << created << '\n';
	}


	GThreadManager->Join();
}
