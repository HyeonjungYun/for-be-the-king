#include "pch.h"
#include "JobTimer.h"
#include "JobQueue.h"
#include "ServerStats.h"
#include "Utils.h"

/*--------------
	JobTimer
---------------*/

void JobTimer::Reserve(uint64 tickAfter, weak_ptr<JobQueue> owner, JobRef job)
{
	if (_stopped.load())
		return;

	// 시계는 Utils::NowMilliseconds (steady_clock) - Distribute 의 now 도 같은 시계여야 한다
	const uint64 executeTick = Utils::NowMilliseconds() + tickAfter;
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

			// 예약 시각보다 얼마나 늦게 나눴나 - 33 ms Flush 가 실제로 얼마나 흔들리는지의 1차 지표 (ServerStats "timer late")
			GStats.RecordTimerLate(now - timerItem.executeTick);

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

uint32 JobTimer::MsUntilNext(uint64 now, uint32 maxWaitMs)
{
	WRITE_LOCK;
	if (_items.empty())
		return maxWaitMs;

	const uint64 due = _items.top().executeTick;
	if (due <= now)
		return 0;

	return static_cast<uint32>((std::min<uint64>)(due - now, maxWaitMs));
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
