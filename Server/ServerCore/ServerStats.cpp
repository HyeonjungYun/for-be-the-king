#include "pch.h"
#include "ServerStats.h"
#include "Utils.h"
#include <psapi.h>

#pragma comment(lib, "psapi.lib")

ServerStats GStats;

const uint64 ServerStats::BUCKET_UPPER_US[ServerStats::BUCKET_COUNT] =
{
	100, 250, 500, 1000, 2000, 4000, 8000,
	16600,
	33000,
	100000,
	UINT64_MAX
};

void ServerStats::RecordFlush(uint64 micros)
{
	for (int32 i = 0; i < BUCKET_COUNT; i++)
	{
		if (micros <= BUCKET_UPPER_US[i])
		{
			_flushBuckets[i].fetch_add(1);
			break;
		}
	}

	_flushCount.fetch_add(1);

	uint64 prevMax = _flushMaxUs.load();
	while (micros > prevMax && _flushMaxUs.compare_exchange_weak(prevMax, micros) == false)
	{
	}
}

namespace
{
	uint64 PercentileUpperUs(const atomic<uint64>* buckets, uint64 total, double p)
	{
		if (total == 0)
			return 0;

		const uint64 target = static_cast<uint64>(total * p);
		uint64 acc = 0;

		for (int32 i = 0; i < ServerStats::BUCKET_COUNT; i++)
		{
			acc += buckets[i].load();
			if (acc >= target)
				return ServerStats::BUCKET_UPPER_US[i];
		}

		return ServerStats::BUCKET_UPPER_US[ServerStats::BUCKET_COUNT - 1];
	}

	const char* Verdict(bool pass) { return pass ? "PASS" : "*** FAIL ***"; }
}

void ServerStats::Dump(int32 sessionCount)
{
	const uint64 nowUs = Utils::NowMicroseconds();
	const double elapsedSec = (_lastDumpUs == 0) ? 0.0 : static_cast<double>(nowUs - _lastDumpUs) / 1000000.0;
	_lastDumpUs = nowUs;

	// 대역폭은 '이번 구간' 값이므로 읽으면서 0으로 되돌린다.
	const uint64 recvBytes = _recvBytes.exchange(0);
	const uint64 sentBytes = _sentBytes.exchange(0);

	const uint64 flushTotal = _flushCount.load();
	const uint64 flushMax = _flushMaxUs.load();
	const uint64 p95 = PercentileUpperUs(_flushBuckets, flushTotal, 0.95);
	const uint64 p99 = PercentileUpperUs(_flushBuckets, flushTotal, 0.99);

	double recvMbps = 0.0;
	double sentMbps = 0.0;

	if (elapsedSec > 0.0)
	{
		recvMbps = (recvBytes * 8.0) / elapsedSec / 1000000.0;
		sentMbps = (sentBytes * 8.0) / elapsedSec / 1000000.0;
	}

	// 클락 1인이 받는 양 = 서버가 보낸 총랼 % 인원
	const double perClientKbps = (sessionCount > 0) ? (sentMbps * 1000.0 / sessionCount) : 0.0;

	PROCESS_MEMORY_COUNTERS pmc = {};
	::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc));
	const double memMB = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);

	cout << "\n===== SERVER STATS (window " << elapsedSec << "s, sessions " << sessionCount << ") =====\n";

	cout << "flush n=" << flushTotal
		<< " p95<=" << p95 << "us " << Verdict(p95 <= 16600)
		<< " p99<=" << p99 << "us " << Verdict(p99 <= 33000)
		<< " max=" << flushMax << "us " << Verdict(flushMax <= 100000) << "\n";

	cout << " SV-5 band    recv=" << recvMbps << " Mbps"
		<< "  sent=" << sentMbps << " Mbps " << Verdict(sentMbps <= 12.0)
		<< "  per-client=" << perClientKbps << " kbps " << Verdict(perClientKbps <= 400.0) << "\n";

	cout << " SV-6 memory  workingset=" << memMB << " MB " << Verdict(memMB <= 130.0) << "\n";

	cout << "==========================================================\n" << endl;
}
