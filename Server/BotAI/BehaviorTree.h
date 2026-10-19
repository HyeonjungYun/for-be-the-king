#pragma once
#include "BotWorld.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

/*------------------
	BehaviorTree
------------------*/

// 아주 작은 행동 트리 (2026-10-12). 노드는 상태가 없고 매 틱 뿌리부터 돈다 - 기억은 Blackboard 에만.
// Selector = 첫 성공, Sequence = 전부 성공, Condition/Action = 람다. Running 은 Success 처럼 멈춘다(이번 틱의 행동이 정해졌다).

namespace BotAI
{
	// 봇 한 명의 성격 (2026-10-20 · bot_personas.csv). 기본값 = 옛 두뇌 그대로 (배율 1 · 지연 0 · 옆걸음 없음)
	struct Persona
	{
		std::string	name = "default";
		float	fleeHpScale = 1.f;			// flee_hp_ratio × - 높으면 일찍 도망
		float	engageHpScale = 1.f;		// engage_hp_ratio × - 낮으면 피가 적어도 덤빈다
		float	avoidPowerScale = 1.f;		// avoid_power_ratio × - 높으면 센 적에게도 덤빈다
		float	engageRadiusScale = 1.f;	// engage_radius_cm ×
		float	lootRadiusScale = 1.f;		// loot_radius_cm ×
		int32	lootGoalBonus = 0;			// loot_goal_value +
		float	escapeEarlyScale = 1.f;		// escape_when_left_s ×
		uint64	reactionUs = 0;				// 처음 본 적에게 반응하기까지 (나를 때린 적은 바로)
		float	strafe = 0.f;				// 0..1 - 평타 사이 옆걸음 (근접은 돌고, 원거리는 비켜 선다)
		float	exploreSpeed = 1.f;			// 둘러볼 때 걸음 (1 = 달리기)
		float	preferPlayers = 0.f;		// 싸움 대상 점수 + (플레이어)
		float	preferMonsters = 0.f;		// 싸움 대상 점수 + (몬스터)
		float	pauseChance = 0.f;			// 탐색 지점에 닿았을 때 멈춰 둘러볼 확률
		uint64	pauseMinUs = 0;
		uint64	pauseMaxUs = 0;
		float	poiBias = 0.f;				// 탐색 지점을 상자 자리 · 집 · 타워에서 고를 확률
		float	woundedEscapeHp = 0.f;		// 이 아래 체력 + 소지품이 있으면 탈출 (0 = 안 함)
		int32	outnumberedAt = 0;			// 적 플레이어가 이 수 이상 몰려 있으면 싸우지 않고 피한다 (0 = 세지 않음)
		uint64	chaseMemoryUs = 0;			// 시야에서 사라진 대상을 마지막 자리까지 쫓는 시간 (0 = 안 쫓음)
	};

	struct Blackboard
	{
		uint64	targetId = 0;			// 쫓는 대상
		uint64	lootId = 0;				// 가려는 가방 · 상자
		float	fleeX = 0.f;			// 도망 지점
		float	fleeY = 0.f;
		uint64	fleeUntilUs = 0;
		float	wanderX = 0.f;			// 탐색 지점
		float	wanderY = 0.f;
		uint64	wanderUntilUs = 0;
		uint64	lastSkillUs = 0;
		std::string	lastBranch;			// 지난 틱의 가지 (바뀔 때만 로그)
		uint32	seed = 1;				// 탐색 난수 (결정적)

		// 2026-10-20 - 성격 · 기억
		Persona	persona;
		float	lastSeenX = 0.f;		// 싸우던 대상을 마지막으로 본 자리 (시야 밖으로 사라지면 거기까지 가 본다)
		float	lastSeenY = 0.f;
		uint64	lastSeenUs = 0;
		uint64	lastSeenId = 0;
		uint64	pauseUntilUs = 0;		// 탐색 지점에서 멈춰 둘러보는 중
		float	strafeSign = 1.f;		// 옆걸음 방향 (가끔 뒤집는다)
		uint64	strafeFlipUs = 0;
		uint64	lastPoiIndex = UINT64_MAX;	// 방금 다녀온 곳은 다시 고르지 않는다

		uint32	NextRandom() { seed = seed * 1664525u + 1013904223u; return seed; }
		float	Random01() { return static_cast<float>(NextRandom() % 10000u) / 10000.f; }
	};

	struct Context
	{
		const World&	world;
		Blackboard&		bb;
		Action&			out;
	};

	enum class Status : std::uint8_t { Success, Failure, Running };

	class Node
	{
	public:
		explicit Node(std::string name) : _name(std::move(name)) {}
		virtual ~Node() = default;
		virtual Status Tick(Context& ctx) = 0;
		const std::string& Name() const { return _name; }

	protected:
		std::string _name;
	};
	using NodePtr = std::shared_ptr<Node>;

	class Composite : public Node
	{
	public:
		using Node::Node;
		Composite& Add(NodePtr child) { _children.push_back(std::move(child)); return *this; }
		const std::vector<NodePtr>& Children() const { return _children; }

	protected:
		std::vector<NodePtr> _children;
	};

	// 첫 성공(또는 Running)에서 멈춘다
	class Selector : public Composite
	{
	public:
		using Composite::Composite;
		Status Tick(Context& ctx) override
		{
			for (NodePtr& child : _children)
			{
				const Status s = child->Tick(ctx);
				if (s != Status::Failure)
					return s;
			}
			return Status::Failure;
		}
	};

	// 첫 실패에서 멈춘다
	class Sequence : public Composite
	{
	public:
		using Composite::Composite;
		Status Tick(Context& ctx) override
		{
			for (NodePtr& child : _children)
			{
				const Status s = child->Tick(ctx);
				if (s != Status::Success)
					return s;
			}
			return Status::Success;
		}
	};

	class Condition : public Node
	{
	public:
		Condition(std::string name, std::function<bool(Context&)> fn) : Node(std::move(name)), _fn(std::move(fn)) {}
		Status Tick(Context& ctx) override { return _fn(ctx) ? Status::Success : Status::Failure; }

	private:
		std::function<bool(Context&)> _fn;
	};

	// 행동을 적고 성공한다 (실패하면 Failure - 다음 가지로)
	class ActionNode : public Node
	{
	public:
		ActionNode(std::string name, std::function<bool(Context&)> fn) : Node(std::move(name)), _fn(std::move(fn)) {}
		Status Tick(Context& ctx) override
		{
			if (_fn(ctx) == false)
				return Status::Failure;
			ctx.out.branch = _name.c_str();
			return Status::Success;
		}

	private:
		std::function<bool(Context&)> _fn;
	};

	class Inverter : public Node
	{
	public:
		Inverter(std::string name, NodePtr child) : Node(std::move(name)), _child(std::move(child)) {}
		Status Tick(Context& ctx) override
		{
			const Status s = _child->Tick(ctx);
			if (s == Status::Running)
				return s;
			return s == Status::Success ? Status::Failure : Status::Success;
		}

	private:
		NodePtr _child;
	};

	inline NodePtr MakeSelector(std::string name) { return std::make_shared<Selector>(std::move(name)); }
	inline NodePtr MakeSequence(std::string name) { return std::make_shared<Sequence>(std::move(name)); }
	inline NodePtr MakeCondition(std::string name, std::function<bool(Context&)> fn) { return std::make_shared<Condition>(std::move(name), std::move(fn)); }
	inline NodePtr MakeAction(std::string name, std::function<bool(Context&)> fn) { return std::make_shared<ActionNode>(std::move(name), std::move(fn)); }
	inline Composite& AsComposite(const NodePtr& node) { return *static_cast<Composite*>(node.get()); }
}
