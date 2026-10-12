#include "pch.h"
#include "../BotAI/BotBrain.h"
#include "CsvTable.h"
#include "ItemTable.h"

// 봇 두뇌 (2026-10-12 · design/quick-specs/bot-ai-2026-10-12.md § AC). 세계를 손으로 만들어 넣고 행동 하나를 확인한다.
// 트리는 상태가 없다 - 같은 세계 · 같은 칠판이면 같은 행동.

namespace
{
	using namespace BotAI;
	constexpr uint64 SEC = 1'000'000;

	World Base()
	{
		World w;
		w.nowUs = 100 * SEC;
		w.self.id = 1;
		w.self.x = 0.f;
		w.self.y = 0.f;
		w.self.hp = 500;
		w.self.maxHp = 500;
		w.self.runEndsAtUs = w.nowUs + 1500 * SEC;
		w.towers.push_back({ "Ta", 10000.f, 0.f });
		return w;
	}

	// 서버의 전역 Creature 클래스와 이름이 겹친다 (pch) - 여기선 항상 BotAI::Creature
	BotAI::Creature Enemy(uint64 id, float x, float y, int32 hp = 500, int32 maxHp = 500, CreatureKind kind = CreatureKind::Player)
	{
		BotAI::Creature c;
		c.id = id;
		c.kind = kind;
		c.x = x;
		c.y = y;
		c.hp = hp;
		c.maxHp = maxHp;
		return c;
	}

	float Dist(float ax, float ay, float bx, float by) { return ::sqrtf((ax - bx) * (ax - bx) + (ay - by) * (ay - by)); }
}

TEST(BotBrain, 아무것도_없으면_탐색하고_같은_세계면_같은_행동이다_AC_AI_01)
{
	Brain brain;
	World w = Base();
	Blackboard bb;
	const Action a = brain.Decide(w, bb);
	EXPECT_EQ(a.type, ActionType::MoveTo);
	EXPECT_STREQ(a.branch, "explore");
	EXPECT_GT(Dist(0.f, 0.f, a.x, a.y), 500.f);

	Blackboard bb2;
	const Action b = brain.Decide(w, bb2);
	EXPECT_FLOAT_EQ(a.x, b.x);
	EXPECT_FLOAT_EQ(a.y, b.y);

	// 지점을 바꾸지 않는다 (hold 안)
	const Action c = brain.Decide(w, bb);
	EXPECT_FLOAT_EQ(a.x, c.x);
	EXPECT_FLOAT_EQ(a.y, c.y);
}

TEST(BotBrain, 피가_낮고_위협이_가까우면_반대쪽으로_도망간다_AC_AI_02)
{
	Brain brain;
	World w = Base();
	w.self.hp = 100;		// 20 %
	w.creatures.push_back(Enemy(2, 500.f, 0.f));
	Blackboard bb;
	const Action a = brain.Decide(w, bb);
	EXPECT_EQ(a.type, ActionType::MoveTo);
	EXPECT_STREQ(a.branch, "flee");
	EXPECT_LT(a.x, -500.f) << "away from the enemy at +x";
	EXPECT_GT(Dist(500.f, 0.f, a.x, a.y), Dist(500.f, 0.f, 0.f, 0.f));

	// 이동 스킬이 있으면 그걸로 도망간다
	SkillSlot dash;
	dash.slot = 5;
	dash.skillId = 1003;
	dash.movement = true;
	w.skills.push_back(dash);
	Blackboard bb2;
	const Action b = brain.Decide(w, bb2);
	EXPECT_EQ(b.type, ActionType::UseSkill);
	EXPECT_EQ(b.slot, 5u);

	// 경계값 - 30 % 는 도망이 아니다 (< 30)
	w.skills.clear();
	w.self.hp = 150;
	Blackboard bb3;
	EXPECT_STRNE(brain.Decide(w, bb3).branch, "flee");
	// 위협이 반경 밖이면 도망이 아니다
	w.self.hp = 100;
	w.creatures[0].x = 801.f;
	Blackboard bb4;
	EXPECT_STRNE(brain.Decide(w, bb4).branch, "flee");
}

TEST(BotBrain, 시간이_얼마_안_남거나_가방이_차면_타워로_가고_구역에서는_선다_AC_AI_03)
{
	Brain brain;
	World w = Base();
	w.self.runEndsAtUs = w.nowUs + 100 * SEC;		// 100 s 남음 < 120
	Blackboard bb;
	Action a = brain.Decide(w, bb);
	EXPECT_EQ(a.type, ActionType::MoveTo);
	EXPECT_STREQ(a.branch, "escape");
	EXPECT_FLOAT_EQ(a.x, 10000.f);

	w.self.x = 9200.f;		// 구역 안 (900)
	a = brain.Decide(w, bb);
	EXPECT_EQ(a.type, ActionType::Escape);

	// 시간은 넉넉한데 가방이 찼다
	World r = Base();
	r.self.carriedValue = 6;
	a = brain.Decide(r, bb);
	EXPECT_STREQ(a.branch, "escape");
	r.self.carriedValue = 5;
	EXPECT_STRNE(brain.Decide(r, bb).branch, "escape");

	// 탈출이 싸움보다 먼저다 - 약한 적이 옆에 있어도
	w.self.x = 0.f;
	w.creatures.push_back(Enemy(2, 300.f, 0.f, 50));
	a = brain.Decide(w, bb);
	EXPECT_STREQ(a.branch, "escape");
}

TEST(BotBrain, 이길_만한_적은_다가가서_때리고_스킬이_되면_스킬을_쓴다_AC_AI_04)
{
	Brain brain;
	World w = Base();
	w.creatures.push_back(Enemy(2, 1000.f, 0.f));
	Blackboard bb;
	Action a = brain.Decide(w, bb);
	EXPECT_EQ(a.type, ActionType::MoveTo);
	EXPECT_STREQ(a.branch, "fight");
	EXPECT_EQ(a.targetId, 2u);

	w.creatures[0].x = 140.f;		// 평타 사거리 150 안
	a = brain.Decide(w, bb);
	EXPECT_EQ(a.type, ActionType::Attack);
	EXPECT_EQ(a.targetId, 2u);
	// 경계 양쪽 (attack_range_cm 150): 150 은 때리고 151 은 다가간다
	w.creatures[0].x = 150.f;
	EXPECT_EQ(brain.Decide(w, bb).type, ActionType::Attack);
	w.creatures[0].x = 151.f;
	EXPECT_EQ(brain.Decide(w, bb).type, ActionType::MoveTo);

	SkillSlot s;
	s.slot = 1;
	s.skillId = 1004;
	s.damage = true;
	s.rangeCm = 500.f;
	w.skills.push_back(s);
	w.creatures[0].x = 400.f;
	a = brain.Decide(w, bb);
	EXPECT_EQ(a.type, ActionType::UseSkill);
	EXPECT_EQ(a.slot, 1u);
	// 쿨다운 중이면 다가간다
	w.skills[0].readyAtUs = w.nowUs + SEC;
	a = brain.Decide(w, bb);
	EXPECT_EQ(a.type, ActionType::MoveTo);
	// 사거리 밖이면 스킬을 쓰지 않는다
	w.skills[0].readyAtUs = 0;
	w.creatures[0].x = 501.f;
	EXPECT_EQ(brain.Decide(w, bb).type, ActionType::MoveTo);

	// 피가 45 % 아래면 피가 많은 적과 싸우지 않는다 (피가 30 % 아래인 적은 마무리한다)
	w.self.hp = 200;
	w.creatures[0].x = 300.f;
	EXPECT_STRNE(brain.Decide(w, bb).branch, "fight");		// avoid - 도망 지점을 정한다
	// 도망 지점은 flee_hold(2 s) 동안 유지한다 - 그 사이 적이 약해져도 계속 간다 (반경 경계에서 가지가 틱마다 바뀌는 떨림 방지, 실기동 2026-10-12)
	w.creatures[0].hp = 100;
	EXPECT_STREQ(brain.Decide(w, bb).branch, "flee");
	EXPECT_STREQ(brain.Decide(w, bb).reason, "keep fleeing");
	w.nowUs += 3 * SEC;
	EXPECT_STREQ(brain.Decide(w, bb).branch, "fight");
}

TEST(BotBrain, 센_적이나_무리는_피하고_아군은_상대하지_않는다_AC_AI_05)
{
	Brain brain;
	World w = Base();
	BotAI::Creature strong = Enemy(2, 600.f, 0.f);
	strong.power = 3.f;		// 내 1.5 배 이상
	w.creatures.push_back(strong);
	Blackboard bb;
	Action a = brain.Decide(w, bb);
	EXPECT_STREQ(a.branch, "avoid");
	EXPECT_EQ(a.type, ActionType::MoveTo);
	EXPECT_LT(a.x, 0.f);

	// 몬스터 무리 4 마리 = 힘 0.35 × 4 × 1.5 = 2.1 ≥ 1.5 × (0.5 + 1) ✗ → 2.1 < 2.25 싸운다. 5 마리면 2.6 → 피한다
	World m = Base();
	for (uint64 i = 0; i < 5; i++)
		m.creatures.push_back(Enemy(10 + i, 700.f + static_cast<float>(i) * 100.f, 0.f, 60, 60, CreatureKind::Monster));
	Blackboard bb2;
	EXPECT_STREQ(brain.Decide(m, bb2).branch, "avoid");
	m.creatures.pop_back();		// 4 마리 = 2.1 < 2.25 → 싸운다 (경계 양쪽: 5 피함 · 4 싸움)
	Blackboard bbFour;
	EXPECT_STREQ(brain.Decide(m, bbFour).branch, "fight");
	m.creatures.pop_back();
	Blackboard bb3;
	EXPECT_STREQ(brain.Decide(m, bb3).branch, "fight");

	// 아군은 위협도 대상도 아니다
	World f = Base();
	BotAI::Creature ally = Enemy(3, 200.f, 0.f);
	ally.ally = true;
	f.creatures.push_back(ally);
	f.self.hp = 50;
	Blackboard bb4;
	EXPECT_STREQ(brain.Decide(f, bb4).branch, "explore");
}

TEST(BotBrain, 적이_없으면_가까운_가방으로_가서_연다_AC_AI_06)
{
	Brain brain;
	World w = Base();
	w.loot.push_back({ 50, 900.f, 0.f, false, false });
	w.loot.push_back({ 51, 300.f, 0.f, true, true });		// 이미 열어 본 상자
	Blackboard bb;
	Action a = brain.Decide(w, bb);
	EXPECT_STREQ(a.branch, "loot");
	EXPECT_EQ(a.type, ActionType::MoveTo);
	EXPECT_EQ(a.targetId, 50u);

	w.self.x = 750.f;		// 150 cm - 손이 닿는다 (170)
	a = brain.Decide(w, bb);
	EXPECT_EQ(a.type, ActionType::OpenLoot);
	// 경계 양쪽 (loot_reach_cm 170): 170 은 열고 171 은 걷는다
	w.self.x = 730.f;
	EXPECT_EQ(brain.Decide(w, bb).type, ActionType::OpenLoot);
	w.self.x = 729.f;
	EXPECT_EQ(brain.Decide(w, bb).type, ActionType::MoveTo);
	EXPECT_EQ(a.targetId, 50u);

	// 반경 밖 (2500) 이면 탐색
	World farWorld = Base();		// 'far' 는 Windows 매크로
	farWorld.loot.push_back({ 52, 2600.f, 0.f, false, false });
	Blackboard bb2;
	EXPECT_STREQ(brain.Decide(farWorld, bb2).branch, "explore");
}

TEST(BotBrain, 묶이면_서고_죽으면_아무것도_안_한다_AC_AI_07)
{
	Brain brain;
	World w = Base();
	w.self.hardCc = true;
	w.creatures.push_back(Enemy(2, 200.f, 0.f));
	Blackboard bb;
	EXPECT_EQ(brain.Decide(w, bb).type, ActionType::Idle);
	EXPECT_STREQ(brain.Decide(w, bb).branch, "stunned");
	w.self.hardCc = false;
	w.self.hp = 0;
	EXPECT_STREQ(brain.Decide(w, bb).branch, "dead");
	// 벽이면 도망 · 탐색 지점을 돌려 잡는다
	World b = Base();
	b.self.hp = 50;
	b.creatures.push_back(Enemy(2, 500.f, 0.f));
	b.walkable = [](float x, float y) { return x > -100.f; };		// 서쪽은 벽
	Blackboard bb2;
	const Action a = brain.Decide(b, bb2);
	EXPECT_STREQ(a.branch, "flee");
	EXPECT_GT(a.x, -100.f);
}

TEST(BotBrainConfig, 표는_키_값이고_범위_밖과_모르는_키는_거부한다_AC_AI_08)
{
	BrainConfig cfg;
	std::string error;
	EXPECT_TRUE(cfg.Parse("key,value\nflee_hp_ratio,0.5\nescape_when_left_s,60\nloot_goal_value,3\n", error)) << error;
	EXPECT_FLOAT_EQ(cfg.fleeHpRatio, 0.5f);
	EXPECT_EQ(cfg.escapeWhenLeftUs, 60ull * SEC);
	EXPECT_EQ(cfg.lootGoalValue, 3);
	EXPECT_FLOAT_EQ(cfg.engageHpRatio, 0.45f);		// 없는 키는 기본값
	EXPECT_FALSE(cfg.Parse("key,value\nflee_hp_ratio,1.5\n", error));
	EXPECT_FALSE(cfg.Parse("key,value\nfly_ratio,0.5\n", error));
	EXPECT_FALSE(cfg.Parse("key,value\nflee_hp_ratio,abc\n", error));
	// 거부 범위 = 스펙 § 7 (검증자 수정 3) - 키마다 경계 양쪽
	EXPECT_TRUE(cfg.Parse("key,value\nthreat_radius_cm,5000\n", error)) << error;
	EXPECT_FALSE(cfg.Parse("key,value\nthreat_radius_cm,5001\n", error));
	EXPECT_FALSE(cfg.Parse("key,value\nthreat_radius_cm,99\n", error));
	EXPECT_TRUE(cfg.Parse("key,value\nloot_reach_cm,50\n", error)) << error;
	EXPECT_FALSE(cfg.Parse("key,value\nloot_reach_cm,49\n", error));
	EXPECT_FALSE(cfg.Parse("key,value\nloot_reach_cm,301\n", error));
	EXPECT_TRUE(cfg.Parse("key,value\nmonster_pack_avoid,10\n", error)) << error;
	EXPECT_FALSE(cfg.Parse("key,value\nmonster_pack_avoid,11\n", error));
	EXPECT_TRUE(cfg.Parse("key,value\nwander_hold_s,1\n", error)) << error;
	EXPECT_FALSE(cfg.Parse("key,value\nwander_hold_s,0\n", error));
	EXPECT_TRUE(cfg.Parse("key,value\nfinish_hp_ratio,0.3\nrecent_hit_s,5\nmonster_pack_radius_cm,1200\narrive_cm,100\n", error)) << error;
	EXPECT_FLOAT_EQ(cfg.finishHpRatio, 0.3f);
	EXPECT_EQ(cfg.recentHitUs, 5ull * SEC);
	EXPECT_FALSE(cfg.Parse("key,value\narrive_cm,9\n", error));
	EXPECT_FALSE(cfg.Parse("", error));

	// 실제 파일
	string text;
	ASSERT_TRUE(Csv::ReadFile(ItemTable::DefaultDataDir() + "\\bot_brain.csv", OUT text));
	EXPECT_TRUE(cfg.Parse(text, error)) << error;
}

TEST(BotSkillBook, skills_csv_에서_피해_이동_대상_여부를_읽는다_AC_AI_09)
{
	SkillBook book;
	std::string error;
	string skills, effects;
	ASSERT_TRUE(Csv::ReadFile(ItemTable::DefaultDataDir() + "\\skills.csv", OUT skills));
	ASSERT_TRUE(Csv::ReadFile(ItemTable::DefaultDataDir() + "\\skill_effects.csv", OUT effects));
	ASSERT_TRUE(book.Parse(skills, effects, error)) << error;
	EXPECT_GE(book.entries.size(), 180u);

	SkillSlot s;
	ASSERT_TRUE(book.Fill(1, 1005, s));		// 머리 부수기 - 대상 하나
	EXPECT_TRUE(s.damage);
	EXPECT_TRUE(s.needsTarget);
	EXPECT_GT(s.rangeCm, 0.f);
	ASSERT_TRUE(book.Fill(5, 1003, s));		// 질주 - 이동
	EXPECT_TRUE(s.movement);
	EXPECT_FALSE(s.damage);
	ASSERT_TRUE(book.Fill(3, 1088, s));		// 피뢰 - 패시브
	EXPECT_FALSE(s.damage);
	EXPECT_FALSE(book.Fill(1, 999999, s));
}
