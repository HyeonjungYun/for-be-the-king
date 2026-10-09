#include "pch.h"
#include "StatusEffects.h"

uint32 StatusSet::AddDot(const DotInstance& dot, uint32 maxStacks)
{
	if (maxStacks == 0)
		maxStacks = 1;

	// 같은 종류 중 가장 먼저 끝나는 것 - 상한이면 그 자리를 새 것이 차지한다
	size_t count = 0;
	size_t oldest = _dots.size();

	for (size_t i = 0; i < _dots.size(); i++)
	{
		if (_dots[i].kind != dot.kind)
			continue;

		count++;
		if (oldest == _dots.size() || _dots[i].endUs < _dots[oldest].endUs)
			oldest = i;
	}

	if (count >= maxStacks && oldest < _dots.size())
	{
		_dots[oldest] = dot;
		return static_cast<uint32>(count);
	}

	_dots.push_back(dot);
	return static_cast<uint32>(count + 1);
}

uint32 StatusSet::RefreshOrAddDot(const DotInstance& dot, uint32 maxStacks)
{
	for (DotInstance& d : _dots)
	{
		if (d.skillId == dot.skillId && d.instigatorId == dot.instigatorId && d.kind == dot.kind)
		{
			d.endUs = max(d.endUs, dot.endUs);
			d.baseDamage = dot.baseDamage;
			d.type = dot.type;
			return DotStacks(dot.kind);
		}
	}
	return AddDot(dot, maxStacks);
}

uint32 StatusSet::DotStacks(const string& kind) const
{
	uint32 count = 0;
	for (const DotInstance& d : _dots)
	{
		if (d.kind == kind)
			count++;
	}
	return count;
}

bool StatusSet::AddBuff(const BuffInstance& buff)
{
	for (BuffInstance& b : _buffs)
	{
		if (b.skillId == buff.skillId && b.stat == buff.stat)
		{
			b = buff;
			return false;
		}
	}

	_buffs.push_back(buff);
	return true;
}

float StatusSet::BuffSum(SkillBuffStat stat) const
{
	float sum = 0.f;
	for (const BuffInstance& b : _buffs)
	{
		if (b.stat == stat)
			sum += b.magnitude;
	}
	return sum;
}

bool StatusSet::AddMark(const MarkInstance& mark)
{
	for (MarkInstance& m : _marks)
	{
		if (m.kind == mark.kind && m.instigatorId == mark.instigatorId)
		{
			m = mark;
			return false;
		}
	}

	_marks.push_back(mark);
	return true;
}

bool StatusSet::HasMark(const string& kind, uint64 instigatorId) const
{
	for (const MarkInstance& m : _marks)
	{
		if (m.kind == kind && m.instigatorId == instigatorId)
			return true;
	}
	return false;
}

bool StatusSet::ConsumeMark(const string& kind, uint64 instigatorId)
{
	for (size_t i = 0; i < _marks.size(); i++)
	{
		if (_marks[i].kind == kind && _marks[i].instigatorId == instigatorId)
		{
			_marks.erase(_marks.begin() + i);
			return true;
		}
	}
	return false;
}

void StatusSet::Clear()
{
	_dots.clear();
	_buffs.clear();
	_marks.clear();
}
