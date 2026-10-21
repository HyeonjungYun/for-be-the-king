#pragma once
#include "IocpCore.h"
#include "NetAddress.h"

class AcceptEvent;
class ServerService;

/*--------------
	Listener
---------------*/

class Listener : public IocpObject
{
public:
	Listener() = default;
	~Listener();

public:
	/* 외부에서 사용 */
	bool StartAccept(ServerServiceRef service);
	void CloseSocket();

	// RegisterAccept 가 포기한 슬롯을 다시 건다 (TD-014, 2026-10-18). 서버 주 루프가 10 초마다 부른다. 돌려주는 값 = 다시 건 슬롯 수
	int32 RetryLostAccepts();
	int32 LostAcceptCount();

public:
	/* 인터페이스 구현 */
	virtual HANDLE GetHandle() override;
	virtual void Dispatch(class IocpEvent* iocpEvent, int32 numOfBytes = 0) override;

private:
	/* 수신 관련 */
	void RegisterAccept(AcceptEvent* acceptEvent);
	void ProcessAccept(AcceptEvent* acceptEvent);

protected:
	SOCKET _socket = INVALID_SOCKET;
	vector<AcceptEvent*> _acceptEvents;
	ServerServiceRef _service;

	USE_LOCK;
	vector<AcceptEvent*> _lostAccepts;		// 8 번 연속 실패한 슬롯 - 예전엔 영영 잃었다 (자원 고갈이 풀리면 다시 건다)

	// CloseSocket 뒤 - 걸려 있던 AcceptEx 가 전부 실패로 돌아온다. 그때는 다시 걸지 않는다 (2026-10-21 검증 툴 스모크:
	// Ctrl+C 종료마다 슬롯 수 × "[ACCEPT] accept slot failed 8 times" 100 줄 + 닫힌 소켓에 세션 800 개를 만들었다)
	atomic<bool> _closed = false;
};

