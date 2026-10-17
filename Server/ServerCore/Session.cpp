#include "pch.h"
#include "Session.h"
#include "SocketUtils.h"
#include "Service.h"
#include "ServerStats.h"

namespace
{
	// 로그 전용. Disconnect 의 cause 는 전부 ASCII 리터럴이라는 전제다.
	// API(const WCHAR*)를 바꾸면 호출부 전체가 파급되므로 출력 지점에서만 좁힌다.
	string ToNarrow(const WCHAR* wide)
	{
		string narrow;
		for (; wide != nullptr && *wide != L'\0'; ++wide)
			narrow.push_back(static_cast<char>(*wide < 128 ? *wide : '?'));

		return narrow;
	}
}

/*--------------
	Session
---------------*/

Session::Session() : _recvBuffer(BUFFER_SIZE)
{
	_socket = SocketUtils::CreateSocket();
}

Session::~Session()
{
	SocketUtils::Close(_socket);
}

void Session::Send(SendBufferRef sendBuffer)
{
	// MakeSendBuffer 는 uint16 에 안 들어가는 패킷을 만들지 않고 null 을 준다 (보안 점검 C2)
	if (sendBuffer == nullptr || IsConnected() == false)
		return;

	// 패킷 종류별 송신량 (SV-5 아이템 몫). 버퍼에 통계 칸이 지정돼 있으면 그쪽으로 센다
	if (sendBuffer->WriteSize() >= static_cast<int32>(sizeof(PacketHeader)))
	{
		const uint16 statId = sendBuffer->GetStatId();
		const uint16 id = (statId != 0) ? statId : reinterpret_cast<PacketHeader*>(sendBuffer->Buffer())->id;
		GStats.AddSentPacket(id, sendBuffer->WriteSize());
	}

	bool registerSend = false;
	bool overflow = false;

	// 큐에 넣는 것만 락 안에서. WSASend 는 락 밖에서 (TD-007, 2026-10-18) - 예전엔 락을 쥔 채 커널 호출을 해
	// 같은 세션에 방송하는 방 스레드들이 그동안 기다렸다. 송신 등록은 _sendRegistered 를 먼저 잡은 한 스레드만 한다
	{
		WRITE_LOCK;

		// 읽지 않는 상대 - 더 쌓지 않고 끊는다 (Session.h MAX_PENDING_SEND_BYTES)
		_pendingSendBytes += sendBuffer->WriteSize();
		if (_pendingSendBytes > MAX_PENDING_SEND_BYTES)
		{
			overflow = true;
		}
		else
		{
			_sendQueue.push(std::move(sendBuffer));
			registerSend = (_sendRegistered.exchange(true) == false);
		}
	}

	if (overflow)
	{
		Disconnect(L"send queue full");
		return;
	}

	if (registerSend)
		RegisterSend();
}

bool Session::Connect()
{
	return RegisterConnect();
}

void Session::Disconnect(const WCHAR* cause)
{
	if (_connected.exchange(false) == false)
		return;

	// TEMP
	LOG_LINE << "Disconnect : " << ToNarrow(cause) << '\n';

	RegisterDisconnect();
}

HANDLE Session::GetHandle()
{
	return reinterpret_cast<HANDLE>(_socket);
}

void Session::Dispatch(IocpEvent* iocpEvent, int32 numOfBytes)
{
	switch (iocpEvent->eventType)
	{
	case EventType::Connect:
		ProcessConnect();
		break;
	case EventType::Disconnect:
		ProcessDisconnect();
		break;
	case EventType::Recv:
		ProcessRecv(numOfBytes);
		break;
	case EventType::Send:
		ProcessSend(numOfBytes);
		break;
	default:
		break;
	}
}

bool Session::RegisterConnect()
{
	if (IsConnected())
		return false;

	if (GetService()->GetServiceType() != ServiceType::Client)
		return false;

	if (SocketUtils::SetReuseAddress(_socket, true) == false)
		return false;

	if (SocketUtils::BindAnyAddress(_socket, 0/*남는거*/) == false)
		return false;

	_connectEvent.Init();
	_connectEvent.owner = shared_from_this(); // ADD_REF

	DWORD numOfBytes = 0;
	SOCKADDR_IN sockAddr = GetService()->GetNetAddress().GetSockAddr();
	if (false == SocketUtils::ConnectEx(_socket, reinterpret_cast<SOCKADDR*>(&sockAddr), sizeof(sockAddr), nullptr, 0, &numOfBytes, &_connectEvent))
	{
		int32 errorCode = ::WSAGetLastError();
		if (errorCode != WSA_IO_PENDING)
		{
			_connectEvent.owner = nullptr; // RELEASE_REF
			return false;
		}
	}

	return true;
}

bool Session::RegisterDisconnect()
{
	_disconnectEvent.Init();
	_disconnectEvent.owner = shared_from_this(); // ADD_REF

	if (false == SocketUtils::DisconnectEx(_socket, &_disconnectEvent, TF_REUSE_SOCKET, 0))
	{
		int32 errorCode = ::WSAGetLastError();
		if (errorCode != WSA_IO_PENDING)
		{
			_disconnectEvent.owner = nullptr; // RELEASE_REF
			return false;
		}
	}

	return true;
}

void Session::RegisterRecv()
{
	if (IsConnected() == false)
		return;

	_recvEvent.Init();
	_recvEvent.owner = shared_from_this(); // ADD_REF

	WSABUF wsaBuf;
	wsaBuf.buf = reinterpret_cast<char*>(_recvBuffer.WritePos());
	wsaBuf.len = _recvBuffer.FreeSize();

	DWORD numOfBytes = 0;
	DWORD flags = 0;
	if (SOCKET_ERROR == ::WSARecv(_socket, &wsaBuf, 1, OUT &numOfBytes, OUT &flags, &_recvEvent, nullptr))
	{
		int32 errorCode = ::WSAGetLastError();
		if (errorCode != WSA_IO_PENDING)
		{
			HandleError(errorCode);
			// HandleError 가 끊지 않는 오류(WSAENETRESET · WSAENOTCONN …)면 수신이 다시 걸리지 않은 채 "연결됨" 으로 남는다. 이미 끊었으면 아무것도 안 한다
			Disconnect(L"RegisterRecv");
			_recvEvent.owner = nullptr; // RELEASE_REF
		}
	}
}

void Session::RegisterSend()
{
	// 🔴 호출자는 _sendRegistered 를 true 로 잡은 한 스레드다 (Send · ProcessSend) - _sendEvent 는 이 스레드만 만진다.
	// 락은 큐를 비우는 동안만 잡는다. WSASend 는 락 밖 (TD-007)
	if (IsConnected() == false)
	{
		_sendRegistered.store(false);
		return;
	}

	_sendEvent.Init();
	_sendEvent.owner = shared_from_this(); // ADD_REF

	// 보낼 데이터를 sendEvent에 등록 - 옮긴다 (참조 카운트를 올렸다 내리지 않는다)
	{
		WRITE_LOCK;
		while (_sendQueue.empty() == false)
		{
			_sendEvent.sendBuffers.push_back(std::move(_sendQueue.front()));
			_sendQueue.pop();
		}
	}

	// Scatter-Gather (흩어져 있는 데이터들을 모아서 한 방에 보낸다)
	vector<WSABUF> wsaBufs;
	wsaBufs.reserve(_sendEvent.sendBuffers.size());
	for (const SendBufferRef& sendBuffer : _sendEvent.sendBuffers)
	{
		WSABUF wsaBuf;
		wsaBuf.buf = reinterpret_cast<char*>(sendBuffer->Buffer());
		wsaBuf.len = static_cast<LONG>(sendBuffer->WriteSize());
		wsaBufs.push_back(wsaBuf);
	}

	DWORD numOfBytes = 0;
	if (SOCKET_ERROR == ::WSASend(_socket, wsaBufs.data(), static_cast<DWORD>(wsaBufs.size()), OUT &numOfBytes, 0, &_sendEvent, nullptr))
	{
		int32 errorCode = ::WSAGetLastError();
		if (errorCode != WSA_IO_PENDING)
		{
			HandleError(errorCode);
			_sendEvent.owner = nullptr; // RELEASE_REF
			_sendEvent.sendBuffers.clear(); // RELEASE_REF
			_sendRegistered.store(false);
		}
	}
}

void Session::ProcessConnect()
{
	_connectEvent.owner = nullptr; // RELEASE_REF

	_connected.store(true);

	// 세션 등록
	GetService()->AddSession(GetSessionRef());

	// 컨텐츠 코드에서 재정의
	OnConnected();

	// 수신 등록
	RegisterRecv();
}

void Session::ProcessDisconnect()
{
	_disconnectEvent.owner = nullptr; // RELEASE_REF

	OnDisconnected(); // 컨텐츠 코드에서 재정의
	GetService()->ReleaseSession(GetSessionRef());
}

void Session::ProcessRecv(int32 numOfBytes)
{
	_recvEvent.owner = nullptr; // RELEASE_REF

	if (numOfBytes == 0)
	{
		Disconnect(L"Recv 0");
		return;
	}
	GStats.AddRecvBytes(numOfBytes);

	if (_recvBuffer.OnWrite(numOfBytes) == false)
	{
		Disconnect(L"OnWrite Overflow");
		return;
	}

	int32 dataSize = _recvBuffer.DataSize();
	int32 processLen = OnRecv(_recvBuffer.ReadPos(), dataSize); // 컨텐츠 코드에서 재정의
	if (processLen < 0 || dataSize < processLen || _recvBuffer.OnRead(processLen) == false)
	{
		Disconnect(L"OnRead Overflow");
		return;
	}
	
	// 커서 정리
	_recvBuffer.Clean();

	// 수신 등록
	RegisterRecv();
}

void Session::ProcessSend(int32 numOfBytes)
{
	_sendEvent.owner = nullptr; // RELEASE_REF
	int64 completedBytes = 0;
	for (const SendBufferRef& sendBuffer : _sendEvent.sendBuffers)
		completedBytes += sendBuffer->WriteSize();
	_sendEvent.sendBuffers.clear(); // RELEASE_REF

	if (numOfBytes == 0)
	{
		Disconnect(L"Send 0");
		return;
	}
	GStats.AddSentBytes(numOfBytes);

	// 컨텐츠 코드에서 재정의
	OnSend(numOfBytes);

	bool more = false;
	{
		WRITE_LOCK;

		_pendingSendBytes -= completedBytes;		// 보낸 만큼 내린다 (Send 가 올렸다)

		// 비었으면 깃발을 락 안에서 내린다 - 락 밖에서 내리면 그 사이 Send 가 넣은 것을 아무도 안 보낸다
		if (_sendQueue.empty())
			_sendRegistered.store(false);
		else
			more = true;
	}

	if (more)
		RegisterSend();
}

void Session::HandleError(int32 errorCode)
{
	switch (errorCode)
	{
	case WSAECONNRESET:
	case WSAECONNABORTED:
		Disconnect(L"HandleError");
		break;
	default:
		// TODO : Log
		LOG_LINE << "Handle Error : " << errorCode << '\n';
		break;
	}
}

/*-----------------
	PacketSession
------------------*/

PacketSession::PacketSession()
{
}

PacketSession::~PacketSession()
{
}

// [size(2)][id(2)][data....][size(2)][id(2)][data....]
int32 PacketSession::OnRecv(BYTE* buffer, int32 len)
{
	int32 processLen = 0;

	while (true)
	{
		int32 dataSize = len - processLen;
		// 최소한 헤더는 파싱할 수 있어야 한다
		if (dataSize < sizeof(PacketHeader))
			break;

		PacketHeader header = *(reinterpret_cast<PacketHeader*>(&buffer[processLen]));
		// 헤더보다 작은 크기는 잘못된 패킷이다 - 0 이면 processLen 이 늘지 않아 IOCP 스레드가 영원히 돈다. -1 → ProcessRecv 가 끊는다
		if (header.size < sizeof(PacketHeader))
			return -1;

		// 헤더에 기록된 패킷 크기를 파싱할 수 있어야 한다
		if (dataSize < header.size)
			break;

		// 패킷 조립 성공
		OnRecvPacket(&buffer[processLen], header.size);

		processLen += header.size;
	}

	return processLen;
}
