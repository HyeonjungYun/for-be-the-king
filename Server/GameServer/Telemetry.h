#pragma once
#include <mutex>
#include <fstream>

/*--------------
	Telemetry
--------------*/

// 런 단위 텔레메트리 (design/gdd/escape-timer.md § G). CSV 두 개 - 서버 시작마다 새 파일.
//   runs-<boot>.csv     한 행 = 한 런 (탈출 · 만료 · 사망 · 끊김 사망) - 22 열
//   gauge-<boot>.csv    타워 게이지 상태 전이
// 방 4개가 각자 스레드에서 부른다 - 뮤텍스 하나로 줄을 통째로 쓴다(깨진 줄 0, AC-ET-16). 런당 1줄이라 드물다.

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

	// 디렉터리를 만들고 두 파일을 연다. 실패하면 기록만 끄고 서버는 돈다(측정 전용 - ADVISORY)
	bool			Open(const string& directory);

	void			AppendRun(const RunRow& row);
	void			AppendGauge(uint32 floorId, const string& towerId, const char* event, uint32 permille, uint32 occupants);

	// 열 순서 그대로 한 줄 (테스트가 열 수를 센다)
	static string	FormatRun(uint64 tsMs, uint64 bootId, uint64 runSeq, const RunRow& row);

	uint64			BootId() const { return _bootId; }

private:
	mutex			_lock;
	ofstream		_runs;
	ofstream		_gauge;
	uint64			_bootId = 0;
	uint64			_runSeq = 0;
	bool			_open = false;
};

extern Telemetry GTelemetry;
