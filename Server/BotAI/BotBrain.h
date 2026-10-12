#pragma once
#include "BehaviorTree.h"
#include <string>
#include <unordered_map>

/*-------------
	BotBrain
-------------*/

// 합리적인 플레이어 AI 의 밑거름 (2026-10-12 사용자 지시 - design/quick-specs/bot-ai-2026-10-12.md).
// 한 틱 = World 를 읽고 Action 하나를 돌려준다. 가지 우선순위:
//   살아남기(피 낮고 위협 가까움 → 도망) > 탈출(시간 · 소지 가치) > 싸움(이길 만한 적) > 피하기(센 적) > 줍기 > 탐색
// 수치는 bot_brain.csv (BrainConfig). 트리는 상태가 없고 Blackboard 만 기억한다 - 같은 World · Blackboard 면 같은 Action (테스트).

namespace BotAI
{
	struct BrainConfig
	{
		float	fleeHpRatio = 0.30f;			// 이 아래면 위협에서 도망
		float	engageHpRatio = 0.45f;			// 이 위여야 싸움을 건다 (아니면 피 낮은 적만)
		float	threatRadiusCm = 800.f;			// 이 안의 적대 생물 = 위협
		float	engageRadiusCm = 1500.f;		// 이 안의 적을 고른다
		float	avoidPowerRatio = 1.5f;			// 적의 힘이 내 힘의 이 배 이상이면 피한다
		float	attackRangeCm = 150.f;			// 평타 사거리 (무기 사거리를 모를 때)
		float	fleeDistanceCm = 1200.f;		// 도망 한 번에 가는 거리
		uint64	fleeHoldUs = 2'000'000;			// 도망 지점을 바꾸지 않는 시간
		float	lootRadiusCm = 2500.f;			// 이 안의 가방 · 상자만 줍는다
		float	lootReachCm = 170.f;			// 여기까지 가면 연다 (서버 180)
		int32	lootGoalValue = 6;				// 소지 가치가 이만큼이면 탈출
		uint64	escapeWhenLeftUs = 120'000'000;	// 런이 이만큼 남으면 탈출
		float	towerReachCm = 900.f;			// 구역 안 (서버 1000)
		float	wanderRadiusCm = 2000.f;		// 탐색 한 번의 거리
		uint64	wanderHoldUs = 6'000'000;		// 탐색 지점 유지
		float	monsterPackAvoid = 3.f;			// 몬스터가 이 수 이상 몰려 있으면 피한다
		float	monsterPowerEach = 0.35f;		// 몬스터 한 마리의 힘 (플레이어 1)
		float	monsterPackRadiusCm = 1200.f;	// 이 안의 몬스터를 한 무리로 센다
		uint64	recentHitUs = 5'000'000;		// 이 안에 나를 때린 적은 싸움 대상 점수 +1
		float	finishHpRatio = 0.30f;			// 내 피가 낮아도 이 아래의 적은 마무리한다
		float	arriveCm = 100.f;				// 탐색 지점에 이만큼 가까우면 도착

		// bot_brain.csv (key,value). 없는 키는 기본값. 모르는 키 · 숫자 아님 · 범위 밖은 거부
		bool	Parse(const std::string& csv, std::string& error);
	};

	class Brain
	{
	public:
		explicit Brain(const BrainConfig& config = BrainConfig());

		// 한 틱. out.branch 에 고른 가지 이름이 남는다
		Action	Decide(const World& world, Blackboard& bb) const;

		const BrainConfig&	Config() const { return _config; }

		// ── 판단 조각 (테스트 · 다른 트리가 재사용) ──
		static const Creature*	NearestThreat(const World& w, float radiusCm);		// 가장 가까운 적대 생물 (아군 제외)
		static float			PowerOf(const World& w, const Creature& c, const BrainConfig& cfg);	// 적의 힘 (몬스터는 무리 수를 센다)
		static const Creature*	BestTarget(const World& w, const BrainConfig& cfg);	// 싸울 만한 가장 좋은 적
		static const Loot*		NearestLoot(const World& w, float radiusCm);
		static const Tower*		NearestTower(const World& w);
		static const SkillSlot*	ReadySkill(const World& w, bool wantDamage, float distCm);	// 지금 쓸 수 있는 칸
		static bool				FleePoint(const World& w, const Creature& threat, float distanceCm, float& outX, float& outY, Blackboard& bb);

	private:
		NodePtr		Build() const;

		BrainConfig	_config;
		NodePtr		_root;
	};

	// skills.csv 의 봇이 알아야 할 열만 (skill_id,kind,shape,aim,range_cm,cast_ms,cooldown_ms,power) - 장비 동기화의 스킬 id 를 SkillSlot 으로
	struct SkillBook
	{
		struct Entry { uint32 skillId = 0; float rangeCm = 0.f; uint32 castMs = 0; uint32 cooldownMs = 0; bool damage = false; bool movement = false; bool selfBuff = false; bool needsTarget = false; };
		std::unordered_map<uint32, Entry> entries;

		bool	Parse(const std::string& skillsCsv, const std::string& effectsCsv, std::string& error);
		bool	Fill(uint32 slot, uint32 skillId, SkillSlot& out) const;		// 모르는 스킬이면 false
	};
}
