#pragma once
#include <functional>

/*---------
	Job
----------*/

using CallbackType = std::function<void()>;

class Job
{
public:
	Job(CallbackType&& callback) : _callback(std::move(callback))
	{
	}

	// 잡은 한 번만 실행된다 - 인자를 람다로 옮기고(복사 1) 호출 때 다시 옮긴다(복사 2 를 없앤다). C_MOVE 같은 protobuf 인자가 매 패킷 두 번 복사됐다
	template<typename T, typename Ret, typename... Args>
	Job(shared_ptr<T> owner, Ret(T::* memFunc)(Args...), Args&&... args)
		: _callback([owner = std::move(owner), memFunc, ...args = std::forward<Args>(args)]() mutable
			{
				(owner.get()->*memFunc)(std::move(args)...);
			})
	{
	}

	void Execute()
	{
		_callback();
	}

private:
	CallbackType _callback;
};

