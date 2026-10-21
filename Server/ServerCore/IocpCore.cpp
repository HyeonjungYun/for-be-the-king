#include "pch.h"
#include "IocpCore.h"
#include "IocpEvent.h"

/*--------------
	IocpCore
---------------*/

IocpCore::IocpCore()
{
	_iocpHandle = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 0);
	ASSERT_CRASH(_iocpHandle != NULL);		// CreateIoCompletionPort 는 실패하면 NULL 이다 (INVALID_HANDLE_VALUE 가 아니다)
}

IocpCore::~IocpCore()
{
	::CloseHandle(_iocpHandle);
}

bool IocpCore::Register(IocpObjectRef iocpObject)
{
	return ::CreateIoCompletionPort(iocpObject->GetHandle(), _iocpHandle, /*key*/0, 0);
}

bool IocpCore::Dispatch(uint32 timeoutMs)
{
	DWORD numOfBytes = 0;
	ULONG_PTR key = 0;	
	IocpEvent* iocpEvent = nullptr;

	if (::GetQueuedCompletionStatus(_iocpHandle, OUT &numOfBytes, OUT &key, OUT reinterpret_cast<LPOVERLAPPED*>(&iocpEvent), timeoutMs))
	{
		iocpEvent->errorCode = 0;
		IocpObjectRef iocpObject = iocpEvent->owner;
		iocpObject->Dispatch(iocpEvent, numOfBytes);
	}
	else
	{
		int32 errCode = ::WSAGetLastError();

		if (iocpEvent == nullptr)
		{
			if (errCode != WAIT_TIMEOUT)
				LOG_LINE << "[IOCP] dispatch failed without event - err=" << errCode << '\n';

			return false;
		}
		if (errCode == WAIT_TIMEOUT)
			return false;

		// 실패한 완료 - 이벤트에 오류를 실어 보낸다. Recv · Send 는 numOfBytes 0 으로 끊기고, Connect 는 ProcessConnect 가 이 값을 본다
		iocpEvent->errorCode = errCode;
		IocpObjectRef iocpObject = iocpEvent->owner;
		iocpObject->Dispatch(iocpEvent, numOfBytes);
	}

	return true;
}
