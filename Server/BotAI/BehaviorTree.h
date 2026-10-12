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
