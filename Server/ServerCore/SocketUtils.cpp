#include "pch.h"
#include "SocketUtils.h"
#include <mstcpip.h>		// tcp_keepalive · SIO_KEEPALIVE_VALS

/*----------------
	SocketUtils
-----------------*/

LPFN_CONNECTEX		SocketUtils::ConnectEx = nullptr;
LPFN_DISCONNECTEX	SocketUtils::DisconnectEx = nullptr;
LPFN_ACCEPTEX		SocketUtils::AcceptEx = nullptr;

void SocketUtils::Init()
{
	WSADATA wsaData;
	ASSERT_CRASH(::WSAStartup(MAKEWORD(2, 2), OUT &wsaData) == 0);
	
	/* 런타임에 주소 얻어오는 API */
	SOCKET dummySocket = CreateSocket();
	ASSERT_CRASH(BindWindowsFunction(dummySocket, WSAID_CONNECTEX, reinterpret_cast<LPVOID*>(&ConnectEx)));
	ASSERT_CRASH(BindWindowsFunction(dummySocket, WSAID_DISCONNECTEX, reinterpret_cast<LPVOID*>(&DisconnectEx)));
	ASSERT_CRASH(BindWindowsFunction(dummySocket, WSAID_ACCEPTEX, reinterpret_cast<LPVOID*>(&AcceptEx)));
	Close(dummySocket);
}

void SocketUtils::Clear()
{
	::WSACleanup();
}

bool SocketUtils::BindWindowsFunction(SOCKET socket, GUID guid, LPVOID* fn)
{
	DWORD bytes = 0;
	return SOCKET_ERROR != ::WSAIoctl(socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), fn, sizeof(*fn), OUT & bytes, NULL, NULL);
}

SOCKET SocketUtils::CreateSocket()
{
	return ::WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
}

bool SocketUtils::SetLinger(SOCKET socket, uint16 onoff, uint16 linger)
{
	LINGER option;
	option.l_onoff = onoff;
	option.l_linger = linger;
	return SetSockOpt(socket, SOL_SOCKET, SO_LINGER, option);
}

bool SocketUtils::SetReuseAddress(SOCKET socket, bool flag)
{
	// BOOL 옵션은 4 바이트로 - bool(1 바이트)을 넘기면 setsockopt 는 성공을 돌려주면서 값은 적용되지 않았다 (2026-10-21 SessionOptionsTest 로 발견)
	const int32 value = flag ? 1 : 0;
	return SetSockOpt(socket, SOL_SOCKET, SO_REUSEADDR, value);
}

bool SocketUtils::SetRecvBufferSize(SOCKET socket, int32 size)
{
	return SetSockOpt(socket, SOL_SOCKET, SO_RCVBUF, size);
}

bool SocketUtils::SetSendBufferSize(SOCKET socket, int32 size)
{
	return SetSockOpt(socket, SOL_SOCKET, SO_SNDBUF, size);
}

bool SocketUtils::SetTcpNoDelay(SOCKET socket, bool flag)
{
	// TCP_NODELAY 는 IPPROTO_TCP 단계다 - SOL_SOCKET 이면 같은 값(1)인 SO_DEBUG 를 켠다. Session::ProcessConnect 가 모든 세션에 켠다 (2026-10-21).
	// 값은 4 바이트 BOOL 로 (bool 1 바이트는 조용히 무시됐다)
	const int32 value = flag ? 1 : 0;
	return SetSockOpt(socket, IPPROTO_TCP, TCP_NODELAY, value);
}

bool SocketUtils::SetKeepAlive(SOCKET socket, uint32 idleMs, uint32 intervalMs)
{
	// SO_KEEPALIVE 를 켜고(getsockopt 로 확인 가능), 주기는 SIO_KEEPALIVE_VALS 로 (기본 2 시간 / 1 초 → idleMs / intervalMs)
	const int32 on = 1;
	if (SetSockOpt(socket, SOL_SOCKET, SO_KEEPALIVE, on) == false)
		return false;

	tcp_keepalive option;
	option.onoff = 1;
	option.keepalivetime = idleMs;
	option.keepaliveinterval = intervalMs;

	DWORD bytes = 0;
	return SOCKET_ERROR != ::WSAIoctl(socket, SIO_KEEPALIVE_VALS, &option, sizeof(option), nullptr, 0, OUT &bytes, nullptr, nullptr);
}

// ListenSocket의 특성을 ClientSocket에 그대로 적용
bool SocketUtils::SetUpdateAcceptSocket(SOCKET socket, SOCKET listenSocket)
{
	return SetSockOpt(socket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, listenSocket);
}

bool SocketUtils::Bind(SOCKET socket, NetAddress netAddr)
{
	return SOCKET_ERROR != ::bind(socket, reinterpret_cast<const SOCKADDR*>(&netAddr.GetSockAddr()), sizeof(SOCKADDR_IN));
}

bool SocketUtils::BindAnyAddress(SOCKET socket, uint16 port)
{
	SOCKADDR_IN myAddress;
	myAddress.sin_family = AF_INET;
	myAddress.sin_addr.s_addr = ::htonl(INADDR_ANY);
	myAddress.sin_port = ::htons(port);

	return SOCKET_ERROR != ::bind(socket, reinterpret_cast<const SOCKADDR*>(&myAddress), sizeof(myAddress));
}

bool SocketUtils::Listen(SOCKET socket, int32 backlog)
{
	return SOCKET_ERROR != ::listen(socket, backlog);
}

void SocketUtils::Close(SOCKET& socket)
{
	if (socket != INVALID_SOCKET)
		::closesocket(socket);
	socket = INVALID_SOCKET;
}