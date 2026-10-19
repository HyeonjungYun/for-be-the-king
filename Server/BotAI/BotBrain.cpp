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

		// 2026-10-20 - 싸움 손놀림 (bot-loadouts 스펙 § 4). 비율이라 표가 아니라 상수로 둔다
		constexpr float	ATTACK_RANGE_MARGIN = 0.9f;		// 무기 사거리 × - windup 끝 사거리 재검사에 걸리지 않게
		constexpr float	BUFF_REACH_EXTRA_CM = 400.f;	// 평타 사거리 + 이만큼 안에 들어오면 버프부터
		constexpr float	ENGAGE_MIN_GAP_CM = 50.f;		// 평타 사거리 + 이만큼 밖이어야 돌진한다
		constexpr float	KITE_MIN_RATIO = 0.45f;			// 원거리 - 적이 사거리 × 이보다 가까우면 물러선다
		constexpr float	KITE_TO_RATIO = 0.7f;			// 물러설 때 사거리 × 이 거리까지
		constexpr float	STRAFE_MIN_RATIO = 0.55f;		// 근접 옆걸음 - 사거리 × 이 반경 ~ ×0.8 에서 돈다
		constexpr float	STRAFE_MAX_RATIO = 0.8f;
		constexpr float	STRAFE_STEP_CM = 160.f;			// 옆걸음 한 번의 옆 거리 × strafe
		constexpr uint64	STRAFE_FLIP_MIN_US = 1'200'000;
		constexpr uint64	STRAFE_FLIP_MAX_US = 3'200'000;
		constexpr float	POI_JITTER_CM = 350.f;			// 둘러볼 곳 둘레 무작위
		constexpr float	OUTNUMBERED_HP = 0.9f;			// 이 아래 체력에서만 머릿수를 센다 (꽉 찬 피면 붙는다)
		constexpr float	FINISH_REACH_EXTRA_CM = 100.f;	// 도망칠 체력이면 평타 사거리 + 이만큼 안의 적만 마무리한다
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

	const Creature* Brain::BestTarget(const World& w, const BrainConfig& cfg, const Persona* persona)
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
			// 반응 지연 - 막 시야에 들어온 적은 아직 "못 봤다". 나를 때린 적은 바로 받아친다
			if (persona != nullptr && persona->reactionUs > 0 && c.firstSeenUs != 0 && c.lastHitMeUs == 0
				&& w.nowUs >= c.firstSeenUs && w.nowUs - c.firstSeenUs < persona->reactionUs)
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
			if (persona != nullptr)
				score += (c.kind == CreatureKind::Player) ? persona->preferPlayers : persona->preferMonsters;
			if (score > bestScore)
			{
				bestScore = score;
				best = &c;
			}
		}
		return best;
	}

	BrainConfig Brain::Tuned(const BrainConfig& cfg, const Persona& p)
	{
		BrainConfig t = cfg;
		t.fleeHpRatio = std::min(0.95f, cfg.fleeHpRatio * p.fleeHpScale);
		t.engageHpRatio = std::min(0.95f, cfg.engageHpRatio * p.engageHpScale);
		t.avoidPowerRatio = cfg.avoidPowerRatio * p.avoidPowerScale;
		t.engageRadiusCm = cfg.engageRadiusCm * p.engageRadiusScale;
		t.lootRadiusCm = cfg.lootRadiusCm * p.lootRadiusScale;
		t.lootGoalValue = std::max(1, cfg.lootGoalValue + p.lootGoalBonus);
		t.escapeWhenLeftUs = static_cast<uint64>(static_cast<double>(cfg.escapeWhenLeftUs) * p.escapeEarlyScale);
		return t;
	}

	float Brain::AttackRange(const World& w, const BrainConfig& cfg)
	{
		return (w.self.attackRangeCm > 0.f) ? w.self.attackRangeCm * ATTACK_RANGE_MARGIN : cfg.attackRangeCm;
	}

	int32 Brain::HostilePlayersNear(const World& w, float radiusCm, float& outX, float& outY)
	{
		int32 n = 0;
		float sx = 0.f, sy = 0.f;
		for (const Creature& c : w.creatures)
		{
			if (Hostile(w, c) == false || c.kind != CreatureKind::Player)
				continue;
			if (Dist(w.self.x, w.self.y, c.x, c.y) > radiusCm)
				continue;
			n++;
			sx += c.x;
			sy += c.y;
		}
		if (n > 0)
		{
			outX = sx / static_cast<float>(n);
			outY = sy / static_cast<float>(n);
		}
		return n;
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

	namespace
	{
		float ReachOf(const SkillSlot& s) { return (s.reachCm > 0.f) ? s.reachCm : s.rangeCm; }
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
				if (s.damage == false || (ReachOf(s) > 0.f && distCm > ReachOf(s)))
					continue;
			}
			else if (s.movement == false || s.role == SkillRole::Engage)
				continue;		// 대상에게 붙는 돌진은 도망에 쓰지 않는다 - 적 쪽으로 날아간다
			if (best == nullptr || ReachOf(s) > ReachOf(*best))
				best = &s;
		}
		return best;
	}

	const SkillSlot* Brain::ReadyRole(const World& w, SkillRole role, float distCm)
	{
		const SkillSlot* best = nullptr;
		for (const SkillSlot& s : w.skills)
		{
			if (s.skillId == 0 || s.role != role || w.nowUs < s.readyAtUs)
				continue;
			// 버프는 거리를 보지 않는다 (호출자가 본다). 나머지는 닿아야 한다
			if (role != SkillRole::Buff && ReachOf(s) > 0.f && distCm > ReachOf(s))
				continue;
			if (best == nullptr || s.cooldownMs > best->cooldownMs)
				best = &s;		// 쿨다운이 긴 것부터 - 짧은 것은 금방 돌아온다
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
		const BrainConfig& base = _config;
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
		AsComposite(root).Add(MakeAction("flee", [base](Context& c)
			{
				const World& w = c.world;
				const BrainConfig cfg = Tuned(base, c.bb.persona);
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

		// 3. 탈출 - 시간이 얼마 안 남았거나 가진 게 충분하면 (또는 다쳤는데 들고 있는 게 있으면) 타워로. 구역 안이면 서서 기다린다
		AsComposite(root).Add(MakeAction("escape", [base](Context& c)
			{
				const World& w = c.world;
				const Persona& p = c.bb.persona;
				const BrainConfig cfg = Tuned(base, p);
				const bool timeUp = (w.self.runEndsAtUs != 0 && w.self.runEndsAtUs > w.nowUs && w.self.runEndsAtUs - w.nowUs <= cfg.escapeWhenLeftUs);
				const bool rich = (w.self.carriedValue >= cfg.lootGoalValue);
				const bool wounded = (p.woundedEscapeHp > 0.f && w.self.HpRatio() < p.woundedEscapeHp && w.self.carriedCount > 0);
				if (timeUp == false && rich == false && wounded == false)
					return false;
				const Tower* tower = NearestTower(w);
				if (tower == nullptr)
					return false;
				// 가는 길에 약한 적이 바로 옆이면 싸움 가지가 먼저 처리하지 않는다 - 탈출이 우선 (가진 것을 지킨다)
				const float d = Dist(w.self.x, w.self.y, tower->x, tower->y);
				const char* why = timeUp ? "time short" : (rich ? "bag full" : "wounded, cashing out");
				if (d <= cfg.towerReachCm || w.self.inEscapeZone)
				{
					c.out.type = ActionType::Escape;
					c.out.targetId = 0;
					c.out.x = tower->x;
					c.out.y = tower->y;
					c.out.reason = timeUp ? "in the zone, time short" : (rich ? "in the zone, bag full" : "in the zone, wounded");
					return true;
				}
				c.out.type = ActionType::MoveTo;
				c.out.x = tower->x;
				c.out.y = tower->y;
				c.out.reason = why;
				return true;
			}));

		// 4. 싸움 - 이길 만한 적이 있고 피가 충분하면 (또는 그 적의 피가 아주 적으면).
		// 손놀림 (2026-10-20): 버프 → 약화 → 피해 스킬 → 돌진 → 평타 (원거리는 거리를 두고, 평타 사이엔 옆걸음) → 다가가기
		AsComposite(root).Add(MakeAction("fight", [base](Context& c)
			{
				const World& w = c.world;
				Blackboard& bb = c.bb;
				const Persona& p = bb.persona;
				const BrainConfig cfg = Tuned(base, p);
				if (w.self.casting || w.self.dashing)
				{
					c.out.type = ActionType::Idle;
					c.out.reason = "casting";
					return true;
				}
				const Creature* target = BestTarget(w, cfg, &p);
				if (target == nullptr)
					return false;
				if (w.self.HpRatio() < cfg.engageHpRatio && target->HpRatio() > cfg.finishHpRatio)
					return false;
				// 도망칠 만큼 다쳤으면 마무리도 손이 닿고 내가 이기고 있을 때만 - 멀리 있는 빈사의 적을 쫓다가 도망 ↔ 싸움을 오가지 않는다 (실기동 2026-10-20)
				if (w.self.HpRatio() < cfg.fleeHpRatio
					&& (target->HpRatio() >= w.self.HpRatio() || Dist(w.self.x, w.self.y, target->x, target->y) > AttackRange(w, cfg) + FINISH_REACH_EXTRA_CM))
					return false;
				// 머릿수 - 적 플레이어가 몰려 있으면 붙지 않는다 (피하기 가지가 무게중심에서 멀어진다). 마무리할 적은 예외
				float gx = 0.f, gy = 0.f;
				if (p.outnumberedAt > 0 && w.self.HpRatio() < OUTNUMBERED_HP && target->HpRatio() > cfg.finishHpRatio
					&& HostilePlayersNear(w, cfg.threatRadiusCm, gx, gy) >= p.outnumberedAt)
					return false;

				bb.targetId = target->id;
				bb.lastSeenId = target->id;
				bb.lastSeenX = target->x;
				bb.lastSeenY = target->y;
				bb.lastSeenUs = w.nowUs;

				const float d = Dist(w.self.x, w.self.y, target->x, target->y);
				const float range = AttackRange(w, cfg);
				auto use = [&](const SkillSlot& s, const char* why)
				{
					c.out.type = ActionType::UseSkill;
					c.out.slot = s.slot;
					c.out.targetId = target->id;
					c.out.x = target->x;
					c.out.y = target->y;
					c.out.reason = why;
					return true;
				};

				if (d <= range + BUFF_REACH_EXTRA_CM)
				{
					if (const SkillSlot* buff = ReadyRole(w, SkillRole::Buff, d))
						return use(*buff, "buff before the clash");
				}
				if (const SkillSlot* debuff = ReadyRole(w, SkillRole::Debuff, d))
					return use(*debuff, "debuff opener");
				if (const SkillSlot* skill = ReadySkill(w, true, d))
					return use(*skill, "damage skill ready");
				if (d > range + ENGAGE_MIN_GAP_CM)
				{
					if (const SkillSlot* engage = ReadyRole(w, SkillRole::Engage, d))
						return use(*engage, "gap closer");
				}

				const bool attackReady = (w.self.attackReadyAtUs <= w.nowUs);
				if (d <= range)
				{
					// 원거리 - 적이 너무 가까우면 평타를 기다리는 동안 물러선다 (카이팅)
					if (w.self.ranged && attackReady == false && d < range * KITE_MIN_RATIO)
					{
						float x = 0.f, y = 0.f;
						if (AwayFrom(w, target->x, target->y, range * KITE_TO_RATIO - d, x, y))
						{
							c.out.type = ActionType::MoveTo;
							c.out.targetId = target->id;
							c.out.x = x;
							c.out.y = y;
							c.out.reason = "kiting";
							return true;
						}
					}
					// 평타 사이 옆걸음 - 근접은 상대 둘레를 돌고, 원거리는 옆으로 비켜 선다 (투사체 · 조준 스킬을 빗나가게)
					if (attackReady == false && p.strafe > 0.f)
					{
						if (bb.strafeFlipUs <= w.nowUs)
						{
							if (bb.Random01() < 0.5f)
								bb.strafeSign = -bb.strafeSign;
							bb.strafeFlipUs = w.nowUs + STRAFE_FLIP_MIN_US + static_cast<uint64>(bb.Random01() * static_cast<float>(STRAFE_FLIP_MAX_US - STRAFE_FLIP_MIN_US));
						}
						float rx = w.self.x - target->x;
						float ry = w.self.y - target->y;
						const float len = std::sqrt(rx * rx + ry * ry);
						if (len < 1.f) { rx = 1.f; ry = 0.f; }
						else { rx /= len; ry /= len; }
						// 둘레를 따라 호 길이 side 만큼 돈다 - 반경을 지켜야 windup 끝 사거리 재검사에 걸리지 않는다
						const float radius = w.self.ranged ? d : std::clamp(d, range * STRAFE_MIN_RATIO, range * STRAFE_MAX_RATIO);
						const float side = STRAFE_STEP_CM * p.strafe * bb.strafeSign;
						const float turn = side / std::max(radius, 50.f);
						const float cs = std::cos(turn), sn = std::sin(turn);
						const float x = target->x + (rx * cs - ry * sn) * radius;
						const float y = target->y + (rx * sn + ry * cs) * radius;
						if (w.walkable == nullptr || w.walkable(x, y))
						{
							c.out.type = ActionType::MoveTo;
							c.out.targetId = target->id;
							c.out.x = x;
							c.out.y = y;
							c.out.reason = "strafing between swings";
							return true;
						}
						bb.strafeSign = -bb.strafeSign;		// 벽 - 다음엔 반대쪽
					}
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

		// 5. 피하기 - 센 적(무리 · 강한 플레이어)이 가깝거나, 적 플레이어가 몰려 있으면 멀어진다
		AsComposite(root).Add(MakeAction("avoid", [base](Context& c)
			{
				const World& w = c.world;
				const Persona& p = c.bb.persona;
				const BrainConfig cfg = Tuned(base, p);
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
				Creature group;
				const char* why = "stronger enemy near";
				if (worst == nullptr && p.outnumberedAt > 0 && w.self.HpRatio() < OUTNUMBERED_HP)
				{
					if (HostilePlayersNear(w, cfg.threatRadiusCm, group.x, group.y) >= p.outnumberedAt)
					{
						worst = &group;		// 무게중심에서 멀어진다
						why = "outnumbered";
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
				c.out.reason = why;
				return true;
			}));

		// 6. 쫓기 - 싸우던 대상이 시야 밖으로 사라졌으면 마지막 자리까지 가 본다 (chase_memory 동안)
		AsComposite(root).Add(MakeAction("chase", [base](Context& c)
			{
				const World& w = c.world;
				Blackboard& bb = c.bb;
				const Persona& p = bb.persona;
				if (p.chaseMemoryUs == 0 || bb.lastSeenUs == 0 || w.nowUs - bb.lastSeenUs > p.chaseMemoryUs)
					return false;
				if (w.self.HpRatio() < Tuned(base, p).engageHpRatio)
					return false;		// 다친 몸으로 안개 속까지 쫓지 않는다
				for (const Creature& cr : w.creatures)
				{
					if (cr.id == bb.lastSeenId)
						return false;		// 아직 보인다 - 싸움 가지가 고르지 않은 것 (센 적 · 피 부족) - 쫓지 않는다
				}
				if (Dist(w.self.x, w.self.y, bb.lastSeenX, bb.lastSeenY) < base.arriveCm)
				{
					bb.lastSeenUs = 0;		// 왔는데 없다 - 잊는다
					return false;
				}
				c.out.type = ActionType::MoveTo;
				c.out.targetId = bb.lastSeenId;
				c.out.x = bb.lastSeenX;
				c.out.y = bb.lastSeenY;
				c.out.reason = "chasing the last sighting";
				return true;
			}));

		// 7. 줍기 - 가까운 가방 · 상자
		AsComposite(root).Add(MakeAction("loot", [base](Context& c)
			{
				const World& w = c.world;
				const BrainConfig cfg = Tuned(base, c.bb.persona);
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

		// 8. 탐색 - 아무것도 없으면 걷는다 (결정적 난수, 벽은 피한다). 성격에 따라 상자 자리 · 집 · 타워를 둘러보고, 닿으면 잠깐 서서 둘러본다
		AsComposite(root).Add(MakeAction("explore", [base](Context& c)
			{
				const World& w = c.world;
				Blackboard& bb = c.bb;
				const Persona& p = bb.persona;
				const BrainConfig& cfg = base;
				if (bb.pauseUntilUs > w.nowUs)
				{
					c.out.type = ActionType::Idle;
					c.out.reason = "looking around";
					return true;
				}
				const bool hasPoint = (bb.wanderX != 0.f || bb.wanderY != 0.f);
				const bool arrived = hasPoint && Dist(w.self.x, w.self.y, bb.wanderX, bb.wanderY) < cfg.arriveCm;
				if (bb.wanderUntilUs <= w.nowUs || arrived || hasPoint == false)
				{
					if (arrived && p.pauseChance > 0.f && bb.Random01() < p.pauseChance)
					{
						bb.pauseUntilUs = w.nowUs + p.pauseMinUs + static_cast<uint64>(bb.Random01() * static_cast<float>(p.pauseMaxUs > p.pauseMinUs ? p.pauseMaxUs - p.pauseMinUs : 0));
						bb.wanderX = 0.f;
						bb.wanderY = 0.f;		// 둘러본 뒤 새 지점
						c.out.type = ActionType::Idle;
						c.out.reason = "looking around";
						return true;
					}
					bool picked = false;
					bool poi = false;
					if (w.pois.empty() == false && p.poiBias > 0.f && bb.Random01() < p.poiBias)
					{
						// 가까운 곳일수록 자주 - 무작위 셋 중 가장 가까운 곳 (방금 다녀온 곳 제외)
						size_t best = SIZE_MAX;
						float bestD = FLT_MAX;
						for (int32 k = 0; k < 3; k++)
						{
							const size_t i = static_cast<size_t>(bb.NextRandom() % static_cast<uint32>(w.pois.size()));
							if (i == bb.lastPoiIndex && w.pois.size() > 1)
								continue;
							const float d = Dist(w.self.x, w.self.y, w.pois[i].x, w.pois[i].y);
							if (d < bestD) { bestD = d; best = i; }
						}
						if (best != SIZE_MAX)
						{
							for (int32 tries = 0; tries < 6 && picked == false; tries++)
							{
								const float a = bb.Random01() * 6.2831853f;
								const float r = POI_JITTER_CM * bb.Random01();
								const float x = w.pois[best].x + std::cos(a) * r;
								const float y = w.pois[best].y + std::sin(a) * r;
								if (w.walkable == nullptr || w.walkable(x, y))
								{
									bb.wanderX = x;
									bb.wanderY = y;
									bb.lastPoiIndex = best;
									picked = true;
									poi = true;
								}
							}
						}
					}
					for (int32 tries = 0; tries < 12 && picked == false; tries++)
					{
						const float a = bb.Random01() * 6.2831853f;
						const float r = cfg.wanderRadiusCm * (0.5f + 0.5f * bb.Random01());
						const float x = w.self.x + std::cos(a) * r;
						const float y = w.self.y + std::sin(a) * r;
						if (w.walkable == nullptr || w.walkable(x, y))
						{
							bb.wanderX = x;
							bb.wanderY = y;
							picked = true;
						}
					}
					// 상자 자리 · 집은 멀 수 있다 - 그 경우엔 도착할 때까지 (최대 5 배) 유지
					bb.wanderUntilUs = w.nowUs + cfg.wanderHoldUs * (poi ? 5 : 1);
				}
				c.out.type = ActionType::MoveTo;
				c.out.x = bb.wanderX;
				c.out.y = bb.wanderY;
				c.out.speedScale = p.exploreSpeed;
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

	/*-------- PersonaBook --------*/

	bool PersonaBook::Parse(const std::string& csv, std::string& error)
	{
		byRole.clear();
		std::vector<std::string> lines, cells;
		SplitLines(csv, lines);
		if (lines.empty())
		{
			error = "bot_personas.csv is empty";
			return false;
		}
		std::vector<std::string> header;
		SplitRow(lines[0], header);
		static const char* names[] = { "role", "flee_hp_scale", "engage_hp_scale", "avoid_power_scale", "engage_radius_scale", "loot_radius_scale",
			"loot_goal_bonus", "escape_early_scale", "reaction_ms_min", "reaction_ms_max", "strafe", "explore_speed", "prefer_players", "prefer_monsters",
			"pause_chance", "pause_ms_min", "pause_ms_max", "poi_bias", "wounded_escape_hp", "outnumbered_at", "chase_memory_s" };
		constexpr size_t N = sizeof(names) / sizeof(names[0]);
		int32 col[N] = {};
		for (size_t i = 0; i < N; i++)
		{
			col[i] = ColumnOf(header, names[i]);
			if (col[i] < 0)
			{
				error = std::string("bot_personas.csv needs column ") + names[i];
				return false;
			}
		}
		for (size_t r = 1; r < lines.size(); r++)
		{
			SplitRow(lines[r], cells);
			if (cells.size() < header.size())
			{
				error = "bot_personas.csv line " + std::to_string(r + 1) + " has too few cells";
				return false;
			}
			float v[N] = {};
			for (size_t i = 1; i < N; i++)
			{
				if (ParseNumber(cells[col[i]], v[i]) == false)
				{
					error = "bot_personas.csv " + cells[col[0]] + " " + names[i] + " must be a number";
					return false;
				}
			}
			// 거부 범위 (bot-loadouts 스펙 § 7) - 조용히 깎지 않는다
			struct Range { size_t i; float lo; float hi; };
			const Range ranges[] = { {1, 0.2f, 5.f}, {2, 0.2f, 5.f}, {3, 0.2f, 5.f}, {4, 0.2f, 5.f}, {5, 0.2f, 5.f}, {6, -10.f, 20.f}, {7, 0.2f, 5.f},
				{8, 0.f, 3000.f}, {9, 0.f, 3000.f}, {10, 0.f, 1.f}, {11, 0.3f, 1.f}, {12, -5.f, 5.f}, {13, -5.f, 5.f}, {14, 0.f, 1.f}, {15, 0.f, 10000.f},
				{16, 0.f, 10000.f}, {17, 0.f, 1.f}, {18, 0.f, 1.f}, {19, 0.f, 10.f}, {20, 0.f, 60.f} };
			for (const Range& g : ranges)
			{
				if (v[g.i] < g.lo || v[g.i] > g.hi)
				{
					error = "bot_personas.csv " + cells[col[0]] + " " + names[g.i] + " must be " + std::to_string(g.lo) + ".." + std::to_string(g.hi);
					return false;
				}
			}
			if (v[8] > v[9] || v[15] > v[16])
			{
				error = "bot_personas.csv " + cells[col[0]] + " min must not exceed max";
				return false;
			}
			Row row;
			Persona& p = row.base;
			p.name = cells[col[0]];
			p.fleeHpScale = v[1];
			p.engageHpScale = v[2];
			p.avoidPowerScale = v[3];
			p.engageRadiusScale = v[4];
			p.lootRadiusScale = v[5];
			p.lootGoalBonus = static_cast<int32>(v[6]);
			p.escapeEarlyScale = v[7];
			row.reactionMinUs = static_cast<uint64>(v[8]) * 1000;
			row.reactionMaxUs = static_cast<uint64>(v[9]) * 1000;
			p.strafe = v[10];
			p.exploreSpeed = v[11];
			p.preferPlayers = v[12];
			p.preferMonsters = v[13];
			p.pauseChance = v[14];
			p.pauseMinUs = static_cast<uint64>(v[15]) * 1000;
			p.pauseMaxUs = static_cast<uint64>(v[16]) * 1000;
			p.poiBias = v[17];
			p.woundedEscapeHp = v[18];
			p.outnumberedAt = static_cast<int32>(v[19]);
			p.chaseMemoryUs = static_cast<uint64>(v[20] * 1'000'000.f);
			if (byRole.emplace(p.name, row).second == false)
			{
				error = "bot_personas.csv duplicate role " + p.name;
				return false;
			}
		}
		if (byRole.empty())
		{
			error = "bot_personas.csv has no rows";
			return false;
		}
		return true;
	}

	Persona PersonaBook::Make(const std::string& role, Blackboard& bb) const
	{
		auto it = byRole.find(role);
		if (it == byRole.end())
			it = byRole.find("balanced");
		if (it == byRole.end())
			return Persona();
		const Row& row = it->second;
		Persona p = row.base;
		// 같은 성격이라도 봇마다 조금씩 다르게 - 배율 ±10 %, 반응 지연은 범위 안 (칠판 난수 - 결정적)
		auto jitter = [&bb](float v) { return v * (0.9f + 0.2f * bb.Random01()); };
		p.fleeHpScale = jitter(p.fleeHpScale);
		p.engageHpScale = jitter(p.engageHpScale);
		p.avoidPowerScale = jitter(p.avoidPowerScale);
		p.engageRadiusScale = jitter(p.engageRadiusScale);
		p.strafe = std::clamp(jitter(p.strafe), 0.f, 1.f);
		p.exploreSpeed = std::clamp(jitter(p.exploreSpeed), 0.3f, 1.f);
		p.reactionUs = row.reactionMinUs + static_cast<uint64>(bb.Random01() * static_cast<float>(row.reactionMaxUs - row.reactionMinUs));
		return p;
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
		const int32 radiusCol = ColumnOf(cells, "radius_cm");		// 없어도 된다 (옛 표) - 반경 스킬의 닿는 거리
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
			e.recast = (kind == "recast");
			float radius = 0.f;
			if (radiusCol >= 0 && radiusCol < static_cast<int32>(cells.size()))
				ParseNumber(cells[radiusCol], radius);
			// 내 둘레 스킬은 반경 안에 있어야 맞는다 - 사거리 열은 같거나 0 이다 (질주처럼 이동 거리인 것도 있다)
			e.reachCm = (shape == "circle_self" && cells[aimCol] == "self_area" && radius > 0.f) ? radius : e.rangeCm;
			if (kind == "passive")
				e.role = SkillRole::None;
			else if (e.selfBuff)
				e.role = SkillRole::Buff;
			else if (e.damage)
				e.role = SkillRole::Damage;
			else if (shape == "single" || shape == "cone" || shape == "circle_point" || (shape == "circle_self" && cells[aimCol] == "self_area"))
				e.role = e.recast ? SkillRole::None : SkillRole::Debuff;		// 표식 · 둔화 · 실명 · 끌기 (차원문 같은 재입력 설치물은 쓰지 않는다)
			else
				e.role = SkillRole::None;		// 벽 세우기 · 분신 · 직선 설치물 - 두뇌가 다루지 않는다
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
			const int32 paramCol = ColumnOf(cells, "param");
			const int32 statCol = ColumnOf(cells, "stat");
			const int32 distCol = ColumnOf(cells, "dist_cm");
			const int32 speedCol = ColumnOf(cells, "speed_cms");
			const int32 durationCol = ColumnOf(cells, "duration_ms");
			const int32 magnitudeCol = ColumnOf(cells, "magnitude");
			if (sid >= 0 && type >= 0)
			{
				for (size_t i = 1; i < lines.size(); i++)
				{
					SplitRow(lines[i], cells);
					if (static_cast<int32>(cells.size()) <= std::max({ sid, type, who, paramCol }))
						continue;
					float f = 0.f;
					if (ParseNumber(cells[sid], f) == false)
						continue;
					auto it = entries.find(static_cast<uint32>(f));
					if (it == entries.end())
						continue;
					const std::string& t = cells[type];
					const bool self = (who < 0) || cells[who] == "self";
					// "그동안 기본 공격만" 버프 - 쓴 뒤 그 시간 동안 다른 스킬을 누르지 않는다 (서버가 basic_only 로 거절한다)
					if (self && t == "buff" && statCol >= 0 && durationCol >= 0 && statCol < static_cast<int32>(cells.size())
						&& durationCol < static_cast<int32>(cells.size()) && cells[statCol] == "basic_only")
					{
						float ms = 0.f;
						if (ParseNumber(cells[durationCol], ms))
							it->second.lockOthersMs = static_cast<uint32>(ms);
					}
					if (self && t == "buff" && statCol >= 0 && durationCol >= 0 && magnitudeCol >= 0 && statCol < static_cast<int32>(cells.size())
						&& durationCol < static_cast<int32>(cells.size()) && magnitudeCol < static_cast<int32>(cells.size()) && cells[statCol] == "attack_range")
					{
						float ms = 0.f, cm = 0.f;
						if (ParseNumber(cells[durationCol], ms) && ParseNumber(cells[magnitudeCol], cm))
						{
							it->second.rangeBuffCm = static_cast<int32>(cm);
							it->second.rangeBuffMs = static_cast<uint32>(ms);
						}
					}
					if (self && (t == "move" || t == "dash" || t == "blink" || t == "teleport" || t == "leap" || t == "backstep"))
					{
						// 피해가 있거나 대상에게 가는 이동은 "붙기" (기습 · 어금니 · 달려들기), 나머지는 내 몸만 옮기는 질주.
						// 뒤로 물러서며 쏘는 것(일보후퇴 back)은 피해 스킬 그대로 - 붙기로 쓰면 사거리 끝에서만 누른다
						Entry& e = it->second;
						const std::string param = (paramCol >= 0) ? cells[paramCol] : std::string();
						const bool hits = (e.role == SkillRole::Damage || e.role == SkillRole::Engage);
						if (e.dashMode == DashMode::None)
						{
							float dist = 0.f, speed = 0.f;
							if (distCol >= 0 && distCol < static_cast<int32>(cells.size())) ParseNumber(cells[distCol], dist);
							if (speedCol >= 0 && speedCol < static_cast<int32>(cells.size())) ParseNumber(cells[speedCol], speed);
							e.dashDistCm = dist;
							e.dashSpeedCms = speed;
							if (param == "back")
								e.dashMode = DashMode::Back;
							else if (param == "to_target" || param == "leap_target")
								e.dashMode = DashMode::ToTarget;
							else if (speed > 0.f && (param == "dash" || param == "leap" || param == "forward" || param == "dash_through" || param == "slide"))
								e.dashMode = DashMode::Aim;
							else
								e.dashMode = DashMode::ServerMoves;
						}
						if (param == "back" && hits)
							continue;
						e.movement = true;
						e.damage = false;
						e.role = (hits || e.needsTarget || param == "to_target" || param == "behind") ? SkillRole::Engage : SkillRole::Dash;
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
		out.role = e.role;
		out.reachCm = e.reachCm;
		out.recast = e.recast;
		out.lockOthersMs = e.lockOthersMs;
		out.rangeBuffCm = e.rangeBuffCm;
		out.rangeBuffMs = e.rangeBuffMs;
		out.dashMode = e.dashMode;
		out.dashDistCm = e.dashDistCm;
		out.dashSpeedCms = e.dashSpeedCms;
		return true;
	}
}
