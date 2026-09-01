#include "pch.h"
#include <thread>
#include "JobTimer.h"
#include "JobQueue.h"
#include "GlobalQueue.h"
#include "CoreTLS.h"

namespace
{
	/**
	 * Push 된 Job 이 실제로 실행됐는지 세는 큐.
	 *
	 * JobQueue::Push 는 다른 큐가 실행 중이 아니면 그 자리에서 Execute 한다.
	 * 따라서 Distribute 가 owner 에게 넘겼는지를 실행 횟수로 확인할 수 있다.
	 */
	class CountingQueue : public JobQueue
	{
	public:
		atomic<int32> executed = 0;
	};

	JobRef MakeCountingJob(shared_ptr<CountingQueue> queue)
	{
		return make_shared<Job>([queue]() { queue->executed.fetch_add(1); });
	}

	// StressTest.cpp 와 같은 이유다. 워커 스레드가 아니라 LEndTickCount 가 0이므로,
	// 손대지 않으면 JobQueue::Execute 가 타임슬라이스를 넘겼다고 보고
	// 남은 일감을 GlobalQueue 로 넘겨 버린다 — 테스트에서는 아무도 그것을 꺼내지 않는다.
	void GiveGenerousTimeSlice() { LEndTickCount = ::GetTickCount64() + 60000; }

	/** GlobalQueue 로 밀려난 잔여 일감을 마저 실행한다 */
	void DrainGlobalQueue()
	{
		GiveGenerousTimeSlice();

		while (JobQueueRef pending = GGlobalQueue->Pop())
		{
			GiveGenerousTimeSlice();
			pending->Execute();
		}
	}
}

TEST(JobTimer, 기한_전에는_배분하지_않는다)
{
	GiveGenerousTimeSlice();

	JobTimer timer;
	auto queue = make_shared<CountingQueue>();

	const uint64 now = ::GetTickCount64();
	timer.Reserve(10000, queue, MakeCountingJob(queue));

	timer.Distribute(now);

	EXPECT_EQ(queue->executed.load(), 0);

	timer.Clear();
}

TEST(JobTimer, 기한이_지나면_배분한다)
{
	GiveGenerousTimeSlice();

	JobTimer timer;
	auto queue = make_shared<CountingQueue>();

	timer.Reserve(0, queue, MakeCountingJob(queue));

	// Reserve 는 GetTickCount64() 기준으로 기한을 잡으므로 넉넉히 앞선 시각을 준다
	timer.Distribute(::GetTickCount64() + 1000);

	EXPECT_EQ(queue->executed.load(), 1);
}

TEST(JobTimer, 기한이_지난_것만_배분한다)
{
	GiveGenerousTimeSlice();

	JobTimer timer;
	auto queue = make_shared<CountingQueue>();

	timer.Reserve(0, queue, MakeCountingJob(queue));
	timer.Reserve(0, queue, MakeCountingJob(queue));
	timer.Reserve(100000, queue, MakeCountingJob(queue));

	timer.Distribute(::GetTickCount64() + 1000);

	// 먼 미래로 예약한 하나는 남아 있어야 한다
	EXPECT_EQ(queue->executed.load(), 2);

	timer.Clear();
}

TEST(JobTimer, 같은_배분에서_기한이_이른_것부터_실행된다)
{
	GiveGenerousTimeSlice();

	JobTimer timer;
	auto queue = make_shared<CountingQueue>();

	// priority_queue 가 executeTick 오름차순으로 나오는지 본다.
	// 늦은 것을 먼저 예약해 삽입 순서와 실행 순서를 구분한다
	vector<int32> order;

	timer.Reserve(200, queue, make_shared<Job>([&order]() { order.push_back(200); }));
	timer.Reserve(100, queue, make_shared<Job>([&order]() { order.push_back(100); }));
	timer.Reserve(300, queue, make_shared<Job>([&order]() { order.push_back(300); }));

	timer.Distribute(::GetTickCount64() + 1000);

	ASSERT_EQ(order.size(), 3u);
	EXPECT_EQ(order[0], 100);
	EXPECT_EQ(order[1], 200);
	EXPECT_EQ(order[2], 300);
}

TEST(JobTimer, owner가_사라졌으면_건너뛴다)
{
	GiveGenerousTimeSlice();

	JobTimer timer;
	atomic<int32> executed = 0;

	{
		auto queue = make_shared<CountingQueue>();
		timer.Reserve(0, queue, make_shared<Job>([&executed]() { executed.fetch_add(1); }));
	}
	// queue 가 파괴됐다. weak_ptr 은 만료되고 Job 은 실행되지 않아야 한다

	timer.Distribute(::GetTickCount64() + 1000);

	EXPECT_EQ(executed.load(), 0);
}

TEST(JobTimer, Clear하면_남은_예약이_사라진다)
{
	GiveGenerousTimeSlice();

	JobTimer timer;
	auto queue = make_shared<CountingQueue>();

	for (int32 i = 0; i < 10; i++)
		timer.Reserve(0, queue, MakeCountingJob(queue));

	timer.Clear();
	timer.Distribute(::GetTickCount64() + 1000);

	EXPECT_EQ(queue->executed.load(), 0);
}

TEST(JobTimer, 빈_타이머를_배분해도_안전하다)
{
	GiveGenerousTimeSlice();

	JobTimer timer;

	timer.Distribute(::GetTickCount64());
	timer.Clear();

	SUCCEED();
}

TEST(Stress, JobTimer_동시_Reserve와_Distribute가_일감을_잃지_않는다)
{
	// 🔴 Distribute 는 _distributing 으로 한 번에 한 스레드만 통과시킨다.
	//    통과하지 못한 스레드는 그냥 돌아가므로, 마지막 한 번이 남은 것을
	//    모두 가져가는지가 관건이다 — 예약이 큐에 남은 채 잊히면 안 된다.
	JobTimer timer;
	auto queue = make_shared<CountingQueue>();

	constexpr int32 THREAD_COUNT = 8;
	constexpr int32 PER_THREAD = 500;

	vector<std::thread> threads;

	for (int32 t = 0; t < THREAD_COUNT; t++)
	{
		threads.emplace_back([&timer, queue]()
			{
				GiveGenerousTimeSlice();

				for (int32 i = 0; i < PER_THREAD; i++)
				{
					timer.Reserve(0, queue, MakeCountingJob(queue));
					timer.Distribute(::GetTickCount64() + 1000);
				}
			});
	}

	for (auto& t : threads)
		t.join();

	// _distributing 경합에 밀려 타이머에 남은 것을 마지막에 비운다
	GiveGenerousTimeSlice();
	timer.Distribute(::GetTickCount64() + 1000);

	DrainGlobalQueue();

	EXPECT_EQ(queue->executed.load(), THREAD_COUNT * PER_THREAD);
}
