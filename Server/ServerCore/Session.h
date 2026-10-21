#pragma once
#include "IocpCore.h"
#include "IocpEvent.h"
#include "NetAddress.h"
#include "RecvBuffer.h"

class Service;

/*--------------
	Session
---------------*/

class Session : public IocpObject
{
	friend class Listener;
	friend class IocpCore;
	friend class Service;

	enum
	{
		BUFFER_SIZE = 0x10000, // 64KB
	};

	// 죽은 상대를 TCP 층에서 알아차리는 시간 - keepalive 를 10 s 쉬고 3 s 간격으로 보낸다 (Windows 는 10 번 보내고 끊는다 → 약 40 s).
	// 예전엔 keepalive 가 없어 선이 뽑힌 클라를 송신 상한(8 MB, 약 10 분)이 찰 때까지 몰랐다 (2026-10-21)
	static constexpr uint32	KEEPALIVE_IDLE_MS = 10'000;
	static constexpr uint32	KEEPALIVE_INTERVAL_MS = 3'000;

public:
	Session();
	virtual ~Session();

public:
						/* 외부에서 사용 */
	void				Send(SendBufferRef sendBuffer);
	bool				Connect();
	void				Disconnect(const WCHAR* cause);

	shared_ptr<Service>	GetService() { return _service.lock(); }
	void				SetService(shared_ptr<Service> service) { _service = service; }

public:
						/* 정보 관련 */
	void				SetNetAddress(NetAddress address) { _netAddress = address; }
	NetAddress			GetAddress() { return _netAddress; }
	SOCKET				GetSocket() { return _socket; }
	bool				IsConnected() { return _connected; }
	SessionRef			GetSessionRef() { return static_pointer_cast<Session>(shared_from_this()); }

private:
						/* 인터페이스 구현 */
	virtual HANDLE		GetHandle() override;
	virtual void		Dispatch(class IocpEvent* iocpEvent, int32 numOfBytes = 0) override;

private:
						/* 전송 관련 */
	bool				RegisterConnect();
	bool				RegisterDisconnect();
	void				RegisterRecv();
	void				RegisterSend();

	void				ProcessConnect();
	void				ProcessDisconnect();
	void				ProcessRecv(int32 numOfBytes);
	void				ProcessSend(int32 numOfBytes);

	void				HandleError(int32 errorCode);

protected:
						/* 컨텐츠 코드에서 재정의 */
	virtual void		OnConnected() { }
	virtual int32		OnRecv(BYTE* /*buffer*/, int32 len) { return len; }
	virtual void		OnSend(int32 /*len*/) { }
	virtual void		OnDisconnected() { }

private:
	weak_ptr<Service>	_service;
	SOCKET				_socket = INVALID_SOCKET;
	NetAddress			_netAddress = {};
	atomic<bool>		_connected = false;

private:
	USE_LOCK;
							/* 수신 관련 */
	RecvBuffer				_recvBuffer;

							/* 송신 관련 */
	queue<SendBufferRef>	_sendQueue;
	atomic<bool>			_sendRegistered = false;

	// 큐에 쌓였거나 보내는 중인 바이트 (락 안에서만 바꾼다). 소켓을 읽지 않는 클라에게 방송이 끝없이 쌓여 메모리가 자랐다 -
	// 넘으면 끊는다 (2026-10-17 보안 점검 H3). 정상 클라는 초당 수십 KB 라 8 MB 는 몇 분을 멈춰 있어야 찬다
	static constexpr int64	MAX_PENDING_SEND_BYTES = 8 * 1024 * 1024;
	int64					_pendingSendBytes = 0;

private:
						/* IocpEvent 재사용 */
	ConnectEvent		_connectEvent;
	DisconnectEvent		_disconnectEvent;
	RecvEvent			_recvEvent;
	SendEvent			_sendEvent;
};

/*-----------------
	PacketSession
------------------*/

struct PacketHeader
{
	uint16 size;
	uint16 id; // 프로토콜ID (ex. 1=로그인, 2=이동요청)
};

class PacketSession : public Session
{
public:
	PacketSession();
	virtual ~PacketSession();

	PacketSessionRef	GetPacketSessionRef() { return static_pointer_cast<PacketSession>(shared_from_this()); }

protected:
	virtual int32		OnRecv(BYTE* buffer, int32 len) final;
	virtual void		OnRecvPacket(BYTE* buffer, int32 len) = 0;
};