#include "pch.h"
#include <thread>
#include "Service.h"
#include "Session.h"
#include "IocpCore.h"
#include "SocketUtils.h"

// 세션 소켓 옵션 (2026-10-21 리뷰 후속). 실 소켓(루프백)으로 받는 쪽 · 거는 쪽 모두 ProcessConnect 를 지나며
// TCP_NODELAY · SO_KEEPALIVE 가 켜지는지 getsockopt 로 본다. 예전엔 둘 다 꺼져 있었다 -
// 33 ms 배치 뒤의 작은 패킷이 Nagle 에 묶였고, 선이 뽑힌 상대를 송신 상한(8 MB)이 찰 때까지 몰랐다.

namespace
{
	constexpr uint16 OPTIONS_PORT = 17991;		// SessionStressTest(17781~17980) 와 겹치지 않게

	atomic<Session*> g_accepted{ nullptr };
	atomic<Session*> g_connected{ nullptr };

	class AcceptedSession : public PacketSession
	{
	protected:
		void OnConnected() override { g_accepted.store(this); }
		void OnRecvPacket(BYTE*, int32) override {}
	};

	class ConnectingSession : public PacketSession
	{
	protected:
		void OnConnected() override { g_connected.store(this); }
		void OnRecvPacket(BYTE*, int32) override {}
	};

	// Winsock 은 BOOL 옵션(TCP_NODELAY · SO_KEEPALIVE)을 1 바이트로 돌려주기도 한다 - 돌려준 길이만큼만 본다
	int32 GetIntOption(SOCKET socket, int32 level, int32 name)
	{
		int32 value = 0;
		int32 length = sizeof(value);
		if (::getsockopt(socket, level, name, reinterpret_cast<char*>(&value), &length) == SOCKET_ERROR)
			return -1;
		if (length == 1)
			return static_cast<int32>(*reinterpret_cast<unsigned char*>(&value));
		return value;
	}
}

TEST(SessionOptions, 받는_쪽과_거는_쪽_모두_NODELAY와_KEEPALIVE가_켜진다)
{
	g_accepted.store(nullptr);
	g_connected.store(nullptr);

	static uint16 s_run = 0;
	const uint16 port = static_cast<uint16>(OPTIONS_PORT + (s_run++ % 8));

	ServerServiceRef server = make_shared<ServerService>(NetAddress(L"127.0.0.1", port), make_shared<IocpCore>(),
		[]() { return make_shared<AcceptedSession>(); }, 1);
	ASSERT_TRUE(server->Start());

	ClientServiceRef client = make_shared<ClientService>(NetAddress(L"127.0.0.1", port), make_shared<IocpCore>(),
		[]() { return make_shared<ConnectingSession>(); }, 1);
	ASSERT_TRUE(client->Start());

	atomic<bool> stop{ false };
	std::thread serverIo([&stop, server]() { while (stop.load() == false) server->GetIocpCore()->Dispatch(5); });
	std::thread clientIo([&stop, client]() { while (stop.load() == false) client->GetIocpCore()->Dispatch(5); });

	for (int32 waited = 0; waited < 5000 && (g_accepted.load() == nullptr || g_connected.load() == nullptr); waited += 5)
		this_thread::sleep_for(chrono::milliseconds(5));

	ASSERT_NE(g_accepted.load(), nullptr) << "accept did not complete";
	ASSERT_NE(g_connected.load(), nullptr) << "connect did not complete";

	for (Session* session : { g_accepted.load(), g_connected.load() })
	{
		const SOCKET socket = session->GetSocket();
		const int32 nodelay = GetIntOption(socket, IPPROTO_TCP, TCP_NODELAY);
		const int32 keepalive = GetIntOption(socket, SOL_SOCKET, SO_KEEPALIVE);
		EXPECT_EQ(nodelay, 1) << "TCP_NODELAY off on socket " << socket;
		EXPECT_EQ(keepalive, 1) << "SO_KEEPALIVE off on socket " << socket;
	}

	// 켜지 않은 소켓은 둘 다 0 이어야 한다 - 위 검사가 "항상 1" 로 통과하는 가짜가 아닌지 (대조군)
	{
		const SOCKET bare = SocketUtils::CreateSocket();
		EXPECT_EQ(GetIntOption(bare, IPPROTO_TCP, TCP_NODELAY), 0);
		EXPECT_EQ(GetIntOption(bare, SOL_SOCKET, SO_KEEPALIVE), 0);
		::closesocket(bare);
	}

	g_connected.load()->GetSessionRef()->Disconnect(L"test end");
	this_thread::sleep_for(chrono::milliseconds(100));

	stop.store(true);
	serverIo.join();
	clientIo.join();
	server->CloseService();
}
