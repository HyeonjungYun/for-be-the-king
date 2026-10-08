#pragma once

#include "DBConnectionPool.h"
#include <condition_variable>
#include <functional>

/*----------------
	DBJobQueue
-----------------*/

class DBJobQueue
{
public:
			~DBJobQueue();

	bool	Init(int32 threadCount, DBConnectionPool* pool);
	void	Shutdown();

	void	Push(function<void(DBConnection*)> job);

	// delayMs 뒤에 큐에 넣는다 - 재시도 대기 동안 DB 스레드를 잡고 자지 않게 (ADR-0010 R-1, 2026-10-08).
	// 예전 재시도는 작업 안에서 sleep 했다 - 막힌 저장 4건이면 워커 4개가 모두 서서 로그인 · 저장이 멈췄다.
	// 종료(Shutdown) 때 남은 지연 작업은 기다리지 않고 바로 한 번씩 돈다
	void	PushAfter(uint32 delayMs, function<void(DBConnection*)> job);

	int32	GetPendingCount();
	int32	GetDelayedCount();

private:
	void	WorkerLoop();

	// _mutex 를 잡은 채 부른다 - 때가 된 지연 작업을 _jobs 로 옮긴다
	void	PromoteDueLocked(chrono::steady_clock::time_point now);

private:
	struct DelayedJob
	{
		chrono::steady_clock::time_point	due;
		uint64								seq = 0;		// 같은 시각이면 넣은 순서
		function<void(DBConnection*)>		job;

		bool operator>(const DelayedJob& other) const { return due != other.due ? due > other.due : seq > other.seq; }
	};

	DBConnectionPool*						_pool = nullptr;

	mutex									_mutex;
	condition_variable						_cv;
	queue<function<void(DBConnection*)>>	_jobs;
	priority_queue<DelayedJob, vector<DelayedJob>, greater<DelayedJob>>	_delayed;
	uint64									_delayedSeq = 0;

	vector<thread>							_threads;
	bool									_running = false;
};

extern DBConnectionPool GDBPool;
extern DBJobQueue GDBQueue;
