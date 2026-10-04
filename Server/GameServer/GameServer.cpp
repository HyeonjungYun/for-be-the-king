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
#include "Bag.h"
#include "DBJobQueue.h"
#include "ObjectUtils.h"

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
	SkillTable::Init();

	// G2 · SV-5 - 아이템 이벤트가 만드는 송신량을 따로 본다. 가방 · 상자의 생성 · 소멸은 가상 칸으로 센다
	GStats.SetPacketNames(PacketName);
	GStats.SetItemPackets({ STAT_ID_BAG_SPAWN, STAT_ID_BAG_DESPAWN, PKT_S_BAG_CONTENTS,
		PKT_S_ITEM_RESULT, PKT_S_INVENTORY_SYNC, PKT_S_EQUIP_SYNC });

	{
		const string dataDir = ItemTable::DefaultDataDir();
		string error;

		if (ItemTable::Load(dataDir, OUT error) == false)
		{
			LOG_LINE << "[BOOT] item data invalid - " << error << " (dir=" << dataDir << ")\n";
			return -1;
		}

		LOG_LINE << "[BOOT] item data loaded - types=" << ItemTable::All().size() << " pools=" << ItemTable::PoolCount() << '\n';

		// 층 아이템 풀 - ItemTable 다음이어야 한다. 부위 · 물리/마법 행에 뽑을 종류가 있는지 ItemTable 로 검사한다.
		if (FloorItemPool::Load(dataDir, OUT error) == false)
		{
			LOG_LINE << "[BOOT] floor item pool invalid - " << error << " (dir=" << dataDir << ")\n";
			return -1;
		}

		LOG_LINE << "[BOOT] floor item pool loaded - pools=" << FloorItemPool::Count() << '\n';

		// 상자 배치 - 층 풀 다음이어야 한다. 상자마다 그 층 · 등급의 풀이 있는지 검사한다.
		if (ChestSpawnTable::Load(dataDir, FLOOR_COUNT, OUT error) == false)
		{
			LOG_LINE << "[BOOT] chest spawns invalid - " << error << " (dir=" << dataDir << ")\n";
			return -1;
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

		if (_dupenv_s(&raw, &length, "FBTK_DISCONNECT_GRACE_MS") == 0 && raw != nullptr)
		{
			GDisconnectGraceUs.store(::strtoull(raw, nullptr, 10) * 1000);
			::free(raw);
		}

		LOG_LINE << "[BOOT] disconnect grace=" << GDisconnectGraceUs.load() / 1000 << "ms\n";
	}

	constexpr int32 DB_THREAD_COUNT = 4;

	if (GDBPool.Connect(DB_THREAD_COUNT, "127.0.0.1", 3306, "root", dbPassword.c_str(), "forbetheking") == false)
	{
		LOG_LINE << "[DB] pool connect failed" << '\n';
		return -1;
	}

	{
		DBConnection* conn = GDBPool.Pop();
		if (conn == nullptr)
		{
			LOG_LINE << "[BOOT] no DB connection for recovery" << '\n';
			return -1;
		}

		const bool ok = RestoreIdCounters(conn) && PurgeExpiredBags(conn) && ValidateItemRows(conn) && LoadLiveBags(conn);
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
	}


	GThreadManager->Join();
}
