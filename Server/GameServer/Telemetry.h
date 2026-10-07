#pragma once
#include <mutex>
#include <fstream>
#include <condition_variable>
#include <deque>
#include <thread>

/*--------------
	Telemetry
--------------*/

// 런 단위 텔레메트리 (design/gdd/escape-timer.md § G). CSV 두 개 - 서버 시작마다 새 파일.
//   runs-<boot>.csv     한 행 = 한 런 (탈출 · 만료 · 사망 · 끊김 사망) - 22 열
//   gauge-<boot>.csv    타워 게이지 상태 전이
// 방 4개가 각자 스레드에서 부른다. 줄은 부른 쪽이 만들고, 파일 쓰기는 전용 스레드 하나가 한다 (2026-10-07) -
// 예전에는 Room 잡 안에서 ofstream 에 쓰고 flush 했다. 디스크가 늦으면 그 방 전체가 멈추는, G2 동기 로그와 같은 길이었다.
// 쓰는 스레드가 하나라 깨진 줄이 없다 (AC-ET-16).

struct RunRow
{
	uint64	charId = 0;
	uint32	floorId = 0;
	uint32	entryPoint = 0;
	string	reason;					// ESCAPED · EXPIRED · DIED · DISCONNECT_DEATH
	uint64	runMs = 0;
	uint32	broughtInSum = 0;
	uint32	endCarriedSum = 0;
	uint32	endCarriedCount = 0;
	uint32	endCarriedMaxGrade = 0;
	uint64	runGold = 0;
	uint64	goldKept = 0;
	uint32	zoneVisits = 0;
	uint32	zoneLeavesVoluntary = 0;
	uint32	zoneLeavesForced = 0;
	uint64	zoneDwellMs = 0;
	uint32	gaugeReversalsPresent = 0;
	uint64	alertDwellMs = 0;
	string	lastTowerId;
	uint64	killerId = 0;
};

class Telemetry
{
public:
	static const char*	RUNS_HEADER;
	static const char*	GAUGE_HEADER;

						~Telemetry();

	// 디렉터리를 만들고 두 파일을 연 뒤 쓰기 스레드를 띄운다. 실패하면 기록만 끄고 서버는 돈다(측정 전용 - ADVISORY)
	bool			Open(const string& directory);

	// 줄을 만들어 큐에 넣고 바로 돌아온다 - 파일을 기다리지 않는다
	void			AppendRun(const RunRow& row);
	void			AppendGauge(uint32 floorId, const string& towerId, const char* event, uint32 permille, uint32 occupants);

	// 열 순서 그대로 한 줄 (테스트가 열 수를 센다)
	static string	FormatRun(uint64 tsMs, uint64 bootId, uint64 runSeq, const RunRow& row);

	uint64			BootId() const { return _bootId; }

	// 테스트 전용 - 쓰기 스레드가 묶음마다 이만큼 멈춘다 (느린 디스크 흉내)
	void			SetWriteStallForTest(uint32 ms) { _stallMs.store(ms); }

private:
	enum class Sink : uint8 { Runs, Gauge };

	void			Enqueue(Sink sink, string line);
	void			WriterLoop();

	mutex			_lock;				// _bootId · _runSeq
	uint64			_bootId = 0;
	uint64			_runSeq = 0;

	mutex						_queueLock;
	condition_variable			_queueCv;
	deque<pair<Sink, string>>	_queue;
	bool						_stop = false;
	atomic<bool>				_open = false;
	atomic<uint32>				_stallMs = 0;

	// 쓰기 스레드만 만진다 (Open 의 헤더 쓰기는 스레드를 띄우기 전)
	ofstream		_runs;
	ofstream		_gauge;
	thread			_writer;
};

extern Telemetry GTelemetry;
