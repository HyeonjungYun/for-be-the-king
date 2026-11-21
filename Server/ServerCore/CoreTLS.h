#pragma once
#include <vector>
#include <memory>

extern thread_local uint32				LThreadId;
extern thread_local uint64				LEndTickCount;

extern thread_local class JobQueue* LCurrentJobQueue;

// 지금 실행 중인 잡의 이름 - 잡 안에서 JOB_TAG() 로 처음 한 번 적힌다. 느린 잡 로그용 (JobQueue::Execute)
extern thread_local const char*			LJobTag;

// 방 잡 묶음 안에서 생긴 송신 등록을 모았다가 묶음이 끝날 때 한 번에 WSASend (2026-11-21 동접 1만 - docs/server-scaling-10k.md § 2-4).
// JobQueue::Execute 가 잡을 돌리는 동안만 가리킨다. null 이면 Session::Send 가 예전처럼 바로 보낸다
extern thread_local std::vector<std::shared_ptr<class Session>>* LSendBatch;
