#pragma once

/*--------------
	LockOrder
---------------*/

// 락 순서 검사 (2026-10-21 리뷰 후속). 매니저 락의 순서 규약 - "파티(10) → 등록부(20): 세션 관리자 · 인스턴스 · 계정 · 사망 커밋 · 끊긴 몸" -
// 은 PartyManager.h 의 주석으로만 지켜지고 있었다. 이제 코드가 본다:
//   수준은 "먼저 잡아야 하는 쪽이 작다". 한 스레드가 수준 L 을 쥔 채 L 이하를 잡으면 위반이다 (역순 · 같은 수준 재진입 둘 다 교착 후보).
//   디버그 빌드는 그 자리에서 크래시, 릴리스는 지점마다 한 번 [ASSERT] 줄 (ASSERT_DEBUG). 비용은 thread_local 스택 push / pop.
// 수준을 주지 않은 락(WRITE_LOCK 그대로 - Session · Listener · Service · 큐)은 검사하지 않는다 - 잎 락이라 다른 락을 안에서 잡지 않는다.
class LockOrder
{
public:
	using ViolationFn = void (*)(int32 heldLevel, int32 wantedLevel, const char* where);

	static void		Acquire(int32 level, const char* where);
	static void		Release(int32 level);

	// 이 스레드가 지금 쥔 순서 있는 락의 수 · 가장 최근 수준 (0 = 없음)
	static int32	Depth();
	static int32	TopLevel();

	// 테스트 - 위반을 크래시 · 로그 대신 넘겨받는다. nullptr 면 기본(ASSERT_DEBUG)
	static void		SetViolationHandler(ViolationFn fn);
};

class LockOrderGuard
{
public:
	LockOrderGuard(int32 level, const char* where) : _level(level) { LockOrder::Acquire(level, where); }
	~LockOrderGuard() { LockOrder::Release(_level); }

	LockOrderGuard(const LockOrderGuard&) = delete;
	LockOrderGuard& operator=(const LockOrderGuard&) = delete;

private:
	int32 _level;
};

// USE_LOCK 을 가진 클래스에서 - 순서 검사 + 잠금. 검사 가드를 먼저 만들고 뮤텍스를 잡는다 (해제는 역순)
#define WRITE_LOCK_ORDERED(level)	LockOrderGuard lockOrderGuard_(level, __FUNCTION__); WRITE_LOCK
