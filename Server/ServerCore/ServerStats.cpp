#include "pch.h"
#include "ServerStats.h"
#include "Utils.h"
#include <psapi.h>

#pragma comment(lib, "psapi.lib")

ServerStats GStats;

namespace
{
	constexpr double SENT_LIMIT_MBPS = 80.0;
	constexpr double PER_CLIENT_LIMIT_KBPS = 400.0;
	constexpr double MEMORY_LIMIT_MB = 512.0;
}

namespace
{
	const char* Verdict(bool pass) { return pass ? "PASS" : "*** FAIL ***"; }
}

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

uint64 ServerStats::PercentileUpperUs(const uint64* buckets, uint64 total, double p)
{
	if (total == 0)
		return 0;

	const uint64 target = static_cast<uint64>(total * p);
	uint64 acc = 0;

	for (int32 i = 0; i < ServerStats::BUCKET_COUNT; i++)
	{
		acc += buckets[i];
		if (acc >= target)
			return ServerStats::BUCKET_UPPER_US[i];
	}

	return ServerStats::BUCKET_UPPER_US[ServerStats::BUCKET_COUNT - 1];
}

void ServerStats::SnapshotBuckets(uint64* outBuckets) const
{
	for (int32 i = 0; i < BUCKET_COUNT; i++)
		outBuckets[i] = _flushBuckets[i].load();
}

void ServerStats::AddSentPacket(uint16 id, uint64 bytes)
{
	_sentByPacket[id < PACKET_ID_LIMIT ? id : 0].fetch_add(bytes);
}

uint64 ServerStats::GetSentPacketBytes(uint16 id) const
{
	return _sentByPacket[id < PACKET_ID_LIMIT ? id : 0].load();
}

void ServerStats::Dump(int32 sessionCount)
{
	const uint64 nowUs = Utils::NowMicroseconds();
	const double elapsedSec = (_lastDumpUs == 0) ? 0.0 : static_cast<double>(nowUs - _lastDumpUs) / 1000000.0;
	_lastDumpUs = nowUs;

	// 대역폭은 '이번 구간' 값이므로 읽으면서 0으로 되돌린다.
	const uint64 recvBytes = _recvBytes.exchange(0);
	const uint64 sentBytes = _sentBytes.exchange(0);

	const uint64 flushTotal = _flushCount.exchange(0);
	const uint64 flushMax = _flushMaxUs.exchange(0);

	const uint64 moveRejects = _moveRejects.exchange(0);
	const uint64 moveBlocked = _moveBlocked.exchange(0);
	const uint64 moveWallRejects = _moveWallRejects.exchange(0);
	const uint64 attackWallBlocked = _attackWallBlocked.exchange(0);
	const uint64 mutualKills = _mutualKills.exchange(0);

	const uint64 fowPairs = _fowPairs.exchange(0);
	const uint64 fowLos = _fowLos.exchange(0);
	const uint64 fowSpawns = _fowSpawns.exchange(0);
	const uint64 fowDespawns = _fowDespawns.exchange(0);
	const uint64 fowShared = _fowShared.exchange(0);
	const uint64 fowCustom = _fowCustom.exchange(0);
	const uint64 fowUpdateUs = _fowUpdateUs.exchange(0);
	const uint64 fowUpdateMaxUs = _fowUpdateMaxUs.exchange(0);

	uint64 buckets[BUCKET_COUNT];
	for (int32 i = 0; i < BUCKET_COUNT; i++)
		buckets[i] = _flushBuckets[i].exchange(0);

	const uint64 p95 = PercentileUpperUs(buckets, flushTotal, 0.95);
	const uint64 p99 = PercentileUpperUs(buckets, flushTotal, 0.99);

	double recvMbps = 0.0;
	double sentMbps = 0.0;

	if (elapsedSec > 0.0)
	{
		recvMbps = (recvBytes * 8.0) / elapsedSec / 1000000.0;
		sentMbps = (sentBytes * 8.0) / elapsedSec / 1000000.0;
	}

	// 클락 1인이 받는 양 = 서버가 보낸 총량 % 인원
	const double perClientKbps = (sessionCount > 0) ? (sentMbps * 1000.0 / sessionCount) : 0.0;

	// 패킷 종류별 - 큐에 넣은 바이트 기준(AddSentPacket). 비율은 이 칸들의 합으로만 나눈다
	uint64 byPacket[PACKET_ID_LIMIT];
	uint64 queuedBytes = 0;
	for (int32 i = 0; i < PACKET_ID_LIMIT; i++)
	{
		byPacket[i] = _sentByPacket[i].exchange(0);
		queuedBytes += byPacket[i];
	}

	auto toMbps = [elapsedSec](uint64 bytes) { return elapsedSec > 0.0 ? (bytes * 8.0) / elapsedSec / 1000000.0 : 0.0; };
	auto shareOf = [queuedBytes](uint64 bytes) { return queuedBytes > 0 ? bytes * 100.0 / queuedBytes : 0.0; };
	auto nameOf = [this](uint16 id) -> string
	{
		const char* name = (_packetName != nullptr) ? _packetName(id) : nullptr;
		return name != nullptr ? string(name) : to_string(id);
	};

	char line[160];
	string itemLine;
	if (_itemPackets.empty() == false)
	{
		uint64 itemBytes = 0;
		string detail;
		for (uint16 id : _itemPackets)
		{
			const uint64 bytes = byPacket[id < PACKET_ID_LIMIT ? id : 0];
			itemBytes += bytes;

			::snprintf(line, sizeof(line), "  %s=%.1fk", nameOf(id).c_str(), toMbps(bytes) * 1000.0);
			detail += line;
		}

		::snprintf(line, sizeof(line), " SV-5 item    %.3f Mbps (%.2f%% of queued %.2f Mbps)",
			toMbps(itemBytes), shareOf(itemBytes), toMbps(queuedBytes));
		itemLine = string(line) + detail + "\n";
	}

	// 상위 6종 - 아이템 몫이 어디에 비해 큰지 작은지를 같은 줄에서 본다
	string topLine;
	{
		vector<pair<uint64, uint16>> ranked;
		for (int32 i = 0; i < PACKET_ID_LIMIT; i++)
		{
			if (byPacket[i] > 0)
				ranked.emplace_back(byPacket[i], static_cast<uint16>(i));
		}

		const size_t shown = min<size_t>(ranked.size(), 6);
		partial_sort(ranked.begin(), ranked.begin() + shown, ranked.end(), greater<>());

		if (shown > 0)
		{
			topLine = " SV-5 top    ";
			for (size_t i = 0; i < shown; i++)
			{
				::snprintf(line, sizeof(line), " %s %.2f Mbps (%.1f%%)", nameOf(ranked[i].second).c_str(),
					toMbps(ranked[i].first), shareOf(ranked[i].first));
				topLine += line;
			}
			topLine += "\n";
		}
	}

	PROCESS_MEMORY_COUNTERS_EX pmc = {};
	::GetProcessMemoryInfo(::GetCurrentProcess(),
		reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
	const double memMB = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
	const double commitMB = static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);

	// 🔴 판정 줄이다. 여러 문장으로 나눠 찍으면 다른 스레드 출력이 사이에 끼어
	//    게이트 판정이 뒤집힌다(2026-09-20 실제로 2건). 블록 전체를 한 덩어리로 로거에 넣는다.
	LogLine out;

	out << "\n===== SERVER STATS (window " << elapsedSec << "s, sessions " << sessionCount << ") =====\n";

	out << "flush n=" << flushTotal
		<< " p95<=" << p95 << "us " << Verdict(p95 <= 16600)
		<< " p99<=" << p99 << "us " << Verdict(p99 <= 33000)
		<< " max=" << flushMax << "us " << Verdict(flushMax <= 100000) << "\n";

	out << " SV-3 move    reject=" << moveRejects << " " << Verdict(moveRejects == 0)
		<< "  blocked=" << moveBlocked << " (hard CC, informational)"
		<< "  wall=" << moveWallRejects << " (bots ignore walls, informational)\n";

	out << " combat       attack_wall=" << attackWallBlocked << " (line of attack blocked, informational)"
		<< "  mutual_kills=" << mutualKills << '\n';

	// FoW - 시야 갱신이 쓴 시간(합 · 최대 1회)과 판정 수, 스폰 변동, 필터 방송의 공유 / 개별 세션 수 (informational)
	if (fowPairs > 0 || fowShared > 0 || fowCustom > 0)
	{
		out << " fow          update_us=" << fowUpdateUs << " max=" << fowUpdateMaxUs << "us  pairs=" << fowPairs << " los=" << fowLos
			<< "  spawn=" << fowSpawns << " despawn=" << fowDespawns
			<< "  sends shared=" << fowShared << " custom=" << fowCustom << " (informational)\n";
	}

	out << " SV-5 band    recv=" << recvMbps << " Mbps"
		<< "  sent=" << sentMbps << " Mbps " << Verdict(sentMbps <= SENT_LIMIT_MBPS)
		<< "  per-client=" << perClientKbps << " kbps " << Verdict(perClientKbps <= PER_CLIENT_LIMIT_KBPS) << "\n";

	out << itemLine << topLine;

	out << " SV-6 memory  commit=" << commitMB << " MB " << Verdict(commitMB <= MEMORY_LIMIT_MB)
		<< "  workingset=" << memMB << " MB\n";

	out << "==========================================================\n\n";
}
