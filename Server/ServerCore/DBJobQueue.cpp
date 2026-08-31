#include "pch.h"
#include "DBJobQueue.h"

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

int32 DBJobQueue::GetPendingCount()
{
	lock_guard<mutex> guard(_mutex);
	return static_cast<int32>(_jobs.size());
}

void DBJobQueue::WorkerLoop()
{
	// 스레드마다 커넥션 하나를 점유. 작업마다 빌리고 돌려주면 락 경합이 생기지만
	// 스레드 수와 커넥션 수가 같으면 그럴 이유가 없다.

	DBConnection* connection = _pool->Pop();
	if (connection == nullptr)
	{
		cout << "[DB] worker got no connection — pool exhausted" << endl;
		return;
	}

	while (true)
	{
		function<void(DBConnection*)> job;

		{
			unique_lock<mutex> guard(_mutex);

			// 조건 변수로 잔다. 스핀 루프면 DB가 한가할 때도 코어를 하나 태운다.
			_cv.wait(guard, [this]() {return _running == false || _jobs.empty() == false; });

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
	}

	_pool->Push(connection);
}
