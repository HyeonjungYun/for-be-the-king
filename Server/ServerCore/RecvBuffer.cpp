#include "pch.h"
#include "RecvBuffer.h"

/*--------------
	RecvBuffer
----------------*/

RecvBuffer::RecvBuffer(int32 bufferSize, int32 bufferCount) : _bufferSize(bufferSize)
{
	_initialCapacity = bufferSize * bufferCount;
	_capacity = _initialCapacity;
	_buffer.resize(_capacity);
}

RecvBuffer::~RecvBuffer()
{
}

void RecvBuffer::Clean()
{
	int32 dataSize = DataSize();
	if (dataSize == 0)
	{
		// 딱 마침 읽기+쓰기 커서가 동일한 위치라면, 둘 다 리셋.
		_readPos = _writePos = 0;

		// 큰 패킷 때문에 커졌던 버퍼는 비었을 때 처음 크기로 - 스냅샷 한 번에 세션마다 64 KB 를 계속 쥐지 않게 (2026-11-21)
		if (_capacity > _initialCapacity)
		{
			vector<BYTE>(_initialCapacity).swap(_buffer);
			_capacity = _initialCapacity;
		}
		return;
	}

	// 여유 공간이 버퍼 1개 크기 미만이면, 데이터를 앞으로 땅긴다.
	if (FreeSize() < _bufferSize)
	{
		// 남은 데이터가 _readPos 보다 길면 두 구간이 겹친다 - memcpy 는 겹침을 보장하지 않는다
		::memmove(&_buffer[0], &_buffer[_readPos], dataSize);
		_readPos = 0;
		_writePos = dataSize;
	}

	// 당겨도 3/4 넘게 차 있다 = 처음 용량보다 큰 패킷이 걸쳐 있다 - 키운다 (2026-11-21).
	// 예전엔 처음 용량(640 KB)이 패킷 상한(64 KB)보다 커서 생길 일이 없었다. 작게 시작하면 여기서 키우지 않을 때
	// FreeSize 0 으로 수신을 걸어 "Recv 0" 으로 끊긴다 (RecvBufferTest FreeSize가_0인_상태를_만들_수_있다)
	if (FreeSize() < _capacity / 4 && _capacity < MAX_CAPACITY)
	{
		_capacity = (min)(_capacity * 2, static_cast<int32>(MAX_CAPACITY));
		_buffer.resize(_capacity);
	}
}

bool RecvBuffer::OnRead(int32 numOfBytes)
{
	if (numOfBytes > DataSize())
		return false;

	_readPos += numOfBytes;
	return true;
}

bool RecvBuffer::OnWrite(int32 numOfBytes)
{
	if (numOfBytes > FreeSize())
		return false;

	_writePos += numOfBytes;
	return true;
}
