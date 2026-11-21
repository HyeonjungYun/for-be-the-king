#include "pch.h"
#include "GlobalQueue.h"

/*----------------
	GlobalQueue
-----------------*/

GlobalQueue::GlobalQueue()
{

}

GlobalQueue::~GlobalQueue()
{

}

void GlobalQueue::Push(JobQueueRef jobQueue)
{
	_jobQueues.Push(std::move(jobQueue));
	if (_waker)
		_waker();
}

JobQueueRef GlobalQueue::Pop()
{
	return _jobQueues.Pop();
}
