#include "pch.h"
#include <thread>
#include "Service.h"
#include "Session.h"
#include "SendBuffer.h"
#include "IocpCore.h"

// Session::Send 동시성 (technical-preferences § 동시성 스트레스 테스트 - 필수 대상 Session::_sendQueue).
// 2026-08-10 _sendQueue 레이스 · 2026-10-18 TD-007(WSASend 를 락 밖으로) 의 회귀 테스트. 실 소켓(루프백)이다.
// 8 스레드가 한 세션으로 동시에 보내고, 받는 쪽이 바이트 수 · 스레드별 순서를 센다.

namespace
{
	constexpr uint16	STRESS_PORT = 17781;		// 게임 서버(7777)와 겹치지 않게
	constexpr int32		SEND_THREADS = 8;
	constexpr int32		SENDS_PER_THREAD = 3000;
	constexpr int32		PACKET_BYTES = static_cast<int32>(sizeof(PacketHeader)) + 4;

	atomic<int64>		g_recvPackets{ 0 };
	atomic<bool>		g_orderBroken{ false };
	int32				g_lastSeq[SEND_THREADS];		// 받는 세션 하나의 수신은 한 번에 하나라 락이 필요 없다
	atomic<Session*>	g_client{ nullptr };

	class CountingSession : public PacketSession
	{
	protected:
		void OnRecvPacket(BYTE* buffer, int32 len) override
		{
			if (len != PACKET_BYTES)
			{
				g_orderBroken.store(true);
				return;
			}
			const PacketHeader* header = reinterpret_cast<const PacketHeader*>(buffer);
			int32 seq = 0;
			::memcpy(&seq, buffer + sizeof(PacketHeader), sizeof(seq));
			const uint16 thread = header->id;
			if (thread >= SEND_THREADS || seq != g_lastSeq[thread] + 1)
				g_orderBroken.store(true);
			else
				g_lastSeq[thread] = seq;
			g_recvPackets.fetch_add(1);
		}
	};

	class SenderSession : public PacketSession
	{
	protected:
		void OnConnected() override { g_client.store(this); }
		void OnRecvPacket(BYTE*, int32) override {}
	};

	SendBufferRef MakePacket(uint16 thread, int32 seq)
	{
		SendBufferRef buf = make_shared<SendBuffer>(PACKET_BYTES);
		BYTE bytes[PACKET_BYTES];
		PacketHeader header{ static_cast<uint16>(PACKET_BYTES), thread };
		::memcpy(bytes, &header, sizeof(header));
		::memcpy(bytes + sizeof(header), &seq, sizeof(seq));
		buf->CopyData(bytes, PACKET_BYTES);
		return buf;
	}
}

TEST(Stress, Session_8스레드_동시_Send가_패킷도_스레드별_순서도_잃지_않는다_TD_007)
{
	g_recvPackets.store(0);
	g_orderBroken.store(false);
	g_client.store(nullptr);
	for (int32& s : g_lastSeq)
		s = -1;

	// CI 가 --gtest_repeat 로 돌린다 - 반복마다 다른 포트 (앞 반복의 소켓이 TIME_WAIT 로 남아도 섞이지 않게)
	static uint16 s_run = 0;
	const uint16 port = static_cast<uint16>(STRESS_PORT + (s_run++ % 200));

	ServerServiceRef server = make_shared<ServerService>(NetAddress(L"127.0.0.1", port), make_shared<IocpCore>(),
		[]() { return make_shared<CountingSession>(); }, 1);
	ASSERT_TRUE(server->Start());

	ClientServiceRef client = make_shared<ClientService>(NetAddress(L"127.0.0.1", port), make_shared<IocpCore>(),
		[]() { return make_shared<SenderSession>(); }, 1);
	ASSERT_TRUE(client->Start());

	atomic<bool> stop{ false };
	vector<std::thread> io;
	for (int32 i = 0; i < 2; i++)
	{
		io.emplace_back([&stop, server]() { while (stop.load() == false) server->GetIocpCore()->Dispatch(5); });
		io.emplace_back([&stop, client]() { while (stop.load() == false) client->GetIocpCore()->Dispatch(5); });
	}

	auto waitFor = [](auto&& cond, int32 timeoutMs)
	{
		for (int32 waited = 0; waited < timeoutMs && cond() == false; waited += 5)
			this_thread::sleep_for(chrono::milliseconds(5));
		return cond();
	};

	const bool connected = waitFor([]() { return g_client.load() != nullptr; }, 5000);
	EXPECT_TRUE(connected);

	if (connected)
	{
		SessionRef sender = g_client.load()->GetSessionRef();

		vector<std::thread> senders;
		for (int32 t = 0; t < SEND_THREADS; t++)
		{
			senders.emplace_back([sender, t]()
			{
				for (int32 seq = 0; seq < SENDS_PER_THREAD; seq++)
					sender->Send(MakePacket(static_cast<uint16>(t), seq));
			});
		}
		for (auto& s : senders)
			s.join();

		const int64 expected = static_cast<int64>(SEND_THREADS) * SENDS_PER_THREAD;
		EXPECT_TRUE(waitFor([expected]() { return g_recvPackets.load() >= expected; }, 10000)) << "received " << g_recvPackets.load() << " / " << expected;
		EXPECT_EQ(g_recvPackets.load(), expected);
		EXPECT_FALSE(g_orderBroken.load());
		EXPECT_TRUE(sender->IsConnected());

		sender->Disconnect(L"test end");
		waitFor([]() { return false; }, 100);
	}

	stop.store(true);
	for (auto& t : io)
		t.join();
	server->CloseService();
}
