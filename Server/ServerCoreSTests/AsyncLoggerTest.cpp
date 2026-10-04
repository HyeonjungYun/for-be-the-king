#include "pch.h"
#include "AsyncLogger.h"

namespace
{
	// sink 가 받은 것을 모아 둔다. 쓰기 스레드에서 불리므로 잠근다
	struct Captured
	{
		std::mutex		lock;
		std::string		out;
		std::string		err;
		int32			calls = 0;

		AsyncLogger::Sink MakeSink()
		{
			return [this](const std::string& text, bool toStderr)
			{
				std::lock_guard<std::mutex> guard(lock);
				(toStderr ? err : out) += text;
				calls++;
			};
		}
	};
}

/*---------------
	AsyncLogger — roadmap.md § 0-11 (동기 로그 출력이 Room 스레드를 세우던 문제)
---------------*/

TEST(AsyncLogger, 넣은_줄이_순서대로_나간다)
{
	Captured captured;
	AsyncLogger logger(captured.MakeSink());

	logger.Push("a\n");
	logger.Push("b\n");
	logger.Push("c\n");
	logger.Flush();

	EXPECT_EQ(captured.out, "a\nb\nc\n");
	EXPECT_TRUE(captured.err.empty());
}

TEST(AsyncLogger, 표준_오류_줄은_따로_나간다)
{
	Captured captured;
	AsyncLogger logger(captured.MakeSink());

	logger.Push("out\n");
	logger.Push("[SLOW FLUSH] x\n", true);
	logger.Flush();

	EXPECT_EQ(captured.out, "out\n");
	EXPECT_EQ(captured.err, "[SLOW FLUSH] x\n");
}

TEST(AsyncLogger, 쓰기가_막혀도_넣는_쪽은_기다리지_않고_상한을_넘으면_버린다)
{
	// 디스크가 막힌 상황 - sink 가 gate 에서 멈춘다. 예전 동기 출력이면 넣는 쪽(Room 잡)이 여기서 같이 멈췄다
	std::mutex gate;
	gate.lock();

	std::atomic<bool> entered = false;
	std::string written;
	AsyncLogger logger([&gate, &entered, &written](const std::string& text, bool)
		{
			entered.store(true);
			std::lock_guard<std::mutex> guard(gate);
			written += text;
		}, 2);

	logger.Push("1\n");
	while (entered.load() == false)
		std::this_thread::yield();

	// 쓰기 스레드는 "1" 을 든 채 막혀 있다. 큐 상한은 2 - 넷 중 둘은 버려진다. 이 호출들은 막히지 않고 돌아온다
	logger.Push("2\n");
	logger.Push("3\n");
	logger.Push("4\n");
	logger.Push("5\n");

	EXPECT_EQ(logger.GetDroppedCount(), 2u);

	gate.unlock();
	logger.Flush();

	EXPECT_EQ(written, "1\n2\n3\n[LOG] dropped 2 line(s) - queue full\n");
}

TEST(Stress, AsyncLogger_여러_스레드의_줄을_잃지_않고_스레드별_순서를_지킨다)
{
	Captured captured;
	AsyncLogger logger(captured.MakeSink());

	constexpr int32 THREAD_COUNT = 8;
	constexpr int32 PER_THREAD = 2000;

	vector<std::thread> threads;
	for (int32 t = 0; t < THREAD_COUNT; t++)
	{
		threads.emplace_back([&logger, t]()
			{
				for (int32 i = 0; i < PER_THREAD; i++)
					logger.Push("t" + to_string(t) + ":" + to_string(i) + "\n");
			});
	}

	for (auto& thread : threads)
		thread.join();

	logger.Flush();
	EXPECT_EQ(logger.GetDroppedCount(), 0u);

	// 줄 수와 스레드별 순서
	std::istringstream lines(captured.out);
	std::string line;
	int32 next[THREAD_COUNT] = {};
	int32 total = 0;

	while (std::getline(lines, line))
	{
		int32 t = 0, i = 0;
		ASSERT_EQ(::sscanf_s(line.c_str(), "t%d:%d", &t, &i), 2) << line;
		ASSERT_GE(t, 0);
		ASSERT_LT(t, THREAD_COUNT);
		EXPECT_EQ(i, next[t]) << "thread " << t << " out of order";
		next[t] = i + 1;
		total++;
	}

	EXPECT_EQ(total, THREAD_COUNT * PER_THREAD);
}
