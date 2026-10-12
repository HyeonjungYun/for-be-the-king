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
		const char*	branch = "idle";		// 어느 가지가 골랐나 (trace)
		const char*	reason = "";
	};
}
