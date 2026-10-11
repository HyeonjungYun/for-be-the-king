#pragma once
#include "SkillTable.h"

/*-----------------
	StatusEffects
------------------*/

// 스킬이 남기는 상태 - 지속 피해 · 자기 버프 · 표식 (design/quick-specs/skill-roster-2026-10-10.md 1 단계).
// Creature 하나가 StatusSet 하나를 가진다. 순수 자료구조 - Room 이 시각을 넘겨 틱 · 만료를 묻고 패킷(S_STATUS)은 Room 이 만든다.
// 규칙 수치(틱 · 중첩 상한)는 skill_rules.csv - 여기엔 없다. GameServerTests StatusEffectsTest 가 검사한다.
//
// 사용자 결정 (2026-10-10): 지속 피해는 같은 종류도 중첩(상한은 규칙) · 표식은 본인 것만 쓰고 중첩 없이 갱신 ·
// 버프는 같은 것이면 갱신, 크기는 아이템 강화 레벨로 (크기 계산은 Room - 여기는 받은 값을 든다)

struct DotInstance
{
	uint32					skillId = 0;
	string					kind;					// bleed · poison · burn · heavy_bleed (skill_effects.csv param)
	uint64					instigatorId = 0;
	float					baseDamage = 0.f;		// 틱당 완화 전 피해 - 걸 때 시전자 스탯으로 고정한다 (나중에 버프가 꺼져도 그대로)
	Protocol::DamageType	type = Protocol::DAMAGE_TYPE_MAGIC;
	uint64					endUs = 0;
	uint64					nextTickUs = 0;
};

struct BuffInstance
{
	uint32					skillId = 0;
	SkillBuffStat			stat = SkillBuffStat::None;
	float					magnitude = 0.f;		// 강화 레벨 반영 뒤 값 (0.15 = +15 %). 음수 = 자기 디버프 (광전사의 눈물 방어력)
	uint64					endUs = 0;
	string					param;					// Immune 의 속성 · DamageMod 의 선택자. 같은 skill + stat + param 이 갱신 단위
};

struct MarkInstance
{
	string					kind;					// spear · arrow · blood · heavy_bleed · rope
	uint64					instigatorId = 0;
	uint64					endUs = 0;
};

class StatusSet
{
public:
	// 같은 종류가 maxStacks 개면 가장 먼저 끝나는 것을 새 것으로 바꾼다. 돌려주는 값 = 그 종류의 중첩 수
	uint32	AddDot(const DotInstance& dot, uint32 maxStacks);

	// 장판 틱 (3 단계) - 같은 스킬 · 같은 주인 · 같은 종류가 이미 있으면 그것을 갱신(끝 시각 · 피해), 없으면 AddDot.
	// 장판 위에 서 있는 동안 틱마다 중첩이 쌓이지 않게 한다. 돌려주는 값 = 그 종류의 중첩 수
	uint32	RefreshOrAddDot(const DotInstance& dot, uint32 maxStacks);
	uint32	DotStacks(const string& kind) const;

	// nowUs 에 틱이 돌아온 인스턴스마다 fn(const DotInstance&) 를 부른다. 한 호출에 인스턴스당 최대 한 틱 -
	// Flush 가 밀려도 틱을 몰아 터뜨리지 않는다. 끝난(endUs 지난) 것은 틱하지 않는다 (Expire 가 뺀다)
	template <typename Fn>
	void	TickDots(uint64 nowUs, uint32 tickMs, Fn&& fn)
	{
		for (DotInstance& d : _dots)
		{
			if (nowUs >= d.endUs || nowUs < d.nextTickUs)
				continue;

			fn(static_cast<const DotInstance&>(d));
			d.nextTickUs += static_cast<uint64>(tickMs) * 1000;
		}
	}

	// 같은 skill + stat + param 은 갱신 (지속 · 크기 모두 새 값). true = 새로 생겼다
	bool	AddBuff(const BuffInstance& buff);
	float	BuffSum(SkillBuffStat stat) const;
	float	BuffSum(SkillBuffStat stat, const string& param) const;		// param 이 같은 것만
	float	BuffMax(SkillBuffStat stat) const;							// 가장 큰 크기 (없으면 0) - 평타 모드처럼 합치면 안 되는 것
	const BuffInstance* FindBuff(SkillBuffStat stat) const;				// 그 스탯의 첫 버프 (없으면 nullptr)
	bool	HasBuff(SkillBuffStat stat, const string& param) const;
	// 유령 신부의 축복 - wall_pierce 는 자기 버프(SelfBuff)라 param 이 없다. HasBuff(stat, "wall_pierce") 로 찾던 것은
	// 한 번도 맞지 않아 축복이 아무것도 하지 않았다 (2026-10-11 실기동에서 발견)
	bool	IgnoresWalls() const;
	// 한 스킬의 버프를 전부 뺀다 (배수진 해제). 돌려주는 값 = 뺀 수
	uint32	RemoveBuffs(uint32 skillId);
	bool	RemoveBuff(uint32 skillId, SkillBuffStat stat);			// 그 스킬의 그 스탯 버프 하나 (건곤일척 - 방어력 감소는 남기고 피해 증가만)

	// 모자 로스터 - 지속 피해 손보기
	uint32	RemoveDots(const string& kind);									// 그 종류 전부 (빈 kind = 전부)
	uint32	ExtendDots(Protocol::DamageType type, uint32 extraMs);				// 그 피해 종류의 남은 시간 연장. 돌려주는 값 = 손댄 수

	// 같은 종류 + 같은 주인은 갱신. true = 새로 생겼다
	bool	AddMark(const MarkInstance& mark);
	bool	HasMark(const string& kind, uint64 instigatorId) const;
	bool	ConsumeMark(const string& kind, uint64 instigatorId);

	// 내게 남은 표식 전부 (누가 남겼든) - 삼십육계 (2026-10-11). 지우기 전에 fn(mark) 를 부른다. 돌려주는 값 = 지운 수
	template <typename Fn>
	uint32	RemoveAllMarks(Fn&& fn)
	{
		for (const MarkInstance& m : _marks)
			fn(m);
		const uint32 n = static_cast<uint32>(_marks.size());
		_marks.clear();
		return n;
	}

	// 끝난 것을 빼며 fn(Protocol::StatusKind, const string& param, uint64 instigatorId, uint32 remainingStacks) 를 부른다.
	// 돌려주는 값 = 버프가 하나라도 빠졌다 (스탯 재계산 필요)
	template <typename Fn>
	bool	Expire(uint64 nowUs, Fn&& fn)
	{
		for (size_t i = 0; i < _dots.size(); )
		{
			if (nowUs < _dots[i].endUs)
			{
				i++;
				continue;
			}

			const DotInstance gone = _dots[i];
			_dots.erase(_dots.begin() + i);
			fn(Protocol::STATUS_DOT, gone.kind, gone.instigatorId, DotStacks(gone.kind));
		}

		bool buffGone = false;
		for (size_t i = 0; i < _buffs.size(); )
		{
			if (nowUs < _buffs[i].endUs)
			{
				i++;
				continue;
			}

			const BuffInstance gone = _buffs[i];
			_buffs.erase(_buffs.begin() + i);
			buffGone = true;
			fn(Protocol::STATUS_BUFF, gone.param.empty() ? string(SkillTable::BuffStatName(gone.stat)) : string(SkillTable::BuffStatName(gone.stat)) + ":" + gone.param, 0, 0);
		}

		for (size_t i = 0; i < _marks.size(); )
		{
			if (nowUs < _marks[i].endUs)
			{
				i++;
				continue;
			}

			const MarkInstance gone = _marks[i];
			_marks.erase(_marks.begin() + i);
			fn(Protocol::STATUS_MARK, gone.kind, gone.instigatorId, 0);
		}

		return buffGone;
	}

	bool	Empty() const { return _dots.empty() && _buffs.empty() && _marks.empty(); }
	void	Clear();

	const vector<DotInstance>&	Dots() const { return _dots; }
	const vector<BuffInstance>&	Buffs() const { return _buffs; }
	const vector<MarkInstance>&	Marks() const { return _marks; }

private:
	vector<DotInstance>		_dots;
	vector<BuffInstance>	_buffs;
	vector<MarkInstance>	_marks;
};
