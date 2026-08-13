#pragma once

/*-------------------
	ServerStats
-------------------*/

class ServerStats
{
public:
	enum { BUCKET_COUNT = 11 };

	void RecordFlush(uint64 micros);

	void AddRecvBytes(uint64 bytes) { _recvBytes.fetch_add(bytes); }
	void AddSentBytes(uint64 bytes) { _sentBytes.fetch_add(bytes); }

	void Dump(int32 sessionCount);

public:
	static const uint64 BUCKET_UPPER_US[BUCKET_COUNT];

private:
	atomic<uint64> _flushBuckets[BUCKET_COUNT];
	atomic<uint64> _flushCount = 0;
	atomic<uint64> _flushMaxUs = 0;

	atomic<uint64> _recvBytes = 0;
	atomic<uint64> _sentBytes = 0;

	uint64 _lastDumpUs = 0;
};

extern ServerStats GStats;