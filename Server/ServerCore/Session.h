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

	// 이 프로세스에서 새로 만드는 세션의 수신 버퍼 처음 크기 = bufferSize × bufferCount (2026-11-21 동접 1만).
	// 기본은 예전 그대로 64 KB × 10. 모자라면 RecvBuffer 가 키우고 비면 되돌린다. 세션을 만들기 전(부팅)에 한 번 부른다
	static void			ConfigureRecvBuffer(int32 bufferSize, int32 bufferCount);

	// FILE_SKIP_COMPLETION_PORT_ON_SUCCESS (2026-11-21 동접 1만 - docs/server-scaling-10k.md § 2-6). 켜면 그 자리에서 끝난
	// WSASend · WSARecv · DisconnectEx 는 IOCP 완료를 만들지 않고 부른 스레드가 바로 마무리한다 - 루프백에서는 거의 모든 송신이 그렇다.
	// 커널 왕복(완료 넣기 → GQCS 로 꺼내기)이 I/O 마다 하나 준다. 기본 켬. 레이어드 LSP 가 있는 옛 환경이면 끈다 (FBTK_IOCP_SKIP=0)
	static void			SetSkipCompletionOnSuccess(bool on) { s_skipCompletion = on; }

	// 거는 쪽(클라 · 봇) 소켓의 출발 주소 - 비면 아무 주소(예전 그대로). 한 PC 부하 시험에서 봇 프로세스마다 127.0.0.N 을 다르게 줘
	// 직전 시험의 TIME_WAIT(같은 주소 · 포트 쌍, Windows 기본 120 s)에 새 접속이 걸려 멈추지 않게 한다 (2026-11-22)
	static void			SetConnectBindAddress(const wstring& ip) { s_connectBindIp = ip; }
	static int32		RecvBufferInitialBytes() { return s_recvBufferSize * s_recvBufferCount; }

public:
						/* 외부에서 사용 */
	void				Send(SendBufferRef sendBuffer);
	// LSendBatch 로 미뤄 둔 송신 등록을 지금 건다 (JobQueue::Execute 가 잡 묶음 끝에). Send 가 _sendRegistered 를 잡아 둔 상태에서만 부른다
	void				FlushDeferredSend() { RegisterSend(); }
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
	enum class DisconnectStart : uint8 { Pending, Done, Failed };
	DisconnectStart		RegisterDisconnect();
	void				RegisterRecv();
	void				RegisterSend();

	void				ProcessConnect();
	void				ProcessDisconnect();
	void				ProcessRecv(int32 numOfBytes);
	void				ProcessSend(int32 numOfBytes);

	// 완료 하나를 마무리한다 - IOCP 완료(Process*)와 그 자리 완료(Register* 안 루프)가 같이 쓴다. true = 다시 걸어야 한다
	bool				CompleteRecv(int32 numOfBytes);
	bool				CompleteSend(int32 numOfBytes);

	void				HandleError(int32 errorCode);

protected:
						/* 컨텐츠 코드에서 재정의 */
	virtual void		OnConnected() { }
	virtual int32		OnRecv(BYTE* /*buffer*/, int32 len) { return len; }
	virtual void		OnSend(int32 /*len*/) { }
	virtual void		OnDisconnected() { }

private:
	static inline bool	s_skipCompletion = true;
	static inline wstring	s_connectBindIp;
	bool				_skipCompletion = false;		// 이 소켓에 켜졌다 (ProcessConnect)
	static inline int32	s_recvBufferSize = BUFFER_SIZE;
	static inline int32	s_recvBufferCount = 10;

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