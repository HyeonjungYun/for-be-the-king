#pragma once

/*--------------
	RecvBuffer
----------------*/

// 처음 용량 = bufferSize × bufferCount. 2026-11-21 (동접 1만) 부터 **모자라면 커지고, 비면 처음 크기로 돌아간다**.
// 예전엔 세션마다 64 KB × 10 = 640 KB 를 고정으로 잡아 1만 세션이면 수신 버퍼만 6.4 GB 였다 (docs/server-scaling-10k.md § 2-1).
// 클라 → 서버 패킷은 수십 바이트라 서버는 작게 시작하고, 큰 스냅샷을 받는 쪽(봇)만 그때그때 커진다.
class RecvBuffer
{
	enum { BUFFER_COUNT = 10 };

public:
	// 패킷 하나는 u16 크기 (64 KB) 가 상한 - 그 두 배면 걸쳐 있는 패킷 하나 + 다음 조각이 언제나 들어간다
	static constexpr int32 MAX_CAPACITY = 0x20000;

	RecvBuffer(int32 bufferSize, int32 bufferCount = BUFFER_COUNT);
	~RecvBuffer();

	// 읽은 만큼 정리하고, 남은 공간이 모자라면 앞으로 당기거나 키운다. 수신을 걸기 직전(걸린 수신이 없을 때)에만 부른다
	void			Clean();
	bool			OnRead(int32 numOfBytes);
	bool			OnWrite(int32 numOfBytes);

	BYTE*			ReadPos() { return &_buffer[_readPos]; }
	BYTE*			WritePos() { return &_buffer[_writePos]; }
	int32			DataSize() { return _writePos - _readPos; }
	int32			FreeSize() { return _capacity - _writePos; }
	int32			Capacity() const { return _capacity; }

private:
	int32			_initialCapacity = 0;
	int32			_capacity = 0;
	int32			_bufferSize = 0;
	int32			_readPos = 0;
	int32			_writePos = 0;
	vector<BYTE>	_buffer;
};
