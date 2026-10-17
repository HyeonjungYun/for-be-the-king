#include "pch.h"
#include "JobTimer.h"
#include "JobQueue.h"

/*--------------
	JobTimer
---------------*/

void JobTimer::Reserve(uint64 tickAfter, weak_ptr<JobQueue> owner, JobRef job)
{
	const uint64 executeTick = ::GetTickCount64() + tickAfter;
	JobData* jobData = new JobData(std::move(owner), std::move(job));

	WRITE_LOCK;

	_items.push(TimerItem{ executeTick, jobData });
}

void JobTimer::Distribute(uint64 now)
{
	// 한 번에 1 쓰레드만 통과
	if (_distributing.exchange(true) == true)
		return;

	vector<TimerItem> items;

	{
		WRITE_LOCK;

		while (_items.empty() == false)
		{
			const TimerItem& timerItem = _items.top();
			if (now < timerItem.executeTick)
				break;

			items.push_back(timerItem);
			_items.pop();
		}
	}

	// pushOnly - 이 스레드가 방 잡을 줄줄이 직접 돌지 않는다 (TD-008, 2026-10-18). 예전엔 첫 잡을 넣은 이 스레드가
	// 그 방의 Flush 를 그 자리에서 돌고, 다음 방, 그다음 방… 이 끝나야 다음 타이머를 나눴다. 이제 GlobalQueue 에 넘겨 노는 워커가 나눠 든다
	for (TimerItem& item : items)
	{
		if (JobQueueRef owner = item.jobData->owner.lock())
			owner->Push(std::move(item.jobData->job), /*pushOnly*/ true);

		delete item.jobData;		
	}

	// 끝났으면 풀어준다
	_distributing.store(false);
}

void JobTimer::Clear()
{
	WRITE_LOCK;

	while (_items.empty() == false)
	{
		const TimerItem& timerItem = _items.top();
		delete timerItem.jobData;
		_items.pop();
	}
}
