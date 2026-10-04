#include "pch.h"
#include "JobQueue.h"
#include "GlobalQueue.h"
#include "ServerStats.h"
#include "Utils.h"
#include <intrin.h>

/*--------------
	JobQueue
---------------*/

void JobQueue::Push(JobRef job, bool pushOnly)
{
	const int32 prevCount = _jobCount.fetch_add(1);
	_jobs.Push(job); // WRITE_LOCK

	// 첫번째 Job을 넣은 쓰레드가 실행까지 담당
	if (prevCount == 0)
	{
		// 이미 실행중인 JobQueue가 없으면 실행
		if (LCurrentJobQueue == nullptr && pushOnly == false)
		{
			Execute();
		}
		else
		{
			// 여유 있는 다른 쓰레드가 실행하도록 GlobalQueue에 넘긴다
			GGlobalQueue->Push(shared_from_this());
		}
	}
}

namespace
{
	// SV-1 꼬리(max) 추적용 - 이보다 긴 Flush 는 가장 느린 잡의 이름과 함께 한 줄 남긴다 (G2 · 2026-10-04)
	constexpr uint64 SLOW_FLUSH_US = 20'000;

	// 스레드 사이클(QueryThreadCycleTime)을 µs 로 바꾸는 비율. 부팅 때 QPC 와 TSC 를 20ms 맞대어 잰다.
	// 느린 Flush 가 "일을 했다"(cpu ≈ wall)인지 "기다렸다"(cpu ≪ wall - 락 · 입출력 · 선점)인지 가른다
	const double GCyclesPerUs = []()
	{
		LARGE_INTEGER freq, begin, now;
		::QueryPerformanceFrequency(&freq);
		::QueryPerformanceCounter(&begin);
		const uint64 cycleBegin = __rdtsc();

		double elapsedUs = 0.0;
		do
		{
			::QueryPerformanceCounter(&now);
			elapsedUs = (now.QuadPart - begin.QuadPart) * 1'000'000.0 / freq.QuadPart;
		} while (elapsedUs < 20'000.0);

		return (__rdtsc() - cycleBegin) / elapsedUs;
	}();

	uint64 ThreadCycles()
	{
		ULONG64 cycles = 0;
		::QueryThreadCycleTime(::GetCurrentThread(), &cycles);
		return cycles;
	}
}

// 1) 일감이 너~무 몰리면?
void JobQueue::Execute()
{
	LCurrentJobQueue = this;

	while (true)
	{
		vector<JobRef> jobs;
		_jobs.PopAll(OUT jobs);

		const int32 jobCount = static_cast<int32>(jobs.size());

		const uint64 flushStartUs = Utils::NowMicroseconds();
		const uint64 flushStartCycles = ThreadCycles();

		uint64 slowestUs = 0;
		const char* slowestTag = nullptr;

		for (int32 i = 0; i < jobCount; i++)
		{
			LJobTag = nullptr;
			const uint64 jobStartUs = Utils::NowMicroseconds();

			jobs[i]->Execute();

			const uint64 jobUs = Utils::NowMicroseconds() - jobStartUs;
			if (jobUs > slowestUs)
			{
				slowestUs = jobUs;
				slowestTag = LJobTag;
			}
		}
		LJobTag = nullptr;

		if (jobCount > 0)
		{
			const uint64 flushUs = Utils::NowMicroseconds() - flushStartUs;
			GStats.RecordFlush(flushUs);

			// 잡 하나가 느린지(slowest ≈ flush), 잡이 몰렸는지(jobs 가 크고 slowest 는 작음)를 가른다
			if (flushUs >= SLOW_FLUSH_US)
			{
				const uint64 cpuUs = static_cast<uint64>((ThreadCycles() - flushStartCycles) / GCyclesPerUs);

				// 표준 오류로 낸다 - 표준 출력(LOG_LINE)의 잠금 · 파일 쓰기가 용의자라, 그것과 다른 길로 남겨야
				// 표준 출력을 버린 실험에서도 이 줄이 살아남는다
				std::osyncstream(std::cerr) << "[SLOW FLUSH] us=" << flushUs << " cpu=" << cpuUs << "us jobs=" << jobCount
					<< " slowest=" << slowestUs << "us tag=" << (slowestTag != nullptr ? slowestTag : "untagged") << '\n';
			}
		}

		// 남은 일감이 0개라면 종료
		if (_jobCount.fetch_sub(jobCount) == jobCount)
		{
			LCurrentJobQueue = nullptr;
			return;
		}

		const uint64 now = ::GetTickCount64();
		if (now >= LEndTickCount)
		{
			LCurrentJobQueue = nullptr;
			// 여유 있는 다른 쓰레드가 실행하도록 GlobalQueue에 넘긴다
			GGlobalQueue->Push(shared_from_this());
			break;
		}			
	}
}
