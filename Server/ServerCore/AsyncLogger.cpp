#include "pch.h"
#include "AsyncLogger.h"

/*----------------
	AsyncLogger
-----------------*/

AsyncLogger::AsyncLogger(Sink sink, size_t maxPending)
	: _sink(sink ? std::move(sink) : Sink(&AsyncLogger::WriteToStd)), _maxPending(maxPending)
{
	_thread = std::thread([this]() { Run(); });
}

AsyncLogger::~AsyncLogger()
{
	{
		std::lock_guard<std::mutex> guard(_lock);
		_stop = true;
	}

	_wake.notify_all();

	if (_thread.joinable())
		_thread.join();
}

void AsyncLogger::Push(std::string&& text, bool toStderr)
{
	{
		std::lock_guard<std::mutex> guard(_lock);

		if (_pending.size() >= _maxPending)
		{
			_dropped.fetch_add(1);
			return;
		}

		_pending.push_back(Entry{ std::move(text), toStderr });
		_accepted++;
	}

	_wake.notify_one();
}

void AsyncLogger::Flush()
{
	std::unique_lock<std::mutex> guard(_lock);
	const uint64 target = _accepted;
	_drained.wait(guard, [this, target]() { return _written >= target; });
}

void AsyncLogger::Run()
{
	std::deque<Entry> batch;

	while (true)
	{
		{
			std::unique_lock<std::mutex> guard(_lock);
			_wake.wait(guard, [this]() { return _stop || _pending.empty() == false; });

			if (_pending.empty() && _stop)
				return;

			batch.swap(_pending);
		}

		// 대상별로 이어 붙여 한 번에 쓴다 - 줄마다 쓰기를 부르면 그만큼 느리고, 이 스레드가 밀리면 큐가 찬다
		std::string out;
		std::string err;
		for (Entry& entry : batch)
			(entry.toStderr ? err : out) += entry.text;

		// 검증 전용 막힘 (SetTestStall). 이 스레드만 멈추고 큐는 계속 받는다
		if (const uint32 stallMs = _stallMs.load(); stallMs > 0)
		{
			const uint64 nowTick = ::GetTickCount64();
			if (nowTick >= _nextStallTick)
			{
				_nextStallTick = nowTick + _stallPeriodMs.load();
				std::this_thread::sleep_for(std::chrono::milliseconds(stallMs));
			}
		}

		const uint64 dropped = _dropped.load();
		if (dropped != _droppedReported)
		{
			out += "[LOG] dropped " + std::to_string(dropped - _droppedReported) + " line(s) - queue full\n";
			_droppedReported = dropped;
		}

		if (out.empty() == false)
			_sink(out, false);
		if (err.empty() == false)
			_sink(err, true);

		{
			std::lock_guard<std::mutex> guard(_lock);
			_written += batch.size();
		}

		_drained.notify_all();
		batch.clear();
	}
}

void AsyncLogger::WriteToStd(const std::string& text, bool toStderr)
{
	FILE* stream = toStderr ? stderr : stdout;
	::fwrite(text.data(), 1, text.size(), stream);
	::fflush(stream);
}

AsyncLogger& GetLogger()
{
	// 일부러 해제하지 않는다 - 종료 중에 다른 전역 객체의 소멸자가 로그를 찍어도 죽은 객체를 건드리지 않게.
	// 남은 줄은 atexit 에서 비운다(그때는 쓰기 스레드가 아직 살아 있다).
	static AsyncLogger* logger = []()
	{
		AsyncLogger* created = new AsyncLogger();
		std::atexit([]() { GetLogger().Flush(); });
		return created;
	}();

	return *logger;
}
