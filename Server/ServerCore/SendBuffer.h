#pragma once

class SendBufferChunk;

/*----------------
	SendBuffer
-----------------*/

class SendBuffer
{
public:
	SendBuffer(int32 bufferSize);
	~SendBuffer();

	BYTE* Buffer() { return _buffer.data(); }
	int32 WriteSize() { return _writeSize; }
	int32 Capacity() { return static_cast<int32>(_buffer.size()); }

	void CopyData(void* data, int32 len);
	void Close(uint32 writeSize);

	// 송신 통계에서 이 버퍼를 셀 칸. 0 이면 헤더의 패킷 id 로 센다.
	// 같은 패킷 id 를 용도별로 나눠 볼 때 쓴다 (예: S_SPAWN 중 가방만) - ServerStats::AddSentPacket
	void	SetStatId(uint16 statId) { _statId = statId; }
	uint16	GetStatId() const { return _statId; }

private:
	vector<BYTE>	_buffer;
	int32			_writeSize = 0;
	uint16			_statId = 0;
};

