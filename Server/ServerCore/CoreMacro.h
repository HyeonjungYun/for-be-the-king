#pragma once

#define OUT

#define NAMESPACE_BEGIN(name)	namespace name {
#define NAMESPACE_END			}

/*---------------
	  Lock
---------------*/

/*---------------
	  Job tag
---------------*/

// 잡으로 도는 함수 첫 줄에 둔다. 처음 적힌 이름만 남는다 - 잡 안에서 부른 다른 태그 함수가 덮어쓰지 않게
#define JOB_TAG()				do { if (LJobTag == nullptr) LJobTag = __FUNCTION__; } while (0)

#define USE_MANY_LOCKS(count)	mutex _locks[count];
#define USE_LOCK				USE_MANY_LOCKS(1)
#define	WRITE_LOCK_IDX(idx)		lock_guard<mutex> lockGuard_##idx(_locks[idx]);
#define WRITE_LOCK				WRITE_LOCK_IDX(0)

/*---------------
	  Crash
---------------*/

#define CRASH(cause)						\
{											\
	uint32* crash = nullptr;				\
	__analysis_assume(crash != nullptr);	\
	*crash = 0xDEADBEEF;					\
}

#define ASSERT_CRASH(expr)			\
{									\
	if (!(expr))					\
	{								\
		CRASH("ASSERT_CRASH");		\
		__analysis_assume(expr);	\
	}								\
}

// 불변식 검사 - 디버그 빌드는 그 자리에서 크래시, 릴리스 빌드는 호출 지점마다 처음 한 번만 표준 오류에 남기고 계속 간다.
// "오늘은 모든 호출자가 지킨다" 로만 지켜지던 규약(예: Room::DealDamage 는 KillBatch 가 열린 동안만)을 코드로 적는 용도 (2026-10-21)
#ifdef _DEBUG
#define ASSERT_DEBUG(expr, msg)		ASSERT_CRASH(expr)
#else
#define ASSERT_DEBUG(expr, msg)																						\
	do																												\
	{																												\
		if (!(expr))																								\
		{																											\
			static std::atomic<bool> s_assertReported{ false };														\
			if (s_assertReported.exchange(true) == false)															\
				LogLine(true) << "[ASSERT] " << msg << " - " << __FUNCTION__ << " (" << __FILE__ << ":" << __LINE__ << ")\n";	\
		}																											\
	} while (0)
#endif