#include "pch.h"
#include <thread>
#include "LockQueue.h"
#include "JobQueue.h"
#include "GlobalQueue.h"
#include "CoreTLS.h"

namespace
{
	constexpr int THREAD_COUNT = 8;
	constexpr int OPS_PER_THREAD = 1000;

	// 워커 스레드가 아니므로 타임슬라이스를 넉넉히 잡아 준다.
	// 이걸 안 하면 JobQueue::Execute 가 LEndTickCount 를 넘겼다고 판단해
	// 곧바로 GlobalQueue 로 넘겨 버린다.
	void GiveGenerousTimeSlice() { LEndTickCount = ::GetTickCount64() + 60000; }
}

/*---------------
	스트레스 1 — LockQueue
---------------*/

TEST(Stress, LockQueue_동시_Push와_PopAll이_원소를_잃지_않는다)
{
	LockQueue<shared_ptr<int>> q;
	atomic<int> popped{ 0 };
	atomic<bool> producersDone{ false };

	vector<std::thread> producers;
	for (int t = 0; t < THREAD_COUNT; t++)
	{
		producers.emplace_back([&q]()
			{
				for (int i = 0; i < OPS_PER_THREAD; i++)
					q.Push(make_shared<int>(i + 1));   // 0 은 종료 신호라 1부터
			});
	}

	std::thread consumer([&]()
		{
			while (true)
			{
				vector<shared_ptr<int>> items;
				q.PopAll(OUT items);
				popped.fetch_add(static_cast<int>(items.size()));

				if (producersDone.load() && items.empty())
					break;
			}
		});

	for (auto& t : producers)
		t.join();

	producersDone.store(true);
	consumer.join();

	EXPECT_EQ(popped.load(), THREAD_COUNT * OPS_PER_THREAD);
}

/*---------------
	스트레스 2 — JobQueue
---------------*/

class CounterJobQueue : public JobQueue
{
public:
	void Increase() { counter.fetch_add(1); }
	atomic<int> counter{ 0 };
};

TEST(Stress, JobQueue_동시_DoAsync가_일감을_잃지_않는다)
{
	auto q = make_shared<CounterJobQueue>();

	vector<std::thread> threads;
	for (int t = 0; t < THREAD_COUNT; t++)
	{
		threads.emplace_back([&q]()
			{
				GiveGenerousTimeSlice();

				for (int i = 0; i < OPS_PER_THREAD; i++)
					q->DoAsync(&CounterJobQueue::Increase);
			});
	}

	for (auto& t : threads)
		t.join();

	// 타임슬라이스에 걸려 GlobalQueue 로 넘어간 잔여 일감을 마저 처리한다.
	GiveGenerousTimeSlice();
	while (JobQueueRef pending = GGlobalQueue->Pop())
	{
		GiveGenerousTimeSlice();
		pending->Execute();
	}

	EXPECT_EQ(q->counter.load(), THREAD_COUNT * OPS_PER_THREAD);
}

/*---------------
	스트레스 3 — GlobalQueue
---------------*/

TEST(Stress, GlobalQueue_동시_Push와_Pop이_큐를_깨뜨리지_않는다)
{
	GlobalQueue queue;
	atomic<int> popped{ 0 };
	atomic<bool> producersDone{ false };

	vector<std::thread> producers;
	for (int t = 0; t < THREAD_COUNT; t++)
	{
		producers.emplace_back([&queue]()
			{
				for (int i = 0; i < OPS_PER_THREAD; i++)
					queue.Push(make_shared<CounterJobQueue>());
			});
	}

	vector<std::thread> consumers;
	for (int t = 0; t < 2; t++)
	{
		consumers.emplace_back([&]()
			{
				while (true)
				{
					if (JobQueueRef q = queue.Pop())
					{
						popped.fetch_add(1);
						continue;
					}

					if (producersDone.load())
						break;

					std::this_thread::yield();
				}
			});
	}

	for (auto& t : producers)
		t.join();

	producersDone.store(true);

	for (auto& t : consumers)
		t.join();

	EXPECT_EQ(popped.load(), THREAD_COUNT * OPS_PER_THREAD);
}