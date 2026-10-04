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
#include "DBJobQueue.h"
#include "ObjectUtils.h"

enum
{
	WORKER_TICK = 64
};

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

	{
		const string dataDir = ItemTable::DefaultDataDir();
		string error;

		if (ItemTable::Load(dataDir, OUT error) == false)
		{
			LOG_LINE << "[BOOT] item data invalid - " << error << " (dir=" << dataDir << ")\n";
			return -1;
		}

		LOG_LINE << "[BOOT] item data loaded - types=" << ItemTable::All().size() << " pools=" << ItemTable::PoolCount() << '\n';
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
