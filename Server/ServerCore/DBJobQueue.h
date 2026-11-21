#pragma once

#include "DBConnectionPool.h"
#include <condition_variable>
#include <functional>

/*----------------
	DBJobQueue
-----------------*/

// DB 작업 우선순위 (2026-11-22 동접 1만 - docs/server-scaling-10k.md § 5-1). 워커는 높은 줄부터 꺼낸다.
//   High   - 런 끝 저장(사망 · 탈출 · 만료) 커밋과 그 재시도. 늦으면 아이템이 붕 뜬다
//   Normal - 로그인 · 입장 적재 · 베이스캠프 조작 (기본)
//   Low    - 1 분 주기 위치 저장 · 만료 가방 지우기 · 안티치트 기록. 몇 초 늦어도 된다
// 접속 폭주 때 로그인 수천 건 뒤에 사망 저장이 줄 서던 것을 막는다. Low 가 굶지 않게 High · Normal 8 번마다 Low 를 한 번 꺼낸다
enum class DBPriority : uint8 { High = 0, Normal = 1, Low = 2 };

class DBJobQueue
{
public:
			~DBJobQueue();

	bool	Init(int32 threadCount, DBConnectionPool* pool);
	void	Shutdown();

	void	Push(function<void(DBConnection*)> job, DBPriority priority = DBPriority::Normal);

	// delayMs 뒤에 큐에 넣는다 - 재시도 대기 동안 DB 스레드를 잡고 자지 않게 (ADR-0010 R-1, 2026-10-08).
	// 예전 재시도는 작업 안에서 sleep 했다 - 막힌 저장 4건이면 워커 4개가 모두 서서 로그인 · 저장이 멈췄다.
	// 종료(Shutdown) 때 남은 지연 작업은 기다리지 않고 바로 한 번씩 돈다
	void	PushAfter(uint32 delayMs, function<void(DBConnection*)> job, DBPriority priority = DBPriority::Normal);

	// 줄마다 기다리는 수 (통계)
	int32	GetPendingCount(DBPriority priority);

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
		DBPriority							priority = DBPriority::Normal;

		bool operator>(const DelayedJob& other) const { return due != other.due ? due > other.due : seq > other.seq; }
	};

	DBConnectionPool*						_pool = nullptr;

	mutex									_mutex;
	condition_variable						_cv;
	queue<function<void(DBConnection*)>>	_jobs[3];		// DBPriority 줄
	int32									_sinceLow = 0;	// Low 를 마지막으로 꺼낸 뒤 High · Normal 을 몇 번 꺼냈나

	bool									AnyJobLocked() const { return _jobs[0].empty() == false || _jobs[1].empty() == false || _jobs[2].empty() == false; }
	function<void(DBConnection*)>			PopJobLocked();
	priority_queue<DelayedJob, vector<DelayedJob>, greater<DelayedJob>>	_delayed;
	uint64									_delayedSeq = 0;

	vector<thread>							_threads;
	bool									_running = false;
};

extern DBConnectionPool GDBPool;
extern DBJobQueue GDBQueue;
