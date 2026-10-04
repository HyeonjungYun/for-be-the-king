#pragma once
#include <stack>

extern thread_local uint32				LThreadId;
extern thread_local uint64				LEndTickCount;

extern thread_local class JobQueue* LCurrentJobQueue;

// 지금 실행 중인 잡의 이름 - 잡 안에서 JOB_TAG() 로 처음 한 번 적힌다. 느린 잡 로그용 (JobQueue::Execute)
extern thread_local const char*			LJobTag;