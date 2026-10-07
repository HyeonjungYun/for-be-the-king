#pragma once

/*-------------------
	ServerStats
-------------------*/

class ServerStats
{
public:
	enum { BUCKET_COUNT = 11 };

	// 패킷 id 칸 수. 이 범위 밖의 id 는 0 번 칸("기타")에 모인다
	enum { PACKET_ID_LIMIT = 2048 };

	// ServerCore 는 패킷 목록을 모른다 - 이름과 "아이템 패킷" 분류는 콘텐츠가 부팅 때 한 번 넣는다
	using PacketNameFn = const char* (*)(uint16 id);

	void			RecordFlush(uint64 micros);

	void			AddRecvBytes(uint64 bytes) { _recvBytes.fetch_add(bytes); }
	void			AddSentBytes(uint64 bytes) { _sentBytes.fetch_add(bytes); }

	// 세션 송신 큐에 넣은 바이트를 패킷 id 별로 센다 (Session::Send). 브로드캐스트는 받는 세션 수만큼 센다.
	// sent(완료 기준)와 시점이 달라 합이 약간 다를 수 있다 - 비율을 볼 때는 이 칸들끼리만 나눈다
	void			AddSentPacket(uint16 id, uint64 bytes);
	uint64			GetSentPacketBytes(uint16 id) const;

	void			SetPacketNames(PacketNameFn fn) { _packetName = fn; }
	void			SetItemPackets(const vector<uint16>& ids) { _itemPackets = ids; }

	void			AddMoveReject() { _moveRejects.fetch_add(1); }
	void			AddMoveBlocked() { _moveBlocked.fetch_add(1); }

	// 서버 벽에 막힌 이동 (reason=wall). SV-3 판정에 넣지 않는다 - 봇은 벽을 모른다 (ADR-0006 § Risks)
	void			AddMoveWallReject() { _moveWallRejects.fetch_add(1); }

	void			Dump(int32 sessionCount);

	static uint64	PercentileUpperUs(const uint64* buckets, uint64 total, double p);

	void			SnapshotBuckets(uint64* outBuckets) const;
	uint64			GetFlushCount() const { return _flushCount.load(); }
	uint64			GetFlushMaxUs() const { return _flushMaxUs.load(); }

public:
	static const uint64 BUCKET_UPPER_US[BUCKET_COUNT];

private:
	atomic<uint64>	_flushBuckets[BUCKET_COUNT];
	atomic<uint64>	_flushCount = 0;
	atomic<uint64>	_flushMaxUs = 0;

	atomic<uint64>	_recvBytes = 0;
	atomic<uint64>	_sentBytes = 0;

	atomic<uint64>	_moveRejects = 0;
	atomic<uint64>	_moveBlocked = 0;
	atomic<uint64>	_moveWallRejects = 0;

	atomic<uint64>	_sentByPacket[PACKET_ID_LIMIT];
	PacketNameFn	_packetName = nullptr;
	vector<uint16>	_itemPackets;

	uint64			_lastDumpUs = 0;
};

extern ServerStats GStats;
