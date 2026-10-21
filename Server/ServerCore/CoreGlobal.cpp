#include "pch.h"
#include "CoreGlobal.h"
#include "ThreadManager.h"
#include "SocketUtils.h"
#include "SendBuffer.h"
#include "GlobalQueue.h"
#include "JobTimer.h"
#include <timeapi.h>

#pragma comment(lib, "winmm.lib")

ThreadManager* GThreadManager = nullptr;
GlobalQueue* GGlobalQueue = nullptr;
JobTimer* GJobTimer = nullptr;

class CoreGlobal
{
public:
	CoreGlobal()
	{
		// 시스템 타이머를 1 ms 로 - GetQueuedCompletionStatus(10 ms) · sleep 이 15.6 ms 단위로 반올림되지 않게.
		// 잡 타이머 자체는 steady_clock 이라 분해능과 무관하지만, 워커가 10 ms 만에 깨어나야 33 ms 예약이 제때 분배된다 (2026-10-21)
		::timeBeginPeriod(1);

		GThreadManager = new ThreadManager();
		GGlobalQueue = new GlobalQueue();
		GJobTimer = new JobTimer();
		SocketUtils::Init();
	}

	~CoreGlobal()
	{
		delete GThreadManager;
		delete GGlobalQueue;
		delete GJobTimer;
		SocketUtils::Clear();

		::timeEndPeriod(1);
	}
} GCoreGlobal;
