#pragma once

/*----------------------
	PositionHistory
----------------------*/

// 생물 하나가 지난 몇 초 동안 서 있던 자리 (2026-10-11 3차 로스터 - 회귀 1167 "3초 전에 서 있던 자리로" · 발자국 추적 1187).
// Room::UpdateTick 이 10 Hz 로 Record 한다. 순수 자료구조 - GameServerTests PositionHistoryTest 가 검사한다
class PositionHistory
{
public:
	struct Sample
	{
		uint64	atUs = 0;
		float	x = 0.f;
		float	y = 0.f;
	};

	static constexpr uint32 CAPACITY = 80;		// 10 Hz × 8 s - 회귀 3 s · 발자국 6 s 를 넉넉히 덮는다

	void Record(uint64 nowUs, float x, float y)
	{
		_samples[_next] = { nowUs, x, y };
		_next = (_next + 1) % CAPACITY;
		if (_count < CAPACITY)
			_count++;
	}

	void Clear()
	{
		_count = 0;
		_next = 0;
	}

	uint32 Count() const { return _count; }

	// tUs 에 서 있던 자리 = tUs 이전의 가장 늦은 표본. 그보다 오래된 표본이 없으면(막 들어왔다) 가장 오래된 것. 비어 있으면 false
	bool At(uint64 tUs, OUT float& x, OUT float& y) const
	{
		if (_count == 0)
			return false;

		const Sample* best = nullptr;
		const Sample* oldest = nullptr;
		for (uint32 i = 0; i < _count; i++)
		{
			const Sample& s = _samples[i];
			if (oldest == nullptr || s.atUs < oldest->atUs)
				oldest = &s;
			if (s.atUs <= tUs && (best == nullptr || s.atUs > best->atUs))
				best = &s;
		}

		const Sample* pick = (best != nullptr) ? best : oldest;
		x = pick->x;
		y = pick->y;
		return true;
	}

	// sinceUs 이후의 표본을 오래된 것부터, 서로 spacingUs 이상 떨어진 것만 (발자국 간격)
	template <typename Fn>
	void ForEachSince(uint64 sinceUs, uint64 spacingUs, Fn&& fn) const
	{
		const uint32 start = (_count < CAPACITY) ? 0 : _next;
		uint64 lastUs = 0;
		bool any = false;
		for (uint32 k = 0; k < _count; k++)
		{
			const Sample& s = _samples[(start + k) % CAPACITY];
			if (s.atUs < sinceUs)
				continue;
			if (any && s.atUs < lastUs + spacingUs)
				continue;
			fn(s);
			lastUs = s.atUs;
			any = true;
		}
	}

private:
	Sample	_samples[CAPACITY];
	uint32	_next = 0;
	uint32	_count = 0;
};
