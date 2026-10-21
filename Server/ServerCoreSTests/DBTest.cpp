#include "pch.h"
#include "DBConnectionPool.h"
#include "DBJobQueue.h"

/*---------------
	DBConnectionPool — DB 없이 되는 것들
---------------*/

TEST(DBConnectionPool, 빈_풀에서_Pop하면_nullptr을_준다)
{
	DBConnectionPool pool;

	EXPECT_EQ(pool.Pop(), nullptr);
	EXPECT_EQ(pool.GetIdleCount(), 0);
}

TEST(DBConnectionPool, nullptr을_Push하면_무시한다)
{
	DBConnectionPool pool;

	pool.Push(nullptr);

	EXPECT_EQ(pool.GetIdleCount(), 0);
}

TEST(DBConnectionPool, Push한_것을_Pop이_돌려준다)
{
	DBConnectionPool pool;

	// 연결하지 않은 DBConnection 도 포인터로서는 유효하다.
	// 풀은 커넥션의 상태를 보지 않고 포인터만 관리한다
	DBConnection* a = new DBConnection();
	DBConnection* b = new DBConnection();

	pool.Push(a);
	pool.Push(b);
	EXPECT_EQ(pool.GetIdleCount(), 2);

	// LIFO — 마지막에 넣은 것이 먼저 나온다
	EXPECT_EQ(pool.Pop(), b);
	EXPECT_EQ(pool.Pop(), a);
	EXPECT_EQ(pool.GetIdleCount(), 0);

	delete a;
	delete b;
}

TEST(DBConnectionPool, 연결에_실패하면_false를_주고_풀을_비운다)
{
	// 🔴 회귀 테스트.
	//
	//    예전에는 실패 분기에 return 이 없어서, delete 한 포인터를
	//    _connections·_idle 에 그대로 넣고 마지막에 return true 를 했다.
	//    use-after-free 와 이중 해제가 동시에 가능한 상태였고,
	//    서버는 연결에 실패했는데 성공한 줄 알고 기동했다.
	//
	//    포트 1 에는 MySQL 이 없으므로 연결이 반드시 실패한다.
	DBConnectionPool pool;

	const bool connected = pool.Connect(2, "127.0.0.1", 1, "nobody", "wrong", "nodb");

	EXPECT_FALSE(connected);
	EXPECT_EQ(pool.GetIdleCount(), 0);

	// 댕글링 포인터가 남아 있으면 여기서 이중 해제로 죽는다
	pool.Clear();
	SUCCEED();
}

TEST(DBConnectionPool, Clear를_두_번_해도_안전하다)
{
	DBConnectionPool pool;

	pool.Clear();
	pool.Clear();

	EXPECT_EQ(pool.GetIdleCount(), 0);
}

/*---------------
	DBJobQueue — 인자 검증과 수명
---------------*/

TEST(DBJobQueue, 풀이_없으면_Init이_실패한다)
{
	DBJobQueue queue;

	EXPECT_FALSE(queue.Init(4, nullptr));
}

TEST(DBJobQueue, 스레드가_0개_이하면_Init이_실패한다)
{
	DBConnectionPool pool;
	DBJobQueue queue;

	EXPECT_FALSE(queue.Init(0, &pool));
	EXPECT_FALSE(queue.Init(-1, &pool));
}

TEST(DBJobQueue, Init_전에_Push하면_무시한다)
{
	// _running 이 false 인 동안 들어온 작업은 버린다.
	// 아무도 꺼내지 않을 큐에 쌓아두면 종료할 때 처리량만 늘어난다
	DBJobQueue queue;

	queue.Push([](DBConnection*) {});
	queue.Push([](DBConnection*) {});

	EXPECT_EQ(queue.GetPendingCount(), 0);
}

TEST(DBJobQueue, 커넥션이_없으면_워커가_스스로_물러난다)
{
	// 빈 풀로 시작하면 워커는 Pop 에서 nullptr 을 받고 즉시 종료한다.
	// Init 자체는 성공하고, Shutdown 이 이미 끝난 스레드를 join 해도 안전해야 한다
	DBConnectionPool pool;
	DBJobQueue queue;

	EXPECT_TRUE(queue.Init(2, &pool));

	queue.Shutdown();
	SUCCEED();
}

TEST(DBJobQueue, Shutdown을_두_번_해도_안전하다)
{
	DBConnectionPool pool;
	DBJobQueue queue;

	queue.Init(1, &pool);

	queue.Shutdown();
	queue.Shutdown();

	SUCCEED();
}

TEST(DBJobQueue, Shutdown_후에_Push하면_무시한다)
{
	DBConnectionPool pool;
	DBJobQueue queue;

	queue.Init(1, &pool);
	queue.Shutdown();

	queue.Push([](DBConnection*) {});

	EXPECT_EQ(queue.GetPendingCount(), 0);
}

/*---------------
	DBJobQueue::PushAfter — 재시도 대기 동안 워커를 잡지 않는다 (ADR-0010 R-1, 2026-10-08)
---------------*/

namespace
{
	// 연결하지 않은 커넥션 하나로 워커 하나를 돌린다 - 작업은 커넥션을 쓰지 않는다
	struct OneWorker
	{
		DBConnectionPool	pool;
		DBJobQueue			queue;
		DBConnection*		conn = new DBConnection();

		OneWorker()
		{
			pool.Push(conn);
			queue.Init(1, &pool);
		}

		~OneWorker()
		{
			queue.Shutdown();
			pool.Pop();
			delete conn;
		}
	};
}

TEST(DBJobQueue, PushAfter_는_시간이_된_뒤에_마감_순서대로_돈다)
{
	OneWorker w;
	mutex lock;
	vector<int32> order;
	atomic<int32> done = 0;

	const auto start = chrono::steady_clock::now();
	chrono::steady_clock::duration ranAt300{};

	auto record = [&](int32 id) { lock_guard<mutex> g(lock); order.push_back(id); done.fetch_add(1); };

	w.queue.PushAfter(300, [&](DBConnection*) { ranAt300 = chrono::steady_clock::now() - start; record(300); });
	w.queue.PushAfter(100, [&](DBConnection*) { record(100); });
	w.queue.Push([&](DBConnection*) { record(0); });

	EXPECT_EQ(w.queue.GetDelayedCount(), 2);

	for (int32 i = 0; i < 200 && done.load() < 3; i++)
		this_thread::sleep_for(chrono::milliseconds(10));

	ASSERT_EQ(done.load(), 3);
	EXPECT_EQ(order, (vector<int32>{ 0, 100, 300 }));
	EXPECT_GE(chrono::duration_cast<chrono::milliseconds>(ranAt300).count(), 290) << "마감 전에 돌았다";
}

TEST(DBJobQueue, 지연_작업이_기다리는_동안_워커는_다른_작업을_한다)
{
	// 회귀: 예전 재시도는 작업 안에서 sleep 했다 - 막힌 저장이 워커를 잡고 있어 로그인 · 저장이 줄을 섰다
	OneWorker w;
	atomic<bool> immediateRan = false;
	atomic<bool> delayedRan = false;

	w.queue.PushAfter(500, [&](DBConnection*) { delayedRan = true; });
	w.queue.Push([&](DBConnection*) { immediateRan = true; });

	for (int32 i = 0; i < 30 && immediateRan.load() == false; i++)
		this_thread::sleep_for(chrono::milliseconds(10));

	EXPECT_TRUE(immediateRan.load()) << "워커 하나뿐인데 300 ms 안에 즉시 작업이 돌았다 = 지연 작업이 워커를 잡지 않는다";
	EXPECT_FALSE(delayedRan.load());
}

TEST(DBJobQueue, 종료하면_남은_지연_작업을_기다리지_않고_한_번씩_돌린다)
{
	atomic<int32> ran = 0;
	const auto start = chrono::steady_clock::now();
	{
		OneWorker w;
		w.queue.PushAfter(60'000, [&](DBConnection*) { ran.fetch_add(1); });
		w.queue.PushAfter(60'000, [&](DBConnection*) { ran.fetch_add(1); });
	}	// Shutdown

	EXPECT_EQ(ran.load(), 2);
	EXPECT_LT(chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - start).count(), 5'000) << "1 분을 기다렸다";
}

TEST(DBConnection, 한_번도_연결하지_않았으면_재연결은_false_오류_번호는_0)
{
	DBConnection conn;

	EXPECT_FALSE(conn.Reconnect());
	EXPECT_EQ(conn.GetLastErrorNo(), 0u);
	EXPECT_EQ(conn.GetReconnectCount(), 0u);
}

TEST(DBConnection, 연결이_없으면_마지막_연결_시도의_오류를_준다)
{
	// 회귀 위험: 재연결이 실패한 커넥션의 쿼리가 "오류 0" 이면 저장 재시도가 영구 실패로 오판해 관대 단계 · 포기로 간다
	DBConnection conn;

	EXPECT_FALSE(conn.Connect("127.0.0.1", 1, "nobody", "wrong", "nodb"));		// 포트 1 - 반드시 실패
	EXPECT_FALSE(conn.Execute("SELECT 1"));
	EXPECT_NE(conn.GetLastErrorNo(), 0u);
	EXPECT_GE(conn.GetLastErrorNo(), 2000u) << "클라이언트 연결 오류(2002 · 2003) - DBError 가 연결 끊김으로 본다";

	EXPECT_FALSE(conn.Reconnect()) << "같은 정보로 다시 - 여전히 실패";
	EXPECT_EQ(conn.GetReconnectCount(), 1u);
}