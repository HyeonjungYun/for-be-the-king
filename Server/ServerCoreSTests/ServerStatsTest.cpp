#include "pch.h"
#include "ServerStats.h"

namespace
{
	// 버킷 경계. ServerStats.cpp 의 BUCKET_UPPER_US 와 같아야 한다
	//   0:100  1:250  2:500  3:1000  4:2000  5:4000  6:8000
	//   7:16600  8:33000  9:100000  10:UINT64_MAX
	constexpr int32 BUCKETS = ServerStats::BUCKET_COUNT;
}

/*---------------
	RecordFlush — 버킷 분류
---------------*/

TEST(ServerStats, 버킷_상한값은_그_버킷에_들어간다)
{
	// micros <= BUCKET_UPPER_US[i] 이므로 상한값 자신은 그 버킷 소속이다
	ServerStats stats;
	stats.RecordFlush(100);

	uint64 buckets[BUCKETS] = {};
	stats.SnapshotBuckets(buckets);

	EXPECT_EQ(buckets[0], 1u);
	EXPECT_EQ(buckets[1], 0u);
}

TEST(ServerStats, 상한을_1_넘기면_다음_버킷으로_간다)
{
	ServerStats stats;
	stats.RecordFlush(101);

	uint64 buckets[BUCKETS] = {};
	stats.SnapshotBuckets(buckets);

	EXPECT_EQ(buckets[0], 0u);
	EXPECT_EQ(buckets[1], 1u);
}

TEST(ServerStats, 게이트_경계값이_올바른_버킷에_들어간다)
{
	// 16600(P95 기준) · 33000(P99 기준) · 100000(max 기준) 은
	// 판정에 직접 쓰이므로 소속을 못 박아 둔다
	ServerStats stats;
	stats.RecordFlush(16600);
	stats.RecordFlush(33000);
	stats.RecordFlush(100000);

	uint64 buckets[BUCKETS] = {};
	stats.SnapshotBuckets(buckets);

	EXPECT_EQ(buckets[7], 1u);
	EXPECT_EQ(buckets[8], 1u);
	EXPECT_EQ(buckets[9], 1u);
}

TEST(ServerStats, 아주_큰_값은_마지막_버킷으로_간다)
{
	ServerStats stats;
	stats.RecordFlush(100001);
	stats.RecordFlush(UINT64_MAX);

	uint64 buckets[BUCKETS] = {};
	stats.SnapshotBuckets(buckets);

	EXPECT_EQ(buckets[BUCKETS - 1], 2u);
}

TEST(ServerStats, 0은_첫_버킷으로_간다)
{
	ServerStats stats;
	stats.RecordFlush(0);

	uint64 buckets[BUCKETS] = {};
	stats.SnapshotBuckets(buckets);

	EXPECT_EQ(buckets[0], 1u);
}

/*---------------
	RecordFlush — 카운트와 최댓값
---------------*/

TEST(ServerStats, 기록한_횟수를_센다)
{
	ServerStats stats;
	for (int32 i = 0; i < 50; i++)
		stats.RecordFlush(123);

	EXPECT_EQ(stats.GetFlushCount(), 50u);
}

TEST(ServerStats, 최댓값은_갱신될_때만_바뀐다)
{
	ServerStats stats;

	stats.RecordFlush(500);
	EXPECT_EQ(stats.GetFlushMaxUs(), 500u);

	stats.RecordFlush(200);
	EXPECT_EQ(stats.GetFlushMaxUs(), 500u);

	stats.RecordFlush(900);
	EXPECT_EQ(stats.GetFlushMaxUs(), 900u);
}

/*---------------
	PercentileUpperUs
---------------*/

TEST(ServerStats, 표본이_없으면_0을_돌려준다)
{
	uint64 buckets[BUCKETS] = {};
	EXPECT_EQ(ServerStats::PercentileUpperUs(buckets, 0, 0.95), 0u);
}

TEST(ServerStats, 전부_한_버킷이면_그_버킷_상한이_나온다)
{
	uint64 buckets[BUCKETS] = {};
	buckets[3] = 1000;			// 상한 1000us

	EXPECT_EQ(ServerStats::PercentileUpperUs(buckets, 1000, 0.95), 1000u);
	EXPECT_EQ(ServerStats::PercentileUpperUs(buckets, 1000, 0.99), 1000u);
}

TEST(ServerStats, 꼬리가_있으면_p99가_p95보다_크다)
{
	// 950건은 100us 안, 40건은 8000us 안, 10건은 100000us 안
	uint64 buckets[BUCKETS] = {};
	buckets[0] = 950;
	buckets[6] = 40;
	buckets[9] = 10;

	const uint64 p95 = ServerStats::PercentileUpperUs(buckets, 1000, 0.95);
	const uint64 p99 = ServerStats::PercentileUpperUs(buckets, 1000, 0.99);

	EXPECT_EQ(p95, 100u);			// 950/1000 = 95% 가 첫 버킷
	EXPECT_EQ(p99, 8000u);			// 990번째 표본은 buckets[6]
	EXPECT_GT(p99, p95);
}

TEST(ServerStats, 표본이_적으면_p95를_과소평가한다)
{
	// 🔴 알려진 한계를 고정해 둔다.
	//
	//    target = (uint64)(total * p) 인데 total 이 작으면 0 이 된다.
	//    그러면 첫 버킷이 비어 있어도 acc(0) >= target(0) 이 참이라
	//    빈 버킷의 상한을 돌려준다.
	//
	//    표본 1건이 4000us 버킷에 있는데 p95 는 100us 로 보고된다.
	//    실측에서는 창당 flush 가 수만 건이라 영향이 없지만,
	//    서버 기동 직후나 짧은 창에서는 낙관적으로 나온다.
	//
	//    지금 고치지 않는 이유는 과거 측정치와의 비교 기준이 달라지기
	//    때문이다. 현재 동작을 고정해 두고, 고칠 때 이 테스트를 뒤집는다.
	uint64 buckets[BUCKETS] = {};
	buckets[5] = 1;				// 표본 하나가 4000us 버킷에 있다

	EXPECT_EQ(ServerStats::PercentileUpperUs(buckets, 1, 0.95), 100u);
}

TEST(ServerStats, 표본이_충분하면_꼬리를_제대로_잡는다)
{
	// 위 테스트와 같은 분포지만 표본을 100배로 키운다.
	// target 이 0 이 아니게 되면서 정확해진다
	uint64 buckets[BUCKETS] = {};
	buckets[5] = 100;

	EXPECT_EQ(ServerStats::PercentileUpperUs(buckets, 100, 0.95), 4000u);
}

/*---------------
	스트레스
---------------*/

TEST(Stress, ServerStats_동시_RecordFlush가_카운트를_잃지_않는다)
{
	ServerStats stats;

	constexpr int32 THREAD_COUNT = 8;
	constexpr int32 PER_THREAD = 5000;

	vector<std::thread> threads;

	for (int32 t = 0; t < THREAD_COUNT; t++)
	{
		threads.emplace_back([&stats, t]()
			{
				// 스레드마다 다른 버킷을 노려 경합을 만든다
				const uint64 micros = ServerStats::BUCKET_UPPER_US[t % 8];

				for (int32 i = 0; i < PER_THREAD; i++)
					stats.RecordFlush(micros);
			});
	}

	for (auto& t : threads)
		t.join();

	EXPECT_EQ(stats.GetFlushCount(), static_cast<uint64>(THREAD_COUNT) * PER_THREAD);

	uint64 buckets[BUCKETS] = {};
	stats.SnapshotBuckets(buckets);

	uint64 sum = 0;
	for (int32 i = 0; i < BUCKETS; i++)
		sum += buckets[i];

	// 모든 기록이 어느 버킷엔가 정확히 한 번씩 들어가야 한다
	EXPECT_EQ(sum, static_cast<uint64>(THREAD_COUNT) * PER_THREAD);
}