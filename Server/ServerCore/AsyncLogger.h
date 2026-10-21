#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>

/*----------------
	AsyncLogger
-----------------*/

// 게임 스레드는 로그를 큐에 넣기만 하고, 전용 스레드 하나가 모아서 쓴다.
//
// 🔴 왜: 예전 LOG_LINE(osyncstream(cout))은 모든 스레드가 표준 출력 하나를 잠그고 그 자리에서 파일에 썼다.
//    쓰기 하나가 OS 에서 늦어지면 로그를 찍는 Room 잡이 전부 그 뒤에 섰다 - 1.7초 정지 · SV-1 max FAIL ·
//    밀린 이동 처리로 SV-3 오탐 400건 (roadmap.md § 0-11, 2026-10-04).
//
// 대가:
//  - 크래시 직전, 아직 쓰지 못한 몇 ms 분량의 로그는 잃는다.
//  - 큐가 상한(maxPending)을 넘으면 그 줄은 버리고 개수만 남긴다 - 디스크가 계속 막혀도 메모리가 무한히 늘지 않게.
//  - 줄의 순서는 Push 순서다. 한 문장(LogLine 하나)은 섞이지 않고 한 덩어리로 나간다.
class AsyncLogger
{
public:
	using Sink = std::function<void(const std::string& text, bool toStderr)>;

	static constexpr size_t DEFAULT_MAX_PENDING = 100'000;

	// sink 가 없으면 표준 출력 · 표준 오류에 쓴다. 테스트는 sink 를 넣어 받은 것을 확인한다
	explicit AsyncLogger(Sink sink = nullptr, size_t maxPending = DEFAULT_MAX_PENDING);
	~AsyncLogger();

	AsyncLogger(const AsyncLogger&) = delete;
	AsyncLogger& operator=(const AsyncLogger&) = delete;

	void		Push(std::string&& text, bool toStderr = false);

	// 지금까지 받아 둔 줄이 전부 sink 에 넘어갈 때까지 기다린다 (테스트 · 종료용)
	void		Flush();

	uint64		GetDroppedCount() const { return _dropped.load(); }

	// 검증 전용 - 쓰기 스레드가 periodMs 마다 stallMs 동안 멈춘다(디스크가 막힌 상황 흉내).
	// 넣는 쪽(Room 잡)이 그동안 멈추지 않는지 보는 데 쓴다 (FBTK_LOG_STALL_MS, roadmap.md § 0-11)
	void		SetTestStall(uint32 stallMs, uint32 periodMs) { _stallMs.store(stallMs); _stallPeriodMs.store(periodMs); }

private:
	void		Run();
	static void	WriteToStd(const std::string& text, bool toStderr);

	struct Entry
	{
		std::string	text;
		bool		toStderr = false;
	};

	Sink					_sink;
	size_t					_maxPending;

	std::mutex				_lock;
	std::condition_variable	_wake;
	std::condition_variable	_drained;
	std::deque<Entry>		_pending;
	uint64					_accepted = 0;		// _lock 아래
	uint64					_written = 0;		// _lock 아래
	bool					_stop = false;		// _lock 아래

	std::atomic<uint64>		_dropped = 0;
	uint64					_droppedReported = 0;	// 쓰기 스레드 전용

	std::atomic<uint32>		_stallMs = 0;
	std::atomic<uint32>		_stallPeriodMs = 0;
	uint64					_nextStallTick = 0;		// 쓰기 스레드 전용

	std::thread				_thread;
};

// 처음 쓸 때 만든다 - 다른 전역 객체의 생성자에서 로그를 찍어도 순서 문제가 없게
AsyncLogger& GetLogger();

/*------------
	LogLine
-------------*/

// LOG_LINE << a << b << '\n';  - 문장 하나가 한 덩어리로 큐에 들어간다.
// 여러 문장으로 한 이벤트를 찍을 때는 객체를 직접 둔다: LogLine out; out << ...; (블록이 끝날 때 나간다)
//
// 머리 (2026-10-21): "2026-10-21 14:03:22.123 I 05 " = 날짜 시각.ms · 수준(I 정보 · E 표준 오류) · 스레드 번호(LThreadId, 2자리).
// 예전엔 시각도 수준도 스레드도 없어 "언제 · 어느 워커에서" 를 알 수 없었다. 한 LogLine 이 여러 줄이면 머리는 첫 줄에만 붙는다.
// 줄 머리에 '[' 를 기대하는 스크립트는 '^\[' 대신 '\[' 로 (하네스 3개 고침)
class LogLine
{
public:
	explicit LogLine(bool toStderr = false) : _toStderr(toStderr) { WritePrefix(); }
	~LogLine() { GetLogger().Push(std::move(_stream).str(), _toStderr); }

	LogLine(const LogLine&) = delete;
	LogLine& operator=(const LogLine&) = delete;

	template<typename T>
	LogLine& operator<<(const T& value)
	{
		_stream << value;
		return *this;
	}

	// endl 같은 조작자
	LogLine& operator<<(std::ostream& (*manip)(std::ostream&))
	{
		_stream << manip;
		return *this;
	}

private:
	void				WritePrefix();

	std::ostringstream	_stream;
	bool				_toStderr;
};
