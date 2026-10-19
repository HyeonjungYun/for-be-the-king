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

// ─── 2026-10-20 봇 고도화 (design/quick-specs/bot-loadouts-2026-10-20.md § 8 AC-BL-06~14) ───

namespace
{
	const char* PERSONA_HEADER = "role,flee_hp_scale,engage_hp_scale,avoid_power_scale,engage_radius_scale,loot_radius_scale,loot_goal_bonus,escape_early_scale,"
		"reaction_ms_min,reaction_ms_max,strafe,explore_speed,prefer_players,prefer_monsters,pause_chance,pause_ms_min,pause_ms_max,poi_bias,wounded_escape_hp,outnumbered_at,chase_memory_s\n";

	SkillSlot Slot(uint32 slot, uint32 skillId, SkillRole role, float reach, uint32 cooldownMs = 8000)
	{
		SkillSlot s;
		s.slot = slot;
		s.skillId = skillId;
		s.role = role;
		s.reachCm = reach;
		s.rangeCm = reach;
		s.cooldownMs = cooldownMs;
		s.damage = (role == SkillRole::Damage);
		s.movement = (role == SkillRole::Engage || role == SkillRole::Dash);
		s.selfBuff = (role == SkillRole::Buff);
		return s;
	}
}

TEST(BotPersona, 표는_성격마다_한_줄이고_범위_밖과_중복은_거부한다_AC_BL_06)
{
	PersonaBook book;
	std::string error;
	const std::string duel = "duel,0.8,0.75,1.5,1.3,0.8,0,0.8,180,320,0.8,0.9,1.0,-0.3,0.25,600,1500,0.5,0,4,6\n";
	ASSERT_TRUE(book.Parse(std::string(PERSONA_HEADER) + duel, error)) << error;
	ASSERT_EQ(book.byRole.size(), 1u);
	EXPECT_FLOAT_EQ(book.byRole.at("duel").base.avoidPowerScale, 1.5f);
	EXPECT_EQ(book.byRole.at("duel").base.outnumberedAt, 4);
	EXPECT_EQ(book.byRole.at("duel").base.chaseMemoryUs, 6ull * SEC);

	EXPECT_FALSE(book.Parse(std::string(PERSONA_HEADER) + duel + duel, error));		// 중복
	EXPECT_NE(error.find("duplicate"), std::string::npos) << error;
	EXPECT_FALSE(book.Parse(std::string(PERSONA_HEADER) + "duel,0.8,0.75,1.5,1.3,0.8,0,0.8,180,320,1.1,0.9,1,0,0.2,600,1500,0.5,0,4,6\n", error));	// strafe 1.1
	EXPECT_NE(error.find("strafe"), std::string::npos) << error;
	EXPECT_FALSE(book.Parse(std::string(PERSONA_HEADER) + "duel,0.8,0.75,1.5,1.3,0.8,0,0.8,400,300,0.5,0.9,1,0,0.2,600,1500,0.5,0,4,6\n", error));	// min > max
	EXPECT_FALSE(book.Parse("role,flee_hp_scale\nduel,1\n", error));		// 열이 모자란다

	// 같은 칠판이면 같은 성격 (결정적), 반응 지연은 범위 안, 모르는 role 은 balanced → 없으면 기본
	ASSERT_TRUE(book.Parse(std::string(PERSONA_HEADER) + duel, error)) << error;		// 위의 거부가 표를 비웠다
	Blackboard a, b;
	a.seed = b.seed = 77;
	const Persona pa = book.Make("duel", a);
	const Persona pb = book.Make("duel", b);
	EXPECT_EQ(pa.reactionUs, pb.reactionUs);
	EXPECT_FLOAT_EQ(pa.strafe, pb.strafe);
	EXPECT_GE(pa.reactionUs, 180'000u);
	EXPECT_LE(pa.reactionUs, 320'000u);
	EXPECT_EQ(book.Make("unknown", a).name, "default");

	// 실제 표
	string text;
	ASSERT_TRUE(Csv::ReadFile(ItemTable::DefaultDataDir() + "\\bot_personas.csv", OUT text));
	ASSERT_TRUE(book.Parse(text, error)) << error;
	for (const char* role : { "duel", "pack", "runner", "balanced" })
		EXPECT_EQ(book.byRole.count(role), 1u) << role;
}

TEST(BotBrain, 원거리_무기는_사거리를_쓰고_가까우면_물러선다_AC_BL_07)
{
	Brain brain;
	World w = Base();
	w.self.attackRangeCm = 900.f;		// 활
	w.self.ranged = true;
	w.creatures.push_back(Enemy(2, 700.f, 0.f));
	Blackboard bb;
	Action a = brain.Decide(w, bb);
	EXPECT_EQ(a.type, ActionType::Attack) << "900 × 0.9 = 810 안";

	w.creatures[0].x = 811.f;
	EXPECT_EQ(brain.Decide(w, bb).type, ActionType::MoveTo);		// 경계 - 810 밖은 다가간다

	// 평타를 기다리는 동안 적이 사거리 × 0.45 (364) 보다 가까우면 물러선다
	w.creatures[0].x = 300.f;
	w.self.attackReadyAtUs = w.nowUs + SEC;
	a = brain.Decide(w, bb);
	EXPECT_EQ(a.type, ActionType::MoveTo);
	EXPECT_STREQ(a.reason, "kiting");
	EXPECT_LT(a.x, 0.f) << "away from the enemy at +x";
	// 평타가 준비됐으면 물러서지 않고 쏜다
	w.self.attackReadyAtUs = 0;
	EXPECT_EQ(brain.Decide(w, bb).type, ActionType::Attack);
}

TEST(BotBrain, 근접은_평타_사이_상대_둘레를_돈다_AC_BL_08)
{
	Brain brain;
	World w = Base();
	w.self.attackRangeCm = 175.f;		// 검 → 157.5
	w.creatures.push_back(Enemy(2, 150.f, 0.f));
	w.self.attackReadyAtUs = w.nowUs + SEC;
	Blackboard bb;
	bb.persona.strafe = 0.8f;
	const Action a = brain.Decide(w, bb);
	EXPECT_EQ(a.type, ActionType::MoveTo);
	EXPECT_STREQ(a.reason, "strafing between swings");
	EXPECT_LE(Dist(150.f, 0.f, a.x, a.y), 175.f) << "stays inside the weapon range";
	EXPECT_GT(std::fabs(a.y), 50.f) << "sideways, not straight in";

	// 옆걸음 성격이 없으면 예전처럼 선 채로 평타를 기다린다
	Blackboard plain;
	EXPECT_EQ(brain.Decide(w, plain).type, ActionType::Attack);
	// 벽이면 옆걸음을 포기하고 평타
	w.walkable = [](float, float y) { return std::fabs(y) < 10.f; };
	Blackboard walled;
	walled.persona.strafe = 0.8f;
	EXPECT_EQ(brain.Decide(w, walled).type, ActionType::Attack);
}

TEST(BotBrain, 버프_약화_피해_돌진_순서로_스킬을_고른다_AC_BL_09)
{
	Brain brain;
	World w = Base();
	w.self.attackRangeCm = 175.f;
	w.creatures.push_back(Enemy(2, 450.f, 0.f));
	w.skills.push_back(Slot(3, 1084, SkillRole::Buff, 0.f, 15000));
	w.skills.push_back(Slot(4, 1017, SkillRole::Debuff, 500.f));
	w.skills.push_back(Slot(1, 1009, SkillRole::Damage, 800.f));
	w.skills.push_back(Slot(5, 1164, SkillRole::Engage, 600.f));
	Blackboard bb;
	Action a = brain.Decide(w, bb);
	EXPECT_EQ(a.slot, 3u) << "buff first (450 <= 157 + 400)";
	EXPECT_STREQ(a.reason, "buff before the clash");
	w.skills[0].readyAtUs = w.nowUs + SEC;
	a = brain.Decide(w, bb);
	EXPECT_EQ(a.slot, 4u) << "then the debuff opener";
	EXPECT_EQ(a.targetId, 2u);
	w.skills[1].readyAtUs = w.nowUs + SEC;
	EXPECT_EQ(brain.Decide(w, bb).slot, 1u) << "then damage";
	w.skills[2].readyAtUs = w.nowUs + SEC;
	a = brain.Decide(w, bb);
	EXPECT_EQ(a.slot, 5u) << "then a gap closer (450 > 157 + 50)";
	EXPECT_STREQ(a.reason, "gap closer");
	// 붙어 있으면 돌진하지 않는다
	w.creatures[0].x = 150.f;
	EXPECT_EQ(brain.Decide(w, bb).type, ActionType::Attack);
	// 버프는 멀리 있는 적에게 미리 쓰지 않는다 (2000 은 engage 1500 밖이라 600 으로)
	w.skills[0].readyAtUs = 0;
	w.creatures[0].x = 600.f;
	EXPECT_NE(brain.Decide(w, bb).slot, 3u);
	// 대상에게 붙는 돌진은 도망에 쓰지 않는다 (적 쪽으로 날아간다)
	World f = Base();
	f.self.hp = 100;
	f.creatures.push_back(Enemy(2, 500.f, 0.f));
	f.skills.push_back(Slot(5, 1164, SkillRole::Engage, 600.f));
	Blackboard bf;
	EXPECT_EQ(brain.Decide(f, bf).type, ActionType::MoveTo);
}

TEST(BotBrain, 막_보인_적에게는_반응_지연_뒤에_덤비고_때린_적은_바로_받아친다_AC_BL_10)
{
	Brain brain;
	World w = Base();
	BotAI::Creature e = Enemy(2, 600.f, 0.f);
	e.firstSeenUs = w.nowUs - 100'000;		// 0.1 s 전에 보였다
	w.creatures.push_back(e);
	Blackboard bb;
	bb.persona.reactionUs = 300'000;
	EXPECT_STRNE(brain.Decide(w, bb).branch, "fight");
	w.nowUs += 250'000;		// 0.35 s
	EXPECT_STREQ(brain.Decide(w, bb).branch, "fight");

	Blackboard bb2;
	bb2.persona.reactionUs = 300'000;
	World h = Base();
	BotAI::Creature hitter = Enemy(2, 600.f, 0.f);
	hitter.firstSeenUs = h.nowUs - 50'000;
	hitter.lastHitMeUs = h.nowUs - 10'000;
	h.creatures.push_back(hitter);
	EXPECT_STREQ(brain.Decide(h, bb2).branch, "fight");
}

TEST(BotBrain, 적_플레이어가_몰려_있으면_붙지_않고_피한다_AC_BL_11)
{
	Brain brain;
	World w = Base();
	w.self.hp = 400;		// 80 % - 머릿수를 센다 (< 90 %)
	for (uint64 i = 0; i < 3; i++)
		w.creatures.push_back(Enemy(2 + i, 500.f, -200.f + static_cast<float>(i) * 200.f));
	Blackboard bb;
	bb.persona.outnumberedAt = 3;
	const Action a = brain.Decide(w, bb);
	EXPECT_STREQ(a.branch, "avoid");
	EXPECT_STREQ(a.reason, "outnumbered");
	EXPECT_LT(a.x, 0.f);
	// 둘이면 싸운다 · 성격이 머릿수를 안 세면 싸운다 · 피가 꽉 차면 싸운다
	w.creatures.pop_back();
	Blackboard bb2;
	bb2.persona.outnumberedAt = 3;
	EXPECT_STREQ(brain.Decide(w, bb2).branch, "fight");
	w.creatures.push_back(Enemy(9, 500.f, 300.f));
	Blackboard plain;
	EXPECT_STREQ(brain.Decide(w, plain).branch, "fight");
	w.self.hp = 500;
	Blackboard full;
	full.persona.outnumberedAt = 3;
	EXPECT_STREQ(brain.Decide(w, full).branch, "fight");
}

TEST(BotBrain, 시야에서_사라진_대상은_마지막_자리까지_쫓는다_AC_BL_12)
{
	Brain brain;
	World w = Base();
	w.creatures.push_back(Enemy(2, 1000.f, 300.f));
	Blackboard bb;
	bb.persona.chaseMemoryUs = 4 * SEC;
	EXPECT_STREQ(brain.Decide(w, bb).branch, "fight");
	w.creatures.clear();		// 안개 속으로
	w.nowUs += SEC;
	Action a = brain.Decide(w, bb);
	EXPECT_STREQ(a.branch, "chase");
	EXPECT_FLOAT_EQ(a.x, 1000.f);
	EXPECT_FLOAT_EQ(a.y, 300.f);
	// 기억이 지나면 잊는다
	w.nowUs += 4 * SEC;
	EXPECT_STREQ(brain.Decide(w, bb).branch, "explore");
	// 도착했는데 없으면 잊는다
	Blackboard bb2;
	bb2.persona.chaseMemoryUs = 4 * SEC;
	bb2.lastSeenUs = w.nowUs;
	bb2.lastSeenId = 2;
	bb2.lastSeenX = 50.f;
	bb2.lastSeenY = 0.f;
	EXPECT_STREQ(brain.Decide(w, bb2).branch, "explore");
	EXPECT_EQ(bb2.lastSeenUs, 0u);
}

TEST(BotBrain, 성격이_있으면_둘러볼_곳을_고르고_닿으면_멈춰_둘러본다_AC_BL_13)
{
	Brain brain;
	World w = Base();
	w.pois.push_back({ 5000.f, 5000.f, 0 });
	Blackboard bb;
	bb.persona.poiBias = 1.f;
	bb.persona.exploreSpeed = 0.7f;
	bb.persona.pauseChance = 1.f;
	bb.persona.pauseMinUs = SEC;
	bb.persona.pauseMaxUs = 2 * SEC;
	Action a = brain.Decide(w, bb);
	EXPECT_STREQ(a.branch, "explore");
	EXPECT_LT(Dist(5000.f, 5000.f, a.x, a.y), 360.f) << "near the point of interest";
	EXPECT_FLOAT_EQ(a.speedScale, 0.7f);
	// 닿으면 멈춰 둘러본다 (Idle) - 그 사이엔 계속 Idle, 끝나면 다시 걷는다
	w.self.x = a.x;
	w.self.y = a.y;
	a = brain.Decide(w, bb);
	EXPECT_EQ(a.type, ActionType::Idle);
	EXPECT_STREQ(a.reason, "looking around");
	EXPECT_EQ(brain.Decide(w, bb).type, ActionType::Idle);
	w.nowUs += 3 * SEC;
	EXPECT_EQ(brain.Decide(w, bb).type, ActionType::MoveTo);
}

TEST(BotBrain, 다치고_들고_있는_게_있으면_일찍_탈출한다_AC_BL_14)
{
	Brain brain;
	World w = Base();
	w.self.hp = 200;		// 40 %
	w.self.carriedCount = 1;
	Blackboard bb;
	bb.persona.woundedEscapeHp = 0.45f;
	Action a = brain.Decide(w, bb);
	EXPECT_STREQ(a.branch, "escape");
	EXPECT_STREQ(a.reason, "wounded, cashing out");
	w.self.carriedCount = 0;		// 빈손이면 탈출할 이유가 없다
	EXPECT_STRNE(brain.Decide(w, bb).branch, "escape");
}

TEST(BotSkillBook, 스킬_칸의_쓰임을_가른다_AC_BL_15)
{
	SkillBook book;
	std::string error;
	string skills, effects;
	ASSERT_TRUE(Csv::ReadFile(ItemTable::DefaultDataDir() + "\\skills.csv", OUT skills));
	ASSERT_TRUE(Csv::ReadFile(ItemTable::DefaultDataDir() + "\\skill_effects.csv", OUT effects));
	ASSERT_TRUE(book.Parse(skills, effects, error)) << error;

	SkillSlot s;
	ASSERT_TRUE(book.Fill(1, 1008, s));		// 일도양단 - 대상에게 돌진 + 피해
	EXPECT_EQ(s.role, SkillRole::Engage);
	ASSERT_TRUE(book.Fill(5, 1164, s));		// 달려들기 - 대상에게 돌진
	EXPECT_EQ(s.role, SkillRole::Engage);
	ASSERT_TRUE(book.Fill(5, 1003, s));		// 질주
	EXPECT_EQ(s.role, SkillRole::Dash);
	ASSERT_TRUE(book.Fill(3, 1084, s));		// 내가 검이 된다 - 자기 강화
	EXPECT_EQ(s.role, SkillRole::Buff);
	ASSERT_TRUE(book.Fill(2, 1017, s));		// 점지 - 표식 (피해 없음)
	EXPECT_EQ(s.role, SkillRole::Debuff);
	ASSERT_TRUE(book.Fill(1, 1014, s));		// 원혼들의 몸부림 - 내 둘레 r250
	EXPECT_EQ(s.role, SkillRole::Damage);
	EXPECT_FLOAT_EQ(s.reachCm, 250.f);
	ASSERT_TRUE(book.Fill(4, 1118, s));		// 찌르고 베기 - 재입력
	EXPECT_TRUE(s.recast);
	ASSERT_TRUE(book.Fill(4, 1122, s));		// 지각변동 - 벽 세우기 (두뇌가 안 쓴다)
	EXPECT_EQ(s.role, SkillRole::None);
	ASSERT_TRUE(book.Fill(2, 1024, s));		// 연뢰 - 패시브
	EXPECT_EQ(s.role, SkillRole::None);
}

TEST(BotSkillBook, 이동_스킬의_몸과_후딜_버프를_읽는다_AC_BL_16)
{
	SkillBook book;
	std::string error;
	string skills, effects;
	ASSERT_TRUE(Csv::ReadFile(ItemTable::DefaultDataDir() + "\\skills.csv", OUT skills));
	ASSERT_TRUE(Csv::ReadFile(ItemTable::DefaultDataDir() + "\\skill_effects.csv", OUT effects));
	ASSERT_TRUE(book.Parse(skills, effects, error)) << error;

	SkillSlot s;
	ASSERT_TRUE(book.Fill(5, 1003, s));		// 질주 400 @ 1200 - 손발이 조준 쪽으로 난다
	EXPECT_EQ(s.dashMode, DashMode::Aim);
	EXPECT_FLOAT_EQ(s.dashDistCm, 400.f);
	EXPECT_FLOAT_EQ(s.dashSpeedCms, 1200.f);
	ASSERT_TRUE(book.Fill(5, 1165, s));		// 추격 - 대상 쪽
	EXPECT_EQ(s.dashMode, DashMode::ToTarget);
	ASSERT_TRUE(book.Fill(2, 1075, s));		// 블링크 - 서버가 옮긴다 (몸을 날리지 않는다)
	EXPECT_EQ(s.dashMode, DashMode::ServerMoves);
	ASSERT_TRUE(book.Fill(2, 1053, s));		// 일보후퇴 - 물러서며 쏘는 피해 스킬 (붙기 아님)
	EXPECT_EQ(s.role, SkillRole::Damage);
	EXPECT_EQ(s.dashMode, DashMode::Back);		// 서버는 back 을 클라가 옮기게 둔다 (속도 창만) - 손발이 뒤로 난다
	ASSERT_TRUE(book.Fill(4, 1133, s));		// 육량전 - 6 s 기본 공격만 · 사거리 −500
	EXPECT_EQ(s.lockOthersMs, 6000u);
	EXPECT_EQ(s.rangeBuffCm, -500);
	EXPECT_EQ(s.rangeBuffMs, 6000u);
	ASSERT_TRUE(book.Fill(4, 1131, s));		// 통아 +600
	EXPECT_EQ(s.rangeBuffCm, 600);
}

TEST(BotBrain, 도망칠_체력이면_손이_닿고_이기는_적만_마무리한다_AC_BL_17)
{
	Brain brain;
	World w = Base();
	w.self.hp = 125;		// 25 % < flee 30 %
	w.creatures.push_back(Enemy(2, 1200.f, 0.f, 50));		// 10 % - 마무리 대상이지만 멀다 (위협 반경 800 밖)
	Blackboard bb;
	EXPECT_STRNE(brain.Decide(w, bb).branch, "fight") << "too far to finish while this hurt";
	// 붙어 있고 내가 이기고 있으면 마무리한다 - 그런데 위협 반경 안이면 flee 가 먼저다 (피 25 %)
	w.creatures[0].x = 140.f;
	Blackboard bb2;
	EXPECT_STREQ(brain.Decide(w, bb2).branch, "flee");
	// 다친 몸으로는 안개 속까지 쫓지 않는다
	Blackboard bb3;
	bb3.persona.chaseMemoryUs = 4 * SEC;
	bb3.lastSeenUs = w.nowUs;
	bb3.lastSeenId = 9;
	bb3.lastSeenX = 2000.f;
	World e = Base();
	e.self.hp = 200;		// 40 % < engage 45 %
	EXPECT_STRNE(brain.Decide(e, bb3).branch, "chase");
}
