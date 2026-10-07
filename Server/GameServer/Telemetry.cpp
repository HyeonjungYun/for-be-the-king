#include "pch.h"
#include "Telemetry.h"
#include <filesystem>
#include <chrono>
#include <ctime>

Telemetry GTelemetry;

const char* Telemetry::RUNS_HEADER = "ts_unix_ms,boot_id,run_seq,char_id,floor_id,entry_point,reason,run_ms,brought_in_sum,end_carried_sum,"
	"end_carried_count,end_carried_max_grade,run_gold,gold_kept,zone_visits,zone_leaves_voluntary,zone_leaves_forced,zone_dwell_ms,"
	"gauge_reversals_present,alert_dwell_ms,last_tower_id,killer_id";

const char* Telemetry::GAUGE_HEADER = "ts_unix_ms,boot_id,floor_id,tower_id,event,progress_permille,occupants";

namespace
{
	uint64 UnixMs()
	{
		return static_cast<uint64>(chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now().time_since_epoch()).count());
	}
}

Telemetry::~Telemetry()
{
	{
		lock_guard<mutex> guard(_queueLock);
		_stop = true;
	}
	_queueCv.notify_one();

	// 남은 줄을 다 쓰고 끝난다 - 그다음 ofstream 이 닫힌다
	if (_writer.joinable())
		_writer.join();
}

bool Telemetry::Open(const string& directory)
{
	{
		lock_guard<mutex> guard(_lock);
		_bootId = UnixMs() / 1000;
	}

	std::error_code ec;
	std::filesystem::create_directories(directory, ec);

	char stamp[32];
	const time_t now = static_cast<time_t>(_bootId);
	tm local{};
	::localtime_s(&local, &now);
	::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);

	_runs.open(directory + "\\runs-" + stamp + ".csv", ios::out | ios::trunc);
	_gauge.open(directory + "\\gauge-" + stamp + ".csv", ios::out | ios::trunc);

	if (_runs.is_open() == false || _gauge.is_open() == false)
		return false;

	_runs << RUNS_HEADER << '\n' << flush;
	_gauge << GAUGE_HEADER << '\n' << flush;

	_writer = thread([this]() { WriterLoop(); });
	_open.store(true);
	return true;
}

string Telemetry::FormatRun(uint64 tsMs, uint64 bootId, uint64 runSeq, const RunRow& r)
{
	ostringstream os;
	os << tsMs << ',' << bootId << ',' << runSeq << ',' << r.charId << ',' << r.floorId << ',' << r.entryPoint << ','
		<< r.reason << ',' << r.runMs << ',' << r.broughtInSum << ',' << r.endCarriedSum << ',' << r.endCarriedCount << ','
		<< r.endCarriedMaxGrade << ',' << r.runGold << ',' << r.goldKept << ',' << r.zoneVisits << ',' << r.zoneLeavesVoluntary << ','
		<< r.zoneLeavesForced << ',' << r.zoneDwellMs << ',' << r.gaugeReversalsPresent << ',' << r.alertDwellMs << ','
		<< r.lastTowerId << ',' << r.killerId;
	return os.str();
}

void Telemetry::AppendRun(const RunRow& row)
{
	string line;
	{
		lock_guard<mutex> guard(_lock);
		line = FormatRun(UnixMs(), _bootId, ++_runSeq, row);
	}

	Enqueue(Sink::Runs, line);
	LOG_LINE << "[TELEM] run " << line << '\n';
}

void Telemetry::AppendGauge(uint32 floorId, const string& towerId, const char* event, uint32 permille, uint32 occupants)
{
	uint64 bootId = 0;
	{
		lock_guard<mutex> guard(_lock);
		bootId = _bootId;
	}

	ostringstream os;
	os << UnixMs() << ',' << bootId << ',' << floorId << ',' << towerId << ',' << event << ',' << permille << ',' << occupants;
	Enqueue(Sink::Gauge, os.str());
}

void Telemetry::Enqueue(Sink sink, string line)
{
	if (_open.load() == false)
		return;

	{
		lock_guard<mutex> guard(_queueLock);
		_queue.emplace_back(sink, move(line));
	}
	_queueCv.notify_one();
}

void Telemetry::WriterLoop()
{
	deque<pair<Sink, string>> batch;

	while (true)
	{
		bool stopping = false;
		{
			unique_lock<mutex> guard(_queueLock);
			_queueCv.wait(guard, [this]() { return _stop || _queue.empty() == false; });
			batch.swap(_queue);
			stopping = _stop;
		}

		if (const uint32 stall = _stallMs.load(); stall > 0 && batch.empty() == false)
			this_thread::sleep_for(chrono::milliseconds(stall));

		bool wroteRuns = false;
		bool wroteGauge = false;
		for (const auto& [sink, line] : batch)
		{
			ofstream& out = (sink == Sink::Runs) ? _runs : _gauge;
			out << line << '\n';
			(sink == Sink::Runs ? wroteRuns : wroteGauge) = true;
		}
		batch.clear();

		// 묶음마다 한 번 - 서버가 죽어도 그 전 묶음까지는 파일에 있다
		if (wroteRuns)
			_runs.flush();
		if (wroteGauge)
			_gauge.flush();

		if (stopping)
		{
			lock_guard<mutex> guard(_queueLock);
			if (_queue.empty())
				return;
		}
	}
}
