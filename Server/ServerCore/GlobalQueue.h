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

private:
	LockQueue<JobQueueRef> _jobQueues;
};

