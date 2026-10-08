#pragma once

/*---------------
	KillBatch
---------------*/

// 전투 Flush 하나(33 ms) 안의 사망을 모았다가 Flush 끝에 한꺼번에 처리한다 (combat-system.md TR-combat-022 "동시 치사는 양쪽 모두 사망").
//
// 예전에는 대상 체력이 0 이 되는 순간 OnCreatureKilled 가 그 대상의 평타 · 시전을 지웠다. 같은 Flush 에서 서로를 죽이는 두 평타는
// 반복 순서상 먼저 해소된 쪽만 들어가고 뒤쪽은 "공격자 사망" 으로 취소됐다 (ADR-0008 R-3).
// 이제 같은 Flush 에서 해소 시각이 된 히트는 동시다 - 그 Flush 에 죽은 쪽도 이미 때가 된 자기 평타 · 스킬은 그대로 들어간다.
//
// Room 에 묶이지 않은 순수 자료구조 - Ref 는 Room 에선 CreatureRef, 테스트에선 아무 값이나.

template <typename Ref>
class KillBatch
{
public:
	struct Kill
	{
		uint64	victimId = 0;
		uint64	instigatorId = 0;
		Ref		victim{};
		Ref		instigator{};
		bool	mutual = false;		// 같은 Flush 에서 상대도 나를 죽였다
	};

	void Begin()
	{
		_open = true;
		_kills.clear();
		_died.clear();
	}

	bool IsOpen() const { return _open; }

	// 이번 Flush 에 죽었다 - 아직 처리 전이라 이미 해소 시각이 된 자기 히트는 들어간다
	bool DiedThisFlush(uint64 id) const { return _died.count(id) > 0; }

	// 처음 죽은 것만 남긴다. 이미 죽은 대상에게 다시 들어온 사망은 false (부른 쪽이 무시)
	bool Note(uint64 victimId, uint64 instigatorId, Ref victim, Ref instigator)
	{
		if (_died.insert(victimId).second == false)
			return false;

		Kill& kill = _kills.emplace_back();
		kill.victimId = victimId;
		kill.instigatorId = instigatorId;
		kill.victim = move(victim);
		kill.instigator = move(instigator);
		return true;
	}

	// 닫고 기록 순서 그대로 돌려준다. 서로를 죽인 쌍은 둘 다 mutual
	vector<Kill> End()
	{
		_open = false;

		for (Kill& kill : _kills)
		{
			for (const Kill& other : _kills)
			{
				if (other.victimId == kill.instigatorId && other.instigatorId == kill.victimId)
				{
					kill.mutual = true;
					break;
				}
			}
		}

		_died.clear();
		return move(_kills);
	}

private:
	bool					_open = false;
	vector<Kill>			_kills;
	unordered_set<uint64>	_died;
};
