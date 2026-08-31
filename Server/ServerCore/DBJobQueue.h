#pragma once

#include "DBConnectionPool.h"
#include <condition_variable>

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

	int32	GetPendingCount();

private:
	void	WorkerLoop();

private:
	DBConnectionPool*						_pool = nullptr;

	mutex									_mutex;
	condition_variable						_cv;
	queue<function<void(DBConnection*)>>	_jobs;

	vector<thread>							_threads;
	bool									_running = false;
};

extern DBConnectionPool GDBPool;
extern DBJobQueue GDBQueue;
