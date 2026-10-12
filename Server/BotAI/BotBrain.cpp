#include "BotBrain.h"
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <sstream>
#include <cstdlib>

namespace BotAI
{
	namespace
	{
		float Dist(float ax, float ay, float bx, float by)
		{
			const float dx = ax - bx;
			const float dy = ay - by;
			return std::sqrt(dx * dx + dy * dy);
		}

		// 한 줄 CSV (따옴표 없음)
		void SplitLines(const std::string& text, std::vector<std::string>& lines)
		{
			std::stringstream ss(text);
			std::string line;
			while (std::getline(ss, line))
			{
				if (line.empty() == false && line.back() == '\r')
					line.pop_back();
				if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF)
					line = line.substr(3);		// BOM
				if (line.empty() == false)
					lines.push_back(line);
			}
		}

		void SplitRow(const std::string& line, std::vector<std::string>& cells)
		{
			cells.clear();
			std::string cur;
			for (char ch : line)
			{
				if (ch == ',') { cells.push_back(cur); cur.clear(); }
				else cur.push_back(ch);
			}
			cells.push_back(cur);
		}

		int32 ColumnOf(const std::vector<std::string>& header, const char* name)
		{
			for (size_t i = 0; i < header.size(); i++)
				if (header[i] == name)
					return static_cast<int32>(i);
			return -1;
		}

		bool ParseNumber(const std::string& s, float& out)
		{
			if (s.empty())
				return false;
			char* end = nullptr;
			out = std::strtof(s.c_str(), &end);
			return end != nullptr && *end == '\0';
		}

		bool Hostile(const World& w, const Creature& c)
		{
			return c.ally == false && c.id != w.self.id && c.hp > 0;
		}

		// 가장 가까운 "적" 쪽으로 돌릴 수 있게 - 위협에서 멀어지는 방향으로 distance 만큼. 벽이면 8 방향으로 돌려 본다
		bool AwayFrom(const World& w, float fromX, float fromY, float distance, float& outX, float& outY)
		{
			float dx = w.self.x - fromX;
			float dy = w.self.y - fromY;
			const float len = std::sqrt(dx * dx + dy * dy);
			if (len < 1.f) { dx = 1.f; dy = 0.f; }
			else { dx /= len; dy /= len; }
			const float base = std::atan2(dy, dx);
			const float turns[8] = { 0.f, 0.6f, -0.6f, 1.2f, -1.2f, 1.9f, -1.9f, 3.14159f };
			for (float t : turns)
			{
				const float a = base + t;
				const float x = w.self.x + std::cos(a) * distance;
				const float y = w.self.y + std::sin(a) * distance;
				if (w.walkable == nullptr || w.walkable(x, y))
				{
					outX = x;
					outY = y;
					return true;
				}
			}
			return false;
		}
	}

	/*-------- BrainConfig --------*/

	bool BrainConfig::Parse(const std::string& csv, std::string& error)
	{
		std::vector<std::string> lines, cells;
		SplitLines(csv, lines);
		if (lines.empty())
		{
			error = "bot_brain.csv is empty";
			return false;
		}
		SplitRow(lines[0], cells);
		const int32 keyCol = ColumnOf(cells, "key");
		const int32 valCol = ColumnOf(cells, "value");
		if (keyCol < 0 || valCol < 0)
		{
			error = "bot_brain.csv needs key,value";
			return false;
		}
		for (size_t i = 1; i < lines.size(); i++)
		{
			SplitRow(lines[i], cells);
			if (static_cast<int32>(cells.size()) <= std::max(keyCol, valCol))
				continue;
			const std::string& key = cells[keyCol];
			float v = 0.f;
			if (ParseNumber(cells[valCol], v) == false)
			{
				error = "bot_brain.csv " + key + " must be a number";
				return false;
			}
			// 거부 범위 = 스펙 § 7 의 "범위" 열 그대로 (검증자 수정 3). 범위 밖은 조용히 깎지 않고 거부한다
			auto within = [&](float lo, float hi) { if (v < lo || v > hi) { error = "bot_brain.csv " + key + " must be " + std::to_string(lo) + ".." + std::to_string(hi); return false; } return true; };
			auto num = [&](float& field, float lo, float hi) { if (within(lo, hi) == false) return false; field = v; return true; };
			auto secs = [&](uint64& field, float lo, float hi) { if (within(lo, hi) == false) return false; field = static_cast<uint64>(v * 1'000'000.f); return true; };
			bool ok = true;
			if (key == "flee_hp_ratio") ok = num(fleeHpRatio, 0.f, 1.f);
			else if (key == "engage_hp_ratio") ok = num(engageHpRatio, 0.f, 1.f);
			else if (key == "threat_radius_cm") ok = num(threatRadiusCm, 100.f, 5000.f);
			else if (key == "engage_radius_cm") ok = num(engageRadiusCm, 100.f, 5000.f);
			else if (key == "avoid_power_ratio") ok = num(avoidPowerRatio, 0.5f, 10.f);
			else if (key == "attack_range_cm") ok = num(attackRangeCm, 50.f, 1000.f);
			else if (key == "flee_distance_cm") ok = num(fleeDistanceCm, 100.f, 5000.f);
			else if (key == "flee_hold_s") ok = secs(fleeHoldUs, 0.f, 60.f);
			else if (key == "loot_radius_cm") ok = num(lootRadiusCm, 100.f, 10000.f);
			else if (key == "loot_reach_cm") ok = num(lootReachCm, 50.f, 300.f);
			else if (key == "loot_goal_value") { if (within(0.f, 100.f) == false) return false; lootGoalValue = static_cast<int32>(v); }
			else if (key == "escape_when_left_s") ok = secs(escapeWhenLeftUs, 0.f, 3600.f);
			else if (key == "tower_reach_cm") ok = num(towerReachCm, 100.f, 2000.f);
			else if (key == "wander_radius_cm") ok = num(wanderRadiusCm, 100.f, 10000.f);
			else if (key == "wander_hold_s") ok = secs(wanderHoldUs, 1.f, 120.f);
			else if (key == "monster_pack_avoid") ok = num(monsterPackAvoid, 1.f, 10.f);
			else if (key == "monster_power_each") ok = num(monsterPowerEach, 0.05f, 5.f);
			else if (key == "monster_pack_radius_cm") ok = num(monsterPackRadiusCm, 100.f, 5000.f);
			else if (key == "recent_hit_s") ok = secs(recentHitUs, 0.f, 60.f);
			else if (key == "finish_hp_ratio") ok = num(finishHpRatio, 0.f, 1.f);
			else if (key == "arrive_cm") ok = num(arriveCm, 10.f, 1000.f);
			else
			{
				error = "bot_brain.csv unknown key " + key;
				return false;
			}
			if (ok == false)
				return false;
		}
		return true;
	}

	/*-------- 판단 조각 --------*/

	namespace
	{
		// 싸움 대상 점수 가중치 (BestTarget) - 표가 아니라 코드 상수: 바꾸면 bot-ai 스펙 § 7 과 테스트 AC-AI-04 를 같이 본다
		constexpr float SCORE_LOW_HP = 2.f;			// 적의 빠진 피 비율 ×
		constexpr float SCORE_NEAR = 1.5f;			// 가까움 ×
		constexpr float SCORE_POWER = 0.5f;			// 적의 힘 − ×
		constexpr float SCORE_RECENT_ATTACKER = 1.f;	// recent_hit_s 안에 나를 때렸다 +
		constexpr float SCORE_PLAYER = 0.5f;		// 플레이어 (가방) +
	}

	const Creature* Brain::NearestThreat(const World& w, float radiusCm)
	{
		const Creature* best = nullptr;
		float bestD = radiusCm;
		for (const Creature& c : w.creatures)
		{
			if (Hostile(w, c) == false)
				continue;
			const float d = Dist(w.self.x, w.self.y, c.x, c.y);
			if (d <= bestD)
			{
				bestD = d;
				best = &c;
			}
		}
		return best;
	}

	float Brain::PowerOf(const World& w, const Creature& c, const BrainConfig& cfg)
	{
		if (c.kind == CreatureKind::Player)
			return c.power * (0.5f + c.HpRatio());		// 피가 적은 적은 약하다
		// 몬스터 - 12 m 안의 다른 몬스터를 무리로 센다
		float n = 0.f;
		for (const Creature& o : w.creatures)
		{
			if (o.kind == CreatureKind::Monster && o.hp > 0 && Dist(o.x, o.y, c.x, c.y) <= cfg.monsterPackRadiusCm)
				n += 1.f;
		}
		return c.power * cfg.monsterPowerEach * std::max(1.f, n) * (0.5f + c.HpRatio());
	}

	const Creature* Brain::BestTarget(const World& w, const BrainConfig& cfg)
	{
		const float myPower = 0.5f + w.self.HpRatio();
		const Creature* best = nullptr;
		float bestScore = -FLT_MAX;
		for (const Creature& c : w.creatures)
		{
			if (Hostile(w, c) == false)
				continue;
			const float d = Dist(w.self.x, w.self.y, c.x, c.y);
			if (d > cfg.engageRadiusCm)
				continue;
			const float power = PowerOf(w, c, cfg);
			if (power >= myPower * cfg.avoidPowerRatio)
				continue;		// 센 적은 싸움 대상이 아니다 (Avoid 가 다룬다)
			// 가까울수록 · 피가 적을수록 · 나를 때린 적일수록 (가중치는 코드 상수 - 스펙 § 7)
			float score = (1.f - c.HpRatio()) * SCORE_LOW_HP + (1.f - std::min(d, cfg.engageRadiusCm) / cfg.engageRadiusCm) * SCORE_NEAR - power * SCORE_POWER;
			if (c.lastHitMeUs != 0 && w.nowUs - c.lastHitMeUs < cfg.recentHitUs)
				score += SCORE_RECENT_ATTACKER;
			if (c.kind == CreatureKind::Player)
				score += SCORE_PLAYER;		// 플레이어가 더 가치 있다 (가방)
			if (score > bestScore)
			{
				bestScore = score;
				best = &c;
			}
		}
		return best;
	}

	const Loot* Brain::NearestLoot(const World& w, float radiusCm)
	{
		const Loot* best = nullptr;
		float bestD = radiusCm;
		for (const Loot& l : w.loot)
		{
			if (l.triedUs)
				continue;
			const float d = Dist(w.self.x, w.self.y, l.x, l.y);
			if (d <= bestD)
			{
				bestD = d;
				best = &l;
			}
		}
		return best;
	}

	const Tower* Brain::NearestTower(const World& w)
	{
		const Tower* best = nullptr;
		float bestD = FLT_MAX;
		for (const Tower& t : w.towers)
		{
			const float d = Dist(w.self.x, w.self.y, t.x, t.y);
			if (d < bestD)
			{
				bestD = d;
				best = &t;
			}
		}
		return best;
	}

	const SkillSlot* Brain::ReadySkill(const World& w, bool wantDamage, float distCm)
	{
		const SkillSlot* best = nullptr;
		for (const SkillSlot& s : w.skills)
		{
			if (s.skillId == 0 || w.nowUs < s.readyAtUs)
				continue;
			if (wantDamage)
			{
				if (s.damage == false || (s.rangeCm > 0.f && distCm > s.rangeCm))
					continue;
			}
			else if (s.movement == false)
				continue;
			if (best == nullptr || s.rangeCm > best->rangeCm)
				best = &s;
		}
		return best;
	}

	bool Brain::FleePoint(const World& w, const Creature& threat, float distanceCm, float& outX, float& outY, Blackboard& bb)
	{
		if (bb.fleeUntilUs > w.nowUs && Dist(w.self.x, w.self.y, bb.fleeX, bb.fleeY) > 100.f)
		{
			outX = bb.fleeX;
			outY = bb.fleeY;
			return true;
		}
		if (AwayFrom(w, threat.x, threat.y, distanceCm, outX, outY) == false)
			return false;
		bb.fleeX = outX;
		bb.fleeY = outY;
		return true;
	}

	/*-------- 트리 --------*/

	Brain::Brain(const BrainConfig& config) : _config(config)
	{
		_root = Build();
	}

	NodePtr Brain::Build() const
	{
		const BrainConfig& cfg = _config;
		NodePtr root = MakeSelector("root");

		// 1. 묶였다 - 아무것도 못 한다
		AsComposite(root).Add(MakeAction("stunned", [](Context& c)
			{
				if (c.world.self.hardCc == false || c.world.self.hp <= 0)
					return false;
				c.out.type = ActionType::Idle;
				c.out.reason = "hard cc";
				return true;
			}));

		// 2. 살아남기 - 피가 낮고 위협이 가까우면 도망 (이동 스킬이 있으면 그걸로).
		// 도망 중(flee · avoid 가 정한 지점, flee_hold 동안)이면 위협이 잠깐 반경 밖으로 나가도 그 지점까지 간다 - 가지가 틱마다 바뀌는 떨림을 막는다
		AsComposite(root).Add(MakeAction("flee", [cfg](Context& c)
			{
				const World& w = c.world;
				if (c.bb.fleeUntilUs > w.nowUs && Dist(w.self.x, w.self.y, c.bb.fleeX, c.bb.fleeY) > 100.f && w.self.casting == false && w.self.dashing == false)
				{
					c.out.type = ActionType::MoveTo;
					c.out.x = c.bb.fleeX;
					c.out.y = c.bb.fleeY;
					c.out.reason = "keep fleeing";
					return true;
				}
				if (w.self.HpRatio() >= cfg.fleeHpRatio)
					return false;
				const Creature* threat = NearestThreat(w, cfg.threatRadiusCm);
				if (threat == nullptr)
					return false;
				float x = 0.f, y = 0.f;
				if (FleePoint(w, *threat, cfg.fleeDistanceCm, x, y, c.bb) == false)
					return false;
				if (c.bb.fleeUntilUs <= w.nowUs)
					c.bb.fleeUntilUs = w.nowUs + cfg.fleeHoldUs;
				if (const SkillSlot* dash = ReadySkill(w, false, 0.f))
				{
					c.out.type = ActionType::UseSkill;
					c.out.slot = dash->slot;
					c.out.x = x;
					c.out.y = y;
					c.out.reason = "flee with a movement skill";
					return true;
				}
				c.out.type = ActionType::MoveTo;
				c.out.x = x;
				c.out.y = y;
				c.out.reason = "hp low, threat near";
				return true;
			}));

		// 3. 탈출 - 시간이 얼마 안 남았거나 가진 게 충분하면 타워로. 구역 안이면 서서 기다린다
		AsComposite(root).Add(MakeAction("escape", [cfg](Context& c)
			{
				const World& w = c.world;
				const bool timeUp = (w.self.runEndsAtUs != 0 && w.self.runEndsAtUs > w.nowUs && w.self.runEndsAtUs - w.nowUs <= cfg.escapeWhenLeftUs);
				const bool rich = (w.self.carriedValue >= cfg.lootGoalValue);
				if (timeUp == false && rich == false)
					return false;
				const Tower* tower = NearestTower(w);
				if (tower == nullptr)
					return false;
				// 가는 길에 약한 적이 바로 옆이면 싸움 가지가 먼저 처리하지 않는다 - 탈출이 우선 (가진 것을 지킨다)
				const float d = Dist(w.self.x, w.self.y, tower->x, tower->y);
				if (d <= cfg.towerReachCm || w.self.inEscapeZone)
				{
					c.out.type = ActionType::Escape;
					c.out.targetId = 0;
					c.out.x = tower->x;
					c.out.y = tower->y;
					c.out.reason = timeUp ? "in the zone, time short" : "in the zone, bag full";
					return true;
				}
				c.out.type = ActionType::MoveTo;
				c.out.x = tower->x;
				c.out.y = tower->y;
				c.out.reason = timeUp ? "time short, to the tower" : "bag full, to the tower";
				return true;
			}));

		// 4. 싸움 - 이길 만한 적이 있고 피가 충분하면 (또는 그 적의 피가 아주 적으면)
		AsComposite(root).Add(MakeAction("fight", [cfg](Context& c)
			{
				const World& w = c.world;
				if (w.self.casting || w.self.dashing)
				{
					c.out.type = ActionType::Idle;
					c.out.reason = "casting";
					return true;
				}
				const Creature* target = BestTarget(w, cfg);
				if (target == nullptr)
					return false;
				if (w.self.HpRatio() < cfg.engageHpRatio && target->HpRatio() > cfg.finishHpRatio)
					return false;
				c.bb.targetId = target->id;
				const float d = Dist(w.self.x, w.self.y, target->x, target->y);
				if (const SkillSlot* skill = ReadySkill(w, true, d))
				{
					c.out.type = ActionType::UseSkill;
					c.out.slot = skill->slot;
					c.out.targetId = target->id;
					c.out.x = target->x;
					c.out.y = target->y;
					c.out.reason = "damage skill ready";
					return true;
				}
				if (d <= cfg.attackRangeCm)
				{
					c.out.type = ActionType::Attack;
					c.out.targetId = target->id;
					c.out.reason = "in attack range";
					return true;
				}
				c.out.type = ActionType::MoveTo;
				c.out.targetId = target->id;
				c.out.x = target->x;
				c.out.y = target->y;
				c.out.reason = "closing in";
				return true;
			}));

		// 5. 피하기 - 센 적(무리 · 강한 플레이어)이 가까우면 멀어진다
		AsComposite(root).Add(MakeAction("avoid", [cfg](Context& c)
			{
				const World& w = c.world;
				const float myPower = 0.5f + w.self.HpRatio();
				const Creature* worst = nullptr;
				float worstD = cfg.threatRadiusCm;
				for (const Creature& cr : w.creatures)
				{
					if (Hostile(w, cr) == false)
						continue;
					const float d = Dist(w.self.x, w.self.y, cr.x, cr.y);
					if (d <= worstD && PowerOf(w, cr, cfg) >= myPower * cfg.avoidPowerRatio)
					{
						worstD = d;
						worst = &cr;
					}
				}
				if (worst == nullptr)
					return false;
				float x = 0.f, y = 0.f;
				if (FleePoint(w, *worst, cfg.fleeDistanceCm, x, y, c.bb) == false)
					return false;
				if (c.bb.fleeUntilUs <= w.nowUs)
					c.bb.fleeUntilUs = w.nowUs + cfg.fleeHoldUs;
				c.out.type = ActionType::MoveTo;
				c.out.x = x;
				c.out.y = y;
				c.out.reason = "stronger enemy near";
				return true;
			}));

		// 6. 줍기 - 가까운 가방 · 상자
		AsComposite(root).Add(MakeAction("loot", [cfg](Context& c)
			{
				const World& w = c.world;
				const Loot* loot = NearestLoot(w, cfg.lootRadiusCm);
				if (loot == nullptr)
					return false;
				c.bb.lootId = loot->id;
				const float d = Dist(w.self.x, w.self.y, loot->x, loot->y);
				if (d <= cfg.lootReachCm)
				{
					c.out.type = ActionType::OpenLoot;
					c.out.targetId = loot->id;
					c.out.x = loot->x;
					c.out.y = loot->y;
					c.out.reason = loot->chest ? "chest in reach" : "bag in reach";
					return true;
				}
				c.out.type = ActionType::MoveTo;
				c.out.targetId = loot->id;
				c.out.x = loot->x;
				c.out.y = loot->y;
				c.out.reason = "walking to loot";
				return true;
			}));

		// 7. 탐색 - 아무것도 없으면 걷는다 (결정적 난수, 벽은 피한다)
		AsComposite(root).Add(MakeAction("explore", [cfg](Context& c)
			{
				const World& w = c.world;
				Blackboard& bb = c.bb;
				const bool arrived = Dist(w.self.x, w.self.y, bb.wanderX, bb.wanderY) < cfg.arriveCm;
				if (bb.wanderUntilUs <= w.nowUs || arrived || (bb.wanderX == 0.f && bb.wanderY == 0.f))
				{
					for (int32 tries = 0; tries < 12; tries++)
					{
						const float a = bb.Random01() * 6.2831853f;
						const float r = cfg.wanderRadiusCm * (0.5f + 0.5f * bb.Random01());
						const float x = w.self.x + std::cos(a) * r;
						const float y = w.self.y + std::sin(a) * r;
						if (w.walkable == nullptr || w.walkable(x, y))
						{
							bb.wanderX = x;
							bb.wanderY = y;
							break;
						}
					}
					bb.wanderUntilUs = w.nowUs + cfg.wanderHoldUs;
				}
				c.out.type = ActionType::MoveTo;
				c.out.x = bb.wanderX;
				c.out.y = bb.wanderY;
				c.out.reason = "nothing around";
				return true;
			}));

		return root;
	}

	Action Brain::Decide(const World& world, Blackboard& bb) const
	{
		Action out;
		Context ctx{ world, bb, out };
		if (world.self.hp <= 0)
		{
			out.type = ActionType::Idle;
			out.branch = "dead";
			out.reason = "dead";
			return out;
		}
		_root->Tick(ctx);
		return out;
	}

	/*-------- SkillBook --------*/

	bool SkillBook::Parse(const std::string& skillsCsv, const std::string& effectsCsv, std::string& error)
	{
		entries.clear();
		std::vector<std::string> lines, cells;
		SplitLines(skillsCsv, lines);
		if (lines.empty())
		{
			error = "skills.csv is empty";
			return false;
		}
		SplitRow(lines[0], cells);
		const int32 idCol = ColumnOf(cells, "skill_id");
		const int32 kindCol = ColumnOf(cells, "kind");
		const int32 shapeCol = ColumnOf(cells, "shape");
		const int32 aimCol = ColumnOf(cells, "aim");
		const int32 rangeCol = ColumnOf(cells, "range_cm");
		const int32 castCol = ColumnOf(cells, "cast_ms");
		const int32 cdCol = ColumnOf(cells, "cooldown_ms");
		const int32 powerCol = ColumnOf(cells, "power");
		if (idCol < 0 || kindCol < 0 || shapeCol < 0 || aimCol < 0 || rangeCol < 0 || castCol < 0 || cdCol < 0 || powerCol < 0)
		{
			error = "skills.csv needs skill_id,kind,shape,aim,range_cm,cast_ms,cooldown_ms,power";
			return false;
		}
		for (size_t i = 1; i < lines.size(); i++)
		{
			SplitRow(lines[i], cells);
			if (static_cast<int32>(cells.size()) <= std::max({ idCol, kindCol, shapeCol, aimCol, rangeCol, castCol, cdCol, powerCol }))
				continue;
			Entry e;
			float f = 0.f;
			if (ParseNumber(cells[idCol], f) == false) continue;
			e.skillId = static_cast<uint32>(f);
			ParseNumber(cells[rangeCol], e.rangeCm);
			if (ParseNumber(cells[castCol], f)) e.castMs = static_cast<uint32>(f);
			if (ParseNumber(cells[cdCol], f)) e.cooldownMs = static_cast<uint32>(f);
			const std::string& shape = cells[shapeCol];
			const std::string& kind = cells[kindCol];
			e.damage = (cells[powerCol] != "none") && (shape != "none") && (kind != "passive");
			e.needsTarget = (cells[aimCol] == "target");
			e.selfBuff = (shape == "none" && cells[aimCol] == "none");
			entries[e.skillId] = e;
		}
		// skill_effects.csv - 내 몸을 옮기는 효과(effect=move, who=self: param dash · back · leap · blink · to_target ...)가 있는 스킬은 movement.
		// 두뇌는 그 칸을 도망 · 접근에만 쓴다 - 피해를 함께 주더라도 싸움 칸으로는 세지 않는다
		lines.clear();
		SplitLines(effectsCsv, lines);
		if (lines.empty() == false)
		{
			SplitRow(lines[0], cells);
			const int32 sid = ColumnOf(cells, "skill_id");
			int32 type = ColumnOf(cells, "effect");
			if (type < 0) type = ColumnOf(cells, "effect_type");
			const int32 who = ColumnOf(cells, "who");
			if (sid >= 0 && type >= 0)
			{
				for (size_t i = 1; i < lines.size(); i++)
				{
					SplitRow(lines[i], cells);
					if (static_cast<int32>(cells.size()) <= std::max({ sid, type, who }))
						continue;
					float f = 0.f;
					if (ParseNumber(cells[sid], f) == false)
						continue;
					auto it = entries.find(static_cast<uint32>(f));
					if (it == entries.end())
						continue;
					const std::string& t = cells[type];
					const bool self = (who < 0) || cells[who] == "self";
					if (self && (t == "move" || t == "dash" || t == "blink" || t == "teleport" || t == "leap" || t == "backstep"))
					{
						it->second.movement = true;
						it->second.damage = false;
					}
				}
			}
		}
		return true;
	}

	bool SkillBook::Fill(uint32 slot, uint32 skillId, SkillSlot& out) const
	{
		auto it = entries.find(skillId);
		if (it == entries.end())
			return false;
		const Entry& e = it->second;
		out.slot = slot;
		out.skillId = skillId;
		out.rangeCm = e.rangeCm;
		out.castMs = e.castMs;
		out.cooldownMs = e.cooldownMs;
		out.damage = e.damage;
		out.movement = e.movement;
		out.selfBuff = e.selfBuff;
		out.needsTarget = e.needsTarget;
		return true;
	}
}
