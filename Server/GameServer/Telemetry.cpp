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

bool Telemetry::Open(const string& directory)
{
	lock_guard<mutex> guard(_lock);

	_bootId = UnixMs() / 1000;

	std::error_code ec;
	std::filesystem::create_directories(directory, ec);

	char stamp[32];
	const time_t now = static_cast<time_t>(_bootId);
	tm local{};
	::localtime_s(&local, &now);
	::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);

	_runs.open(directory + "\\runs-" + stamp + ".csv", ios::out | ios::trunc);
	_gauge.open(directory + "\\gauge-" + stamp + ".csv", ios::out | ios::trunc);
	_open = _runs.is_open() && _gauge.is_open();

	if (_open)
	{
		_runs << RUNS_HEADER << '\n' << flush;
		_gauge << GAUGE_HEADER << '\n' << flush;
	}

	return _open;
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
		if (_open)
			_runs << line << '\n' << flush;
	}

	LOG_LINE << "[TELEM] run " << line << '\n';
}

void Telemetry::AppendGauge(uint32 floorId, const string& towerId, const char* event, uint32 permille, uint32 occupants)
{
	lock_guard<mutex> guard(_lock);
	if (_open)
		_gauge << UnixMs() << ',' << _bootId << ',' << floorId << ',' << towerId << ',' << event << ',' << permille << ',' << occupants << '\n' << flush;
}
