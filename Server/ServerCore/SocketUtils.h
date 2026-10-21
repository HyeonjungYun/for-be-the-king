#pragma once
#include "NetAddress.h"

/*----------------
	SocketUtils
-----------------*/

class SocketUtils
{
public:
	static LPFN_CONNECTEX		ConnectEx;
	static LPFN_DISCONNECTEX	DisconnectEx;
	static LPFN_ACCEPTEX		AcceptEx;

public:
	static void Init();
	static void Clear();

	static bool BindWindowsFunction(SOCKET socket, GUID guid, LPVOID* fn);
	static SOCKET CreateSocket();

	static bool SetLinger(SOCKET socket, uint16 onoff, uint16 linger);
	static bool SetReuseAddress(SOCKET socket, bool flag);
	static bool SetRecvBufferSize(SOCKET socket, int32 size);
	static bool SetSendBufferSize(SOCKET socket, int32 size);
	static bool SetTcpNoDelay(SOCKET socket, bool flag);

	// TCP keepalive - idleMs 동안 조용하면 intervalMs 간격으로 탐침을 보낸다 (SIO_KEEPALIVE_VALS). 죽은 상대를 OS 가 끊어 준다 (2026-10-21)
	static bool SetKeepAlive(SOCKET socket, uint32 idleMs, uint32 intervalMs);
	static bool SetUpdateAcceptSocket(SOCKET socket, SOCKET listenSocket);

	static bool Bind(SOCKET socket, NetAddress netAddr);
	static bool BindAnyAddress(SOCKET socket, uint16 port);
	static bool Listen(SOCKET socket, int32 backlog = SOMAXCONN);
	static void Close(SOCKET& socket);
};

// 🔴 BOOL · int 옵션은 int32 로 넘길 것 - bool(1 바이트)을 넘기면 Winsock 이 성공을 돌려주면서 값을 적용하지 않는다 (2026-10-21)
template<typename T>
static inline bool SetSockOpt(SOCKET socket, int32 level, int32 optName, T optVal)
{
	static_assert(std::is_same_v<T, bool> == false, "pass an int32 (0 / 1) - a 1-byte bool is silently ignored by setsockopt");
	return SOCKET_ERROR != ::setsockopt(socket, level, optName, reinterpret_cast<char*>(&optVal), sizeof(T));
}