#pragma once

/*----------------
	GlobalQueue
-----------------*/

class GlobalQueue
{
public:
	GlobalQueue();
	~GlobalQueue();

	void					Push(JobQueueRef jobQueue);
	JobQueueRef				Pop();
	size_t					Size() { return _jobQueues.Size(); }		// 종료 때 "남은 일이 있나" (2026-10-21)

	// 방 큐가 들어올 때마다 부른다 - 서버가 IOCP 워커 하나를 깨우는 함수를 넣는다 (2026-11-21). 없으면 예전처럼 워커가 들여다볼 때까지
	void					SetWaker(function<void()> waker) { _waker = std::move(waker); }

private:
	LockQueue<JobQueueRef> _jobQueues;
	function<void()>		_waker;		// 부팅 때 한 번 - 이후 읽기만
};

