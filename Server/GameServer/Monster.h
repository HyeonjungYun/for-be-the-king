#pragma once
#include "Creature.h"
#include "MonsterBrain.h"

// 서버가 움직이는 몬스터 (design/gdd/monster-ai.md). 판정은 플레이어와 같은 Creature 공식 · 같은 전투 파이프라인이다.
// object_id 는 MONSTER_ID_BASE(40억) 밴드 - 리스폰마다 새 id (이전 생애의 늦은 피해가 새 개체에 붙지 않게)
constexpr uint64 MONSTER_ID_BASE = 4'000'000'000;

extern atomic<uint64> GNextMonsterId;

// G3 - 몬스터 판단 비용 · 이동 항목 수 (monster-ai.md F6 · F7, AC-MA-71). 10 초마다 [MON] 줄로 찍고 창을 비운다
struct MonsterStats
{
	atomic<int32>	alive = 0;
	atomic<uint64>	moveEntries = 0;		// S_MOVE 에 실린 몬스터 항목 수
	atomic<uint64>	aiTicks = 0;
	atomic<uint64>	aiTotalUs = 0;
	atomic<uint64>	aiMaxUs = 0;
	atomic<uint64>	hits = 0;				// 몬스터가 입힌 타격 (평타 · 스킬)
	atomic<uint64>	playerKills = 0;		// 몬스터가 죽인 플레이어
	atomic<uint64>	deaths = 0;				// 죽은 몬스터
	atomic<uint64>	resetSnaps = 0;			// T12 순간 이동 (OQ-2 판단 근거)

	// 길찾기 (§ K, 2026-10-17)
	atomic<uint64>	paths = 0;				// 길을 구한 횟수
	atomic<uint64>	pathFails = 0;
	atomic<uint64>	pathTotalUs = 0;
	atomic<uint64>	pathMaxUs = 0;
	atomic<uint64>	onPathSteps = 0;		// 꺾는 점을 따라 걸은 걸음 수
	atomic<uint64>	casts = 0;				// 몬스터 스킬 시전 (보스 · 돌진 · 화살)

	void	RecordAi(uint64 micros);
	void	RecordPath(bool found, uint64 micros);
	void	Dump(uint64 windowSeconds);
};

extern MonsterStats GMonsterStats;

class Monster : public Creature
{
public:
	Monster(uint64 objectId, const MonsterTypeDef& type, const MonsterSpawnDef& spawn);
	virtual ~Monster();

	// 만피 · CC 해제 (복귀 도착 T11)
	void		RestoreFull();

public:
	const MonsterTypeDef*	type = nullptr;
	const MonsterSpawnDef*	spawn = nullptr;

	MonsterBrainState		brain;
	BrainOutput				decision;			// 직전 판단 - 이동 스텝이 목표점으로 쓴다
	uint64					lastStepUs = 0;
	uint64					diedAtUs = 0;		// 0 = 살아 있음. 시체 유예 뒤 방에서 뺀다
	uint64					holdUntilUs = 0;	// 몬스터 스킬의 예고 · 돌진이 끝날 때까지 서 있는다 (§ L 돌진 9002 - 시전 + 예고 + 날아가는 시간)

	// 테스트 모드 더미 (design/quick-specs/test-mode-2026-10-11.md) - /t spawn 으로 세운 몬스터. 자리(spawn)는 방이 따로 들고,
	// 리스폰 · 드랍이 없다. aiFixed = 판단을 건너뛰어 그 자리에 서 있기만 한다 (맞고 밀리는 것은 그대로)
	bool					isDummy = false;
	bool					aiFixed = false;
	uint64					dummyOwnerId = 0;

	// 베이스캠프 허수아비 (2026-10-17) - 주인 없음(주인 없는 더미 청소에서 빠진다) · 죽지 않음 · 맞고 잠깐 뒤 만피
	bool					practice = false;

	// 무리 (2026-10-11 몬스터 개선) - 0 = 혼자 (예전 두뇌). 아니면 Room::_packs 의 위치 + 1. packIndex = 줄 순서 (0 = 태어날 때의 우두머리)
	uint32					packSlot = 0;
	uint8					packIndex = 0;
};
