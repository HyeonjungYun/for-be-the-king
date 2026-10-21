#pragma once

struct JobData
{
	JobData(weak_ptr<JobQueue> owner, JobRef job) : owner(std::move(owner)), job(std::move(job))
	{

	}

	weak_ptr<JobQueue>	owner;
	JobRef				job;
};

struct TimerItem
{
	bool operator<(const TimerItem& other) const
	{
		return executeTick > other.executeTick;
	}

	uint64 executeTick = 0;
	JobData* jobData = nullptr;
};

/*--------------
	JobTimer
---------------*/

class JobTimer
{
public:
	void			Reserve(uint64 tickAfter, weak_ptr<JobQueue> owner, JobRef job);
	void			Distribute(uint64 now);
	void			Clear();

	// 정상 종료 (2026-10-21) - 이후의 Reserve 는 버린다. 방의 Flush 루프는 끝에서 자기를 다시 예약하므로, 이것으로 루프가 멈춘다
	void			Stop() { _stopped.store(true); }
	bool			IsStopped() const { return _stopped.load(); }

	// 가장 이른 예약까지 남은 ms (없으면 maxWaitMs). 워커가 GetQueuedCompletionStatus 의 대기 시간을 이만큼으로 줄인다 -
	// 예전엔 늘 10 ms 를 잤고, 그만큼 33 ms 예약이 늦게 분배됐다 (실측 최대 11 ms, 2026-10-21)
	uint32			MsUntilNext(uint64 now, uint32 maxWaitMs);

private:
	USE_LOCK;
	priority_queue<TimerItem>	_items;
	atomic<bool>				_distributing = false;
	atomic<bool>				_stopped = false;
};

