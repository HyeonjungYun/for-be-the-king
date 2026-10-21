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

	// 벽 · 엄폐물에 시선이 막힌 평타 - 대상 락 거절 + windup 중 취소 (combat-system.md Rule 7). 정보용 - 봇은 벽을 모른다
	void			AddAttackWallBlocked() { _attackWallBlocked.fetch_add(1); }

	// 같은 전투 Flush 에서 서로를 죽인 쌍 (TR-combat-022 - 예전엔 늘 0 이었다)
	void			AddMutualKill() { _mutualKills.fetch_add(1); }

	// FoW (design/gdd/fog-of-war.md, 2026-10-11) - 시야 갱신 비용과 효과. 정보용 줄 하나
	//   pairs = 거리 검사한 (보는 사람, 생물) 쌍 · los = 그중 시선 검사까지 간 수 · spawn / despawn = 시야로 생긴 · 사라진 수
	//   shared / custom = 필터 방송에서 공유 버퍼를 받은 / 자기 몫을 따로 받은 세션 수 (custom 이 곧 직렬화 추가 비용)
	void			AddFowWork(uint64 pairs, uint64 los) { _fowPairs.fetch_add(pairs); _fowLos.fetch_add(los); }
	void			AddFowChurn(uint64 spawned, uint64 despawned) { _fowSpawns.fetch_add(spawned); _fowDespawns.fetch_add(despawned); }
	void			AddFowSends(uint64 shared, uint64 custom) { _fowShared.fetch_add(shared); _fowCustom.fetch_add(custom); }
	void			AddFowUpdateUs(uint64 us) { _fowUpdateUs.fetch_add(us); uint64 m = _fowUpdateMaxUs.load(); while (us > m && _fowUpdateMaxUs.compare_exchange_weak(m, us) == false) {} }

	// 잡 타이머가 예약 시각보다 늦게 분배한 시간 (ms) - 33 ms Flush 의 흔들림 (JobTimer::Distribute, 2026-10-21)
	void			RecordTimerLate(uint64 lateMs);

	// 콘텐츠가 Dump 에 덧붙이는 줄 (진행 중인 런 끝 저장 수 등). ServerCore 는 그 값을 모른다
	using ExtraLineFn = function<string()>;
	void			SetExtraLine(ExtraLineFn fn) { _extraLine = move(fn); }

	// Dump 마다 한 행을 CSV 로도 남긴다 - <dir>\stats-<boot>.csv. 사람이 읽는 블록은 그대로. 실패하면 false (서버는 돈다)
	bool			EnableCsv(const string& dir);

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
	atomic<uint64>	_attackWallBlocked = 0;
	atomic<uint64>	_mutualKills = 0;

	atomic<uint64>	_fowPairs = 0;
	atomic<uint64>	_fowLos = 0;
	atomic<uint64>	_fowSpawns = 0;
	atomic<uint64>	_fowDespawns = 0;
	atomic<uint64>	_fowShared = 0;
	atomic<uint64>	_fowCustom = 0;
	atomic<uint64>	_fowUpdateUs = 0;
	atomic<uint64>	_fowUpdateMaxUs = 0;

	atomic<uint64>	_sentByPacket[PACKET_ID_LIMIT];
	PacketNameFn	_packetName = nullptr;
	vector<uint16>	_itemPackets;

	// 타이머 지연 - Flush 와 같은 11 버킷(µs 로 환산) · 최대
	atomic<uint64>	_timerLateBuckets[BUCKET_COUNT];
	atomic<uint64>	_timerLateCount = 0;
	atomic<uint64>	_timerLateMaxUs = 0;

	ExtraLineFn		_extraLine;

	mutex			_csvLock;
	FILE*			_csv = nullptr;
	uint64			_dumpSeq = 0;

	uint64			_lastDumpUs = 0;
};

extern ServerStats GStats;
