#include "pch.h"
#include "Listener.h"
#include "SocketUtils.h"
#include "IocpEvent.h"
#include "Session.h"
#include "Service.h"

/*--------------
	Listener
---------------*/

Listener::~Listener()
{
	SocketUtils::Close(_socket);

	for (AcceptEvent* acceptEvent : _acceptEvents)
	{
		// TODO

		delete(acceptEvent);
	}
}

bool Listener::StartAccept(ServerServiceRef service)
{
	_service = service;
	if (_service == nullptr)
		return false;

	_socket = SocketUtils::CreateSocket();
	if (_socket == INVALID_SOCKET)
		return false;

	if (_service->GetIocpCore()->Register(shared_from_this()) == false)
		return false;

	if (SocketUtils::SetReuseAddress(_socket, true) == false)
		return false;

	if (SocketUtils::SetLinger(_socket, 0, 0) == false)
		return false;

	if (SocketUtils::Bind(_socket, _service->GetNetAddress()) == false)
		return false;

	if (SocketUtils::Listen(_socket) == false)
		return false;

	const int32 acceptCount = _service->GetMaxSessionCount();
	for (int32 i = 0; i < acceptCount; i++)
	{
		AcceptEvent* acceptEvent = new AcceptEvent();
		acceptEvent->owner = shared_from_this();
		_acceptEvents.push_back(acceptEvent);
		RegisterAccept(acceptEvent);
	}

	return true;
}

void Listener::CloseSocket()
{
	_closed.store(true);
	SocketUtils::Close(_socket);
}

HANDLE Listener::GetHandle()
{
	return reinterpret_cast<HANDLE>(_socket);
}

void Listener::Dispatch(IocpEvent* iocpEvent, int32 /*numOfBytes*/)
{
	ASSERT_CRASH(iocpEvent->eventType == EventType::Accept);
	AcceptEvent* acceptEvent = static_cast<AcceptEvent*>(iocpEvent);
	ProcessAccept(acceptEvent);
}

void Listener::RegisterAccept(AcceptEvent* acceptEvent)
{
	// 예전엔 실패하면 자기를 다시 불렀다 - 실패가 계속되면(리슨 소켓 닫힘 · 자원 고갈) 스택이 넘치고 세션(640 KB)을 계속 만들었다.
	// CreateSession 이 null(IOCP 등록 실패)이면 바로 역참조했다. 이제 몇 번만 다시 해 본다
	constexpr int32 MAX_ATTEMPTS = 8;
	if (_closed.load())
		return;		// 닫는 중 - 슬롯을 다시 걸지도, 보관하지도 않는다
	for (int32 attempt = 0; attempt < MAX_ATTEMPTS; attempt++)
	{
		SessionRef session = _service->CreateSession(); // Register IOCP
		if (session == nullptr)
			continue;

		acceptEvent->Init();
		acceptEvent->session = session;

		DWORD bytesReceived = 0;
		if (SocketUtils::AcceptEx(_socket, session->GetSocket(), session->_recvBuffer.WritePos(), 0, sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16, OUT & bytesReceived, static_cast<LPOVERLAPPED>(acceptEvent))
			|| ::WSAGetLastError() == WSA_IO_PENDING)
			return;
	}

	// 이번엔 포기하지만 슬롯은 잃지 않는다 - 서버 주 루프가 RetryLostAccepts 로 다시 건다 (TD-014, 2026-10-18)
	{
		WRITE_LOCK;
		_lostAccepts.push_back(acceptEvent);
	}
	LOG_LINE << "[ACCEPT] accept slot failed " << MAX_ATTEMPTS << " times - parked for retry\n";
}

int32 Listener::RetryLostAccepts()
{
	vector<AcceptEvent*> lost;
	{
		WRITE_LOCK;
		lost.swap(_lostAccepts);
	}

	for (AcceptEvent* acceptEvent : lost)
		RegisterAccept(acceptEvent);		// 또 실패하면 다시 목록으로

	if (lost.empty() == false)
		LOG_LINE << "[ACCEPT] retried " << lost.size() << " parked accept slot(s), still parked=" << LostAcceptCount() << '\n';
	return static_cast<int32>(lost.size());
}

int32 Listener::LostAcceptCount()
{
	WRITE_LOCK;
	return static_cast<int32>(_lostAccepts.size());
}

void Listener::ProcessAccept(AcceptEvent* acceptEvent)
{
	SessionRef session = acceptEvent->session;
	if (_closed.load())
	{
		acceptEvent->session = nullptr;		// 닫는 중에 돌아온 슬롯 - 세션을 놓아 준다
		return;
	}

	if (false == SocketUtils::SetUpdateAcceptSocket(session->GetSocket(), _socket))
	{
		RegisterAccept(acceptEvent);
		return;
	}

	SOCKADDR_IN sockAddress;
	int32 sizeOfSockAddr = sizeof(sockAddress);
	if (SOCKET_ERROR == ::getpeername(session->GetSocket(), OUT reinterpret_cast<SOCKADDR*>(&sockAddress), &sizeOfSockAddr))
	{
		RegisterAccept(acceptEvent);
		return;
	}

	session->SetNetAddress(NetAddress(sockAddress));
	session->ProcessConnect();
	RegisterAccept(acceptEvent);
}