#include "pch.h"
#include "RecvBuffer.h"

// RecvBuffer(bufferSize) → capacity = bufferSize × BUFFER_COUNT(10)
// 아래는 전부 bufferSize=100 → capacity 1000 을 전제한다.

TEST(RecvBuffer, 생성_직후_전체가_비어있다)
{
	RecvBuffer buf(100);
	EXPECT_EQ(buf.DataSize(), 0);
	EXPECT_EQ(buf.FreeSize(), 1000);
}

TEST(RecvBuffer, 용량을_넘겨_쓰면_실패한다)
{
	RecvBuffer buf(100);
	EXPECT_TRUE(buf.OnWrite(1000));
	EXPECT_FALSE(buf.OnWrite(1));
}

TEST(RecvBuffer, 쓴_것보다_많이_읽으면_실패한다)
{
	RecvBuffer buf(100);
	buf.OnWrite(50);

	EXPECT_FALSE(buf.OnRead(51));
	EXPECT_TRUE(buf.OnRead(50));
}

TEST(RecvBuffer, 다_읽고_Clean하면_커서가_0으로_돌아간다)
{
	RecvBuffer buf(100);
	buf.OnWrite(500);
	buf.OnRead(500);

	buf.Clean();

	EXPECT_EQ(buf.DataSize(), 0);
	EXPECT_EQ(buf.FreeSize(), 1000);
}

TEST(RecvBuffer, 남은_공간이_한_칸보다_작으면_데이터를_앞으로_당긴다)
{
	RecvBuffer buf(100);
	buf.OnWrite(950);    // FreeSize = 50 < bufferSize(100)
	buf.OnRead(900);     // DataSize = 50

	buf.Clean();

	EXPECT_EQ(buf.DataSize(), 50);
	EXPECT_EQ(buf.FreeSize(), 950);
}

TEST(RecvBuffer, 남은_공간이_충분하면_Clean이_아무것도_하지_않는다)
{
	RecvBuffer buf(100);
	buf.OnWrite(200);
	buf.OnRead(100);

	buf.Clean();

	EXPECT_EQ(buf.DataSize(), 100);
	EXPECT_EQ(buf.FreeSize(), 800);
}

TEST(RecvBuffer, FreeSize가_0인_상태를_만들_수_있다)
{
	// 🔴 이 상태에서 Session::RegisterRecv 는 wsaBuf.len = 0 으로 WSARecv 를 건다.
	//    0바이트 완료 → ProcessRecv(0) → Disconnect(L"Recv 0") 로 정상 접속이 끊긴다.
	//    RecvBuffer 자체의 버그는 아니지만 이 조건을 만들 수 있다는 사실을 기록해 둔다.
	RecvBuffer buf(100);
	EXPECT_TRUE(buf.OnWrite(1000));
	EXPECT_EQ(buf.FreeSize(), 0);
}

// 2026-11-21 (동접 1만) - 작게 시작한 버퍼가 큰 패킷을 만나면 커지고, 비면 처음 크기로 돌아간다
TEST(RecvBuffer, 가득_찬_채로_Clean하면_키워서_다음_수신_자리를_만든다)
{
	RecvBuffer buf(100, 1);		// 처음 100 바이트 - 300 바이트짜리 패킷이 걸쳐 들어오는 중
	EXPECT_TRUE(buf.OnWrite(100));
	EXPECT_EQ(buf.FreeSize(), 0);

	buf.Clean();
	EXPECT_EQ(buf.Capacity(), 200);
	EXPECT_EQ(buf.DataSize(), 100);		// 받은 조각은 그대로
	EXPECT_EQ(buf.FreeSize(), 100);

	EXPECT_TRUE(buf.OnWrite(100));
	buf.Clean();
	EXPECT_EQ(buf.Capacity(), 400);
	EXPECT_TRUE(buf.OnWrite(100));		// 300 바이트가 다 들어왔다
	EXPECT_TRUE(buf.OnRead(300));

	buf.Clean();
	EXPECT_EQ(buf.Capacity(), 100);		// 비었으니 처음 크기로
	EXPECT_EQ(buf.FreeSize(), 100);
}

TEST(RecvBuffer, 여유가_넉넉하면_키우지_않고_상한을_넘지_않는다)
{
	RecvBuffer buf(100, 1);
	EXPECT_TRUE(buf.OnWrite(60));
	EXPECT_TRUE(buf.OnRead(10));
	buf.Clean();							// 당기면 50 / 100 - 3/4 미만이라 그대로
	EXPECT_EQ(buf.Capacity(), 100);

	RecvBuffer big(RecvBuffer::MAX_CAPACITY, 1);
	EXPECT_TRUE(big.OnWrite(RecvBuffer::MAX_CAPACITY));
	big.Clean();
	EXPECT_EQ(big.Capacity(), RecvBuffer::MAX_CAPACITY);		// 상한에서는 더 키우지 않는다
}