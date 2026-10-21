#include "pch.h"
#include "DBJobQueue.h"
#include "DBError.h"
#include "ThreadManager.h"

DBConnectionPool GDBPool;
DBJobQueue GDBQueue;

DBJobQueue::~DBJobQueue()
{
	Shutdown();
}

bool DBJobQueue::Init(int32 threadCount, DBConnectionPool* pool)
{
	if (pool == nullptr || threadCount <= 0)
		return false;

	_pool = pool;
	_running = true;

	for (int32 i = 0; i < threadCount; i++)
		_threads.emplace_back([this]() {WorkerLoop(); });

	return true;
}

void DBJobQueue::Shutdown()
{
	{
		lock_guard<mutex> guard(_mutex);
		if (_running == false)
			return;

		_running = false;

		// 지연 작업은 기다리지 않는다 - 종료 신호가 와도 남은 작업은 처리한다는 규칙 그대로, 지금 한 번씩
		while (_delayed.empty() == false)
		{
			_jobs.push(move(const_cast<DelayedJob&>(_delayed.top()).job));
			_delayed.pop();
		}
	}

	_cv.notify_all();

	for (thread& t : _threads)
	{
		if (t.joinable())
			t.join();
	}

	_threads.clear();
}

void DBJobQueue::Push(function<void(DBConnection*)> job)
{
	{
		lock_guard<mutex> guard(_mutex);
		if (_running == false)
			return;

		_jobs.push(move(job));
	}

	_cv.notify_one();
}

void DBJobQueue::PushAfter(uint32 delayMs, function<void(DBConnection*)> job)
{
	if (delayMs == 0)
	{
		Push(move(job));
		return;
	}

	{
		lock_guard<mutex> guard(_mutex);
		if (_running == false)
			return;

		DelayedJob delayed;
		delayed.due = chrono::steady_clock::now() + chrono::milliseconds(delayMs);
		delayed.seq = _delayedSeq++;
		delayed.job = move(job);
		_delayed.push(move(delayed));
	}

	// 자고 있는 워커가 새 마감 시각으로 다시 잠들게 깨운다
	_cv.notify_one();
}

int32 DBJobQueue::GetPendingCount()
{
	lock_guard<mutex> guard(_mutex);
	return static_cast<int32>(_jobs.size());
}

int32 DBJobQueue::GetDelayedCount()
{
	lock_guard<mutex> guard(_mutex);
	return static_cast<int32>(_delayed.size());
}

void DBJobQueue::PromoteDueLocked(chrono::steady_clock::time_point now)
{
	while (_delayed.empty() == false && _delayed.top().due <= now)
	{
		_jobs.push(move(const_cast<DelayedJob&>(_delayed.top()).job));
		_delayed.pop();
	}
}

void DBJobQueue::WorkerLoop()
{
	// 스레드마다 커넥션 하나를 점유. 작업마다 빌리고 돌려주면 락 경합이 생기지만
	// 스레드 수와 커넥션 수가 같으면 그럴 이유가 없다.

	ThreadManager::InitTLS();		// 로그 머리의 스레드 번호 (LThreadId) - 예전엔 DB 워커가 전부 0 이었다 (2026-10-21)

	DBConnection* connection = _pool->Pop();
	if (connection == nullptr)
	{
		LOG_LINE << "[DB] worker got no connection — pool exhausted" << '\n';
		return;
	}

	while (true)
	{
		function<void(DBConnection*)> job;

		{
			unique_lock<mutex> guard(_mutex);

			// 조건 변수로 잔다. 스핀 루프면 DB가 한가할 때도 코어를 하나 태운다.
			// 지연 작업이 있으면 가장 이른 마감까지만 잔다. 깰 때마다 때가 된 것을 먼저 옮긴다 - 큐가 계속 차 있어도 굶지 않게
			while (true)
			{
				PromoteDueLocked(chrono::steady_clock::now());

				if (_running == false || _jobs.empty() == false)
					break;

				if (_delayed.empty())
					_cv.wait(guard);
				else
					_cv.wait_until(guard, _delayed.top().due);
			}

			// 종료 신호가 와도 남은 작업은 처리한다.
			if (_jobs.empty())
			{
				if (_running == false)
					break;

				continue;
			}

			job = move(_jobs.front());
			_jobs.pop();
		}

		job(connection);

		// 끊긴 커넥션으로는 다음 작업도 전부 실패한다 - 작업이 연결 끊김으로 끝났으면 여기서 다시 붙인다.
		// 예전엔 MySQL 이 한 번 재시작하면 이 워커의 모든 작업이 서버가 꺼질 때까지 실패했다 (ADR-0010 R-1)
		if (DBError::IsConnectionLost(connection->GetLastErrorNo()))
			connection->Reconnect();
	}

	_pool->Push(connection);
}
