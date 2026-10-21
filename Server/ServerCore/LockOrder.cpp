#include "pch.h"
#include "LockOrder.h"

/*--------------
	LockOrder
---------------*/

namespace
{
	thread_local vector<int32>	t_held;
	LockOrder::ViolationFn		s_onViolation = nullptr;
}

void LockOrder::Acquire(int32 level, const char* where)
{
	if (t_held.empty() == false && t_held.back() >= level)
	{
		if (s_onViolation != nullptr)
		{
			s_onViolation(t_held.back(), level, where);
		}
		else
		{
			LogLine(true) << "[ASSERT] lock order - holding level " << t_held.back() << " while taking level " << level
				<< " in " << (where != nullptr ? where : "?") << '\n';
			ASSERT_DEBUG(false, "lock order violated (see the line above)");
		}
	}

	t_held.push_back(level);
}

void LockOrder::Release(int32 level)
{
	if (t_held.empty())
		return;

	// 보통 맨 위다. 아니면(가드 수명이 겹친 경우) 가장 최근의 같은 수준을 뺀다
	if (t_held.back() == level)
	{
		t_held.pop_back();
		return;
	}

	for (auto it = t_held.rbegin(); it != t_held.rend(); ++it)
	{
		if (*it == level)
		{
			t_held.erase(next(it).base());
			return;
		}
	}
}

int32 LockOrder::Depth()
{
	return static_cast<int32>(t_held.size());
}

int32 LockOrder::TopLevel()
{
	return t_held.empty() ? 0 : t_held.back();
}

void LockOrder::SetViolationHandler(ViolationFn fn)
{
	s_onViolation = fn;
}
