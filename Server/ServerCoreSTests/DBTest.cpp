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