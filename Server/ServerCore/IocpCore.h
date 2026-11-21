#pragma once

/*----------------
	IocpObject
-----------------*/

class IocpObject : public enable_shared_from_this<IocpObject>
{
public:
	virtual ~IocpObject() = default;
	virtual HANDLE GetHandle() = 0;
	virtual void Dispatch(class IocpEvent* iocpEvent, int32 numOfBytes = 0) = 0;
};

/*--------------
	IocpCore
---------------*/

class IocpCore
{
public:
	IocpCore();
	~IocpCore();

	HANDLE		GetHandle() { return _iocpHandle; }

	bool		Register(IocpObjectRef iocpObject);
	bool		Dispatch(uint32 timeoutMs = INFINITE);

	// 일감 없이 워커 하나를 깨운다 - GlobalQueue 에 방 큐가 들어왔을 때 (2026-11-21 동접 1만). 예전엔 워커들이 10 ms 마다 깨어나 들여다봤다
	void		PostWake();

private:
	HANDLE		_iocpHandle;
};