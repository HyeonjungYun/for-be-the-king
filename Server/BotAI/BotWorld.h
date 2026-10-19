#pragma once
#include <cstdint>
#include <string>
#include <vector>

/*-------------
	BotWorld
-------------*/

// 봇 AI 의 세계 모형 (2026-10-12 · design/quick-specs/bot-ai-2026-10-12.md).
// 판단(BotBrain)은 이것만 읽는다 - 패킷 · 세션 · 방을 모른다. DummyClient 는 받은 패킷으로 이것을 채우고,
// 나중에 서버 안의 PVE AI 플레이어는 Room 에서 직접 채운다 (같은 두뇌, 다른 감각 기관). 단위 cm · µs.
// 🔴 이 헤더는 ServerCore 도 protobuf 도 모른다 - GameServerTests 가 그대로 컴파일한다.

namespace BotAI
{
	using uint64 = std::uint64_t;
	using uint32 = std::uint32_t;
	using int32 = std::int32_t;

	enum class CreatureKind : std::uint8_t { Player, Monster };

	// 내가 아는 생물 하나 (시야 안 - 서버 FoW 가 보낸 것만)
	struct Creature
	{
		uint64			id = 0;
		CreatureKind	kind = CreatureKind::Player;
		float			x = 0.f;
		float			y = 0.f;
		int32			hp = 0;
		int32			maxHp = 1;
		bool			ally = false;			// 같은 파티 · 팀 - 때리지 않는다
		uint64			lastHitMeUs = 0;		// 이 생물이 나를 마지막으로 때린 시각 (0 = 없음)
		uint32			monsterTypeId = 0;		// 몬스터만
		float			power = 1.f;			// 나와 견준 힘 (1 = 비슷, 2 = 두 배). 모르면 1
		uint64			firstSeenUs = 0;		// 처음 보인 시각 - 반응 지연 (사람처럼 보자마자 덤비지 않는다). 0 = 오래전

		float			HpRatio() const { return maxHp > 0 ? static_cast<float>(hp) / static_cast<float>(maxHp) : 0.f; }
	};

	// 가방 · 상자
	struct Loot
	{
		uint64	id = 0;
		float	x = 0.f;
		float	y = 0.f;
		bool	chest = false;
		bool	triedUs = false;		// 이미 열어 봤다 (다시 안 간다)
	};

	struct Tower
	{
		std::string	id;
		float		x = 0.f;
		float		y = 0.f;
	};

	// 둘러볼 만한 곳 (상자 자리 · 집 · 타워) - 탐색이 무작위 지점 대신 고른다 (2026-10-20)
	struct Poi
	{
		float	x = 0.f;
		float	y = 0.f;
		uint32	kind = 0;		// 0 상자 자리 · 1 집 · 2 타워
	};

	// 이동 스킬의 몸 (Room::ApplySelfEffect 와 같은 갈래, 2026-10-20). Aim · Back · ToTarget 은 클라가 옮기고 서버는 거리 · 속도만 검증한다.
	// ServerMoves (blink · behind · to_anchor ...) 는 서버가 옮겨 보정 S_MOVE 를 보낸다 - 손발은 몸을 날리지 않는다
	enum class DashMode : std::uint8_t { None, Aim, Back, ToTarget, ServerMoves };

	// 스킬 칸의 쓰임 (SkillBook 이 skills.csv · skill_effects.csv 에서 가른다, 2026-10-20)
	enum class SkillRole : std::uint8_t
	{
		None,		// 패시브 · 벽 세우기 · 분신처럼 두뇌가 쓰지 않는 것
		Damage,		// 적을 다치게 한다
		Engage,		// 적에게 붙는다 (피해 돌진 · 대상 돌진)
		Dash,		// 내 몸만 옮긴다 (질주 · 블링크) - 도망 · 접근
		Buff,		// 대상 없는 자기 강화 - 싸움 직전
		Debuff,		// 피해 없이 적에게 거는 것 (표식 · 둔화 · 실명 · 침묵 · 끌기)
	};

	// 내가 쓸 수 있는 스킬 칸 (장비 동기화 + skills.csv)
	struct SkillSlot
	{
		uint32	slot = 0;				// Protocol::EquipSlot 값
		uint32	skillId = 0;
		float	rangeCm = 0.f;
		uint32	castMs = 0;
		uint32	cooldownMs = 0;
		uint64	readyAtUs = 0;			// 이 시각 전엔 쓰지 않는다
		bool	damage = false;			// 적을 다치게 한다
		bool	movement = false;		// 질주 · 블링크 (거리 벌리기 · 좁히기)
		bool	selfBuff = false;		// 대상 없음
		bool	needsTarget = false;	// single/target
		SkillRole	role = SkillRole::None;	// 0 이면 위 네 칸으로 옛 규칙 (테스트 · 손으로 만든 칸)
		float	reachCm = 0.f;			// 실제로 닿는 거리 - 내 둘레 스킬은 반경, 나머지는 사거리. 0 = rangeCm
		bool	recast = false;			// 다시 누르면 둘째 동작 (찌르고 베기 · 비창 ...)
		uint32	lockOthersMs = 0;		// 쓰면 그동안 다른 스킬을 못 쓴다 (통아 · 육량전 - basic_only 버프)
		// 내 몸을 옮기는 스킬 (skill_effects move · self) - 손발이 같은 거리 · 속도로 몸을 날린다 (서버는 그 창만 연다)
		int32	rangeBuffCm = 0;			// 평타 사거리 버프 (통아 +600 · 육량전 −500 · 단창 −120) - 손발이 그 시간 동안 사거리에 더한다
		uint32	rangeBuffMs = 0;
		DashMode	dashMode = DashMode::None;
		float	dashDistCm = 0.f;
		float	dashSpeedCms = 0.f;
	};

	struct Self
	{
		uint64	id = 0;
		float	x = 0.f;
		float	y = 0.f;
		int32	hp = 500;
		int32	maxHp = 500;
		float	moveSpeedCms = 340.f;
		uint32	weaponTypeId = 0;
		int32	carriedCount = 0;		// 소지 아이템 수
		int32	carriedValue = 0;		// 소지 가치 (등급 합) - 탈출 판단
		bool	hardCc = false;
		bool	casting = false;
		bool	dashing = false;
		uint64	runEndsAtUs = 0;		// 0 = 모른다
		uint32	gaugePermille = 0;		// 탈출 게이지 (내가 구역 안일 때)
		bool	inEscapeZone = false;
		uint64	lastDamagedUs = 0;
		float	attackRangeCm = 0.f;	// 무기 평타 사거리 (item_types.csv). 0 = 모른다 → attack_range_cm
		bool	ranged = false;			// 활 · 스태프 - 거리를 두고 쏜다
		uint64	attackReadyAtUs = 0;	// 다음 평타를 보낼 수 있는 시각 (손발이 안다). 0 = 지금

		float	HpRatio() const { return maxHp > 0 ? static_cast<float>(hp) / static_cast<float>(maxHp) : 0.f; }
	};

	struct World
	{
		uint64					nowUs = 0;
		Self					self;
		std::vector<Creature>	creatures;
		std::vector<Loot>		loot;
		std::vector<Tower>		towers;
		std::vector<SkillSlot>	skills;
		std::vector<Poi>		pois;
		// 걸을 수 있는지 (벽) - 없으면 늘 참. DummyClient 는 BotPath 격자로 답한다
		bool (*walkable)(float x, float y) = nullptr;
	};

	// 두뇌가 돌려주는 한 가지 행동 - 손발(DummyClient 패킷 · 서버 Room)이 실행한다
	enum class ActionType : std::uint8_t { Idle, MoveTo, Attack, UseSkill, OpenLoot, Escape };

	struct Action
	{
		ActionType	type = ActionType::Idle;
		float		x = 0.f;				// MoveTo · UseSkill(aim)
		float		y = 0.f;
		uint64		targetId = 0;			// Attack · UseSkill · OpenLoot
		uint32		slot = 0;				// UseSkill
		float		speedScale = 1.f;		// MoveTo - 1 = 달리기. 둘러볼 때는 느리게 (성격)
		const char*	branch = "idle";		// 어느 가지가 골랐나 (trace)
		const char*	reason = "";
	};
}
