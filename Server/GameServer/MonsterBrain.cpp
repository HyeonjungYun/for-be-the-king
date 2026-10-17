#include "pch.h"
#include "MonsterBrain.h"
#include "NavGrid.h"
#include "FloorBounds.h"
#include "SkillTable.h"
#include "Player.h"
#include <cmath>

namespace
{
	float Dist(float ax, float ay, float bx, float by)
	{
		const float dx = ax - bx;
		const float dy = ay - by;
		return ::sqrtf(dx * dx + dy * dy);
	}

	const BrainPlayer* FindPlayer(const vector<BrainPlayer>& players, uint64 id)
	{
		for (const BrainPlayer& p : players)
		{
			if (p.id == id)
				return &p;
		}
		return nullptr;
	}

	void ResetStuck(MonsterBrainState& s)
	{
		s.stuckAccumUs = 0;
		s.windowStartUs = 0;
		s.windowProgressCm = 0.f;
	}

	void EnterChase(MonsterBrainState& s, uint64 targetId, uint64 nowUs)
	{
		s.state = MonsterState::Chase;
		s.stateSinceUs = nowUs;
		s.targetId = targetId;
		s.lastEngagedUs = nowUs;
		s.losLostSinceUs = 0;
		ResetStuck(s);

		if (MonsterBrain::ThreatOf(s, targetId) <= 0.f)
			MonsterBrain::AddThreat(s, targetId, 1.f);
	}

	void EnterSearch(MonsterBrainState& s, uint64 nowUs)
	{
		s.state = MonsterState::Search;
		s.stateSinceUs = nowUs;
		s.targetId = 0;
		s.searchArrivedUs = 0;
		ResetStuck(s);
	}

	void EnterReturn(MonsterBrainState& s, uint64 nowUs, BrainOutput& out)
	{
		// evade - 위협표를 비운다. 복귀 중에는 어그로가 없다
		s.state = MonsterState::Return;
		s.stateSinceUs = nowUs;
		s.returnStartUs = nowUs;
		s.targetId = 0;
		s.threat.clear();
		s.pendingAttackerId = 0;
		ResetStuck(s);
		out.enteredReturn = true;
	}

	void EnterIdle(MonsterBrainState& s, uint64 nowUs)
	{
		s.state = MonsterState::Idle;
		s.stateSinceUs = nowUs;
		s.targetId = 0;
		s.threat.clear();
		s.pendingAttackerId = 0;
		s.lockoutUntilUs = nowUs + MonsterConfig::REAGGRO_LOCKOUT_US;
		ResetStuck(s);
	}

	float Score(const MonsterBrainState& s, const MonsterTypeDef& def, const BrainInput& in, const BrainPlayer& p)
	{
		return MonsterBrain::ThreatOf(s, p.id) * MonsterBrain::DistanceWeight(def, Dist(in.x, in.y, p.x, p.y));
	}

	// F3 - 점수 최댓값, 동점이면 가까운 쪽. 현재 목표는 1.25 배를 넘는 후보가 있을 때만, windup 중이 아닐 때만 바뀐다
	uint64 SelectTarget(const MonsterBrainState& s, const MonsterTypeDef& def, const BrainInput& in)
	{
		const vector<BrainPlayer>& players = *in.players;

		const BrainPlayer* best = nullptr;
		float bestScore = -1.f;
		float bestDist = 0.f;

		for (const ThreatEntry& t : s.threat)
		{
			const BrainPlayer* p = FindPlayer(players, t.id);
			if (p == nullptr)
				continue;

			const float score = Score(s, def, in, *p);
			const float dist = Dist(in.x, in.y, p->x, p->y);
			if (score > bestScore || (score == bestScore && dist < bestDist))
			{
				best = p;
				bestScore = score;
				bestDist = dist;
			}
		}

		if (best == nullptr)
			return 0;

		const BrainPlayer* current = FindPlayer(players, s.targetId);
		if (current == nullptr || MonsterBrain::ThreatOf(s, s.targetId) <= 0.f)
			return best->id;

		if (best->id == current->id || in.windingUp)
			return current->id;

		return (bestScore >= MonsterConfig::RETARGET_RATIO * Score(s, def, in, *current)) ? best->id : current->id;
	}

	// T1 · T2 · T3 - 추격 · 수색 공통 포기 조건
	bool ShouldGiveUp(const MonsterBrainState& s, const MonsterTypeDef& def, const BrainInput& in)
	{
		if (Dist(in.x, in.y, in.homeX, in.homeY) > def.leashRadiusCm)
			return true;

		if (in.nowUs - s.lastEngagedUs > static_cast<uint64>(def.giveUpS * 1'000'000.0))
			return true;

		return s.stuckAccumUs >= MonsterConfig::STUCK_US;
	}

	bool LineClear(const BrainInput& in, float bx, float by)
	{
		return in.lineClear == nullptr || in.lineClear(in.x, in.y, bx, by);
	}

	// § L - 원거리가 물러날 자리. 목표 반대쪽부터 ±40 · ±80° 로 비껴 보며, 몸이 갈 수 있고 · 목표에서 지금보다 멀고 · 끈 안쪽인 첫 자리
	bool FindRetreat(const BrainInput& in, const MonsterTypeDef& def, const BrainPlayer& target, float distNow, OUT float& outX, OUT float& outY)
	{
		float ax = in.x - target.x;
		float ay = in.y - target.y;
		const float len = ::sqrtf(ax * ax + ay * ay);
		if (len < 1.f)
		{
			ax = 1.f;
			ay = 0.f;
		}
		else
		{
			ax /= len;
			ay /= len;
		}

		const float angles[] = { 0.f, 40.f, -40.f, 80.f, -80.f };
		for (float a : angles)
		{
			const float rad = a * 3.14159265f / 180.f;
			const float rx = ax * ::cosf(rad) - ay * ::sinf(rad);
			const float ry = ax * ::sinf(rad) + ay * ::cosf(rad);
			const float px = in.x + rx * MonsterConfig::RETREAT_STEP_CM;
			const float py = in.y + ry * MonsterConfig::RETREAT_STEP_CM;

			if (Dist(px, py, target.x, target.y) <= distNow + 50.f)
				continue;
			if (def.leashRadiusCm > 0.f && Dist(px, py, in.homeX, in.homeY) > def.leashRadiusCm - MonsterConfig::RETREAT_LEASH_MARGIN_CM)
				continue;

			const bool clear = (in.bodyClear != nullptr) ? in.bodyClear(in.x, in.y, px, py) : LineClear(in, px, py);
			if (clear == false)
				continue;

			outX = px;
			outY = py;
			return true;
		}
		return false;
	}
}

MonsterCastDecision MonsterSkillUse::Decide(const SkillDef& skill, float x, float y, const BrainPlayer* target, const vector<BrainPlayer>& players, const LineClearFn& lineClear)
{
	MonsterCastDecision c;

	switch (skill.shape)
	{
	case SkillShape::CircleSelf:
	{
		// 보스 광역 - 유효 목표 하나라도 반경 - 50 안 (예전 규칙 그대로). 조준은 발밑
		const float trigger = skill.radiusCm - MonsterConfig::SKILL_TRIGGER_MARGIN_CM;
		for (const BrainPlayer& p : players)
		{
			if (Dist(x, y, p.x, p.y) <= trigger)
			{
				c.cast = true;
				c.aimX = x;
				c.aimY = y;
				return c;
			}
		}
		return c;
	}

	case SkillShape::Line:
	case SkillShape::Projectile:
	{
		if (target == nullptr)
			return c;

		const float d = Dist(x, y, target->x, target->y);
		const float minD = (skill.shape == SkillShape::Line) ? MonsterConfig::CHARGE_MIN_CM : 0.f;
		if (d < minD || d > skill.rangeCm)
			return c;
		if (lineClear != nullptr && lineClear(x, y, target->x, target->y) == false)
			return c;

		c.cast = true;
		c.targetId = target->id;
		c.aimX = target->x;
		c.aimY = target->y;
		return c;
	}

	default:
		return c;
	}
}

const char* MonsterStateName(MonsterState state)
{
	switch (state)
	{
	case MonsterState::Idle:	return "idle";
	case MonsterState::Chase:	return "chase";
	case MonsterState::Search:	return "search";
	case MonsterState::Return:	return "return";
	case MonsterState::Dead:	return "dead";
	default:					return "?";
	}
}

float MonsterBrain::DistanceWeight(const MonsterTypeDef& def, float distCm)
{
	if (distCm <= def.attackRangeCm)
		return 2.0f;

	if (distCm <= 2.f * def.aggroRadiusCm)
		return 1.0f;

	return 0.25f;
}

void MonsterBrain::AddThreat(MonsterBrainState& s, uint64 id, float amount)
{
	for (ThreatEntry& t : s.threat)
	{
		if (t.id == id)
		{
			t.threat += amount;
			return;
		}
	}

	if (static_cast<int32>(s.threat.size()) < MonsterConfig::THREAT_MAX)
	{
		s.threat.push_back({ id, amount });
		return;
	}

	// 33 번째 - 가장 낮은 항목보다 크면 그 자리를 뺏는다 (Edge Cases)
	auto lowest = min_element(s.threat.begin(), s.threat.end(), [](const ThreatEntry& a, const ThreatEntry& b) { return a.threat < b.threat; });
	if (lowest->threat < amount)
		*lowest = { id, amount };
}

float MonsterBrain::ThreatOf(const MonsterBrainState& s, uint64 id)
{
	for (const ThreatEntry& t : s.threat)
	{
		if (t.id == id)
			return t.threat;
	}
	return 0.f;
}

void MonsterBrain::OnDamaged(MonsterBrainState& s, uint64 attackerId, int32 damage, float attackerX, float attackerY, uint64 nowUs)
{
	if (s.state == MonsterState::Dead || s.state == MonsterState::Return)
		return;

	AddThreat(s, attackerId, static_cast<float>(max(damage, 0)));
	s.lastEngagedUs = nowUs;

	if (s.state == MonsterState::Idle || s.state == MonsterState::Search)
	{
		s.pendingAttackerId = attackerId;
		s.pendingAttackerX = attackerX;
		s.pendingAttackerY = attackerY;
	}
}

void MonsterBrain::NoteEngaged(MonsterBrainState& s, uint64 nowUs)
{
	if (s.state == MonsterState::Chase || s.state == MonsterState::Search)
		s.lastEngagedUs = nowUs;
}

bool MonsterBrain::Alert(MonsterBrainState& s, uint64 targetId, uint64 nowUs)
{
	if (s.state != MonsterState::Idle || nowUs < s.lockoutUntilUs || targetId == 0)
		return false;

	AddThreat(s, targetId, 1.f);
	EnterChase(s, targetId, nowUs);
	return true;
}

void MonsterBrain::Kill(MonsterBrainState& s, uint64 nowUs)
{
	s.state = MonsterState::Dead;
	s.stateSinceUs = nowUs;
	s.targetId = 0;
	s.threat.clear();
	s.pendingAttackerId = 0;
}

void MonsterBrain::Reset(MonsterBrainState& s, uint64 nowUs)
{
	s = MonsterBrainState();
	s.state = MonsterState::Idle;
	s.stateSinceUs = nowUs;
	s.lockoutUntilUs = nowUs + MonsterConfig::REAGGRO_LOCKOUT_US;
}

BrainOutput MonsterBrain::Think(MonsterBrainState& s, const MonsterTypeDef& def, const BrainInput& in)
{
	BrainOutput out;
	if (s.state == MonsterState::Dead || in.players == nullptr)
		return out;

	const vector<BrainPlayer>& players = *in.players;
	const uint64 now = in.nowUs;

	// 사망 · 퇴장 · 탈출한 플레이어는 그 틱에 위협표에서 지운다 (§ 12)
	s.threat.erase(remove_if(s.threat.begin(), s.threat.end(),
		[&players](const ThreatEntry& t) { return FindPlayer(players, t.id) == nullptr; }), s.threat.end());

	if (s.pendingAttackerId != 0 && FindPlayer(players, s.pendingAttackerId) == nullptr)
		s.pendingAttackerId = 0;

	switch (s.state)
	{
	case MonsterState::Idle:
	{
		const uint64 attackerId = s.pendingAttackerId;
		s.pendingAttackerId = 0;

		if (now < s.lockoutUntilUs)
		{
			s.threat.clear();
			break;
		}

		// T9 - 맞았다. 쏜 자리가 추격 한계 밖이면 쫓지 않는다 (스나이핑 방지)
		if (attackerId != 0)
		{
			if (Dist(s.pendingAttackerX, s.pendingAttackerY, in.homeX, in.homeY) > def.leashRadiusCm)
			{
				s.threat.clear();
				break;
			}

			EnterChase(s, attackerId, now);
			out.aggroed = true;
			break;
		}

		// T8 - 감지 반경 안 · 시야가 트인 플레이어. 전원 시드 1, 점수(같으면 가까운 쪽)로 하나를 고른다
		const BrainPlayer* chosen = nullptr;
		float chosenScore = -1.f;
		float chosenDist = 0.f;

		for (const BrainPlayer& p : players)
		{
			const float d = Dist(in.x, in.y, p.x, p.y);
			if (d > def.aggroRadiusCm || LineClear(in, p.x, p.y) == false)
				continue;

			AddThreat(s, p.id, 1.f);
			const float score = ThreatOf(s, p.id) * DistanceWeight(def, d);
			if (score > chosenScore || (score == chosenScore && d < chosenDist))
			{
				chosen = &p;
				chosenScore = score;
				chosenDist = d;
			}
		}

		if (chosen != nullptr)
		{
			EnterChase(s, chosen->id, now);
			out.aggroed = true;
		}
		break;
	}

	case MonsterState::Chase:
	{
		if (ShouldGiveUp(s, def, in))
		{
			EnterReturn(s, now, out);
			break;
		}

		s.pendingAttackerId = 0;	// 추격 중 피격은 위협만 쌓는다 (OnDamaged)

		// T4 - 쫓을 사람이 없다
		const uint64 targetId = SelectTarget(s, def, in);
		if (targetId == 0)
		{
			EnterReturn(s, now, out);
			break;
		}

		if (targetId != s.targetId)
		{
			s.targetId = targetId;
			s.losLostSinceUs = 0;
		}

		const BrainPlayer* target = FindPlayer(players, targetId);
		const float d = Dist(in.x, in.y, target->x, target->y);
		const bool seen = LineClear(in, target->x, target->y);

		if (seen)
		{
			s.losLostSinceUs = 0;
			s.lastSeenX = target->x;
			s.lastSeenY = target->y;
		}
		else
		{
			if (s.losLostSinceUs == 0)
				s.losLostSinceUs = now;

			// T5 - 벽 뒤로 500 ms. 마지막으로 본 자리를 뒤진다
			if (now - s.losLostSinceUs >= MonsterConfig::LOS_LOSS_US)
			{
				EnterSearch(s, now);
				out.hasGoal = true;
				out.goalX = s.lastSeenX;
				out.goalY = s.lastSeenY;
				break;
			}
		}

		if (d <= def.attackRangeCm)
		{
			s.lastEngagedUs = now;
			if (in.windingUp == false && in.casting == false)
				out.attackTargetId = targetId;
		}

		// 원거리 (§ L) - 멀리서 쏘기만 해도 교전 중. 너무 가까우면 물러난다 (구석이면 그 자리에서 버틴다)
		const bool ranged = def.keepRangeCm > 0.f;
		if (ranged && seen && d <= def.keepRangeCm * MonsterConfig::RANGED_ENGAGE_RATIO)
			s.lastEngagedUs = now;

		// 물러나기는 RETREAT_MAX_US 동안만, 그 뒤 RETREAT_COOLDOWN_US 는 붙어 있어도 맞서 쏜다 - 플레이어가 더 빨라서
		// 무한히 물러나면 한 발도 못 쏘고 죽는다 (2026-10-18 봇 실측)
		const bool retreatOpen = now < s.retreatEndUs;
		if (ranged && seen && d < def.kiteMinCm && in.windingUp == false && in.casting == false && (retreatOpen || now >= s.retreatReadyUs))
		{
			if (retreatOpen == false)
			{
				s.retreatEndUs = now + MonsterConfig::RETREAT_MAX_US;
				s.retreatReadyUs = s.retreatEndUs + MonsterConfig::RETREAT_COOLDOWN_US;
			}

			float rx = 0.f;
			float ry = 0.f;
			if (FindRetreat(in, def, *target, d, OUT rx, OUT ry))
			{
				out.attackTargetId = 0;
				out.retreating = true;
				out.hasGoal = true;
				out.goalX = rx;
				out.goalY = ry;
				out.stopDistCm = 0.f;
				break;
			}

			s.retreatEndUs = now;		// 구석 - 물러날 자리가 없다. 쉬는 시간은 그대로 (그동안 맞서 쏜다)
		}

		// 몬스터 스킬 - 쿨다운 끝 · 비시전 · windup 아님. 모양마다 조건이 다르다 (MonsterSkillUse)
		if (def.skillId != 0 && now >= s.skillReadyAtUs && in.casting == false && in.windingUp == false)
		{
			if (const SkillDef* skill = SkillTable::Find(def.skillId))
			{
				const MonsterCastDecision c = MonsterSkillUse::Decide(*skill, in.x, in.y, target, players, in.lineClear);
				if (c.cast)
				{
					out.castSkill = true;
					out.castTargetId = c.targetId;
					out.castAimX = c.aimX;
					out.castAimY = c.aimY;
					out.attackTargetId = 0;
				}
			}
		}

		// 시야가 끊긴 500 ms 동안은 마지막으로 본 자리로 간다 - 벽 너머 위치를 몬스터가 알지 못한다.
		// 원거리는 보이는 동안 keep 까지만 다가가 그 자리에서 쏜다
		out.hasGoal = true;
		out.goalX = s.lastSeenX;
		out.goalY = s.lastSeenY;
		out.stopDistCm = (ranged && seen) ? def.keepRangeCm : def.attackRangeCm * MonsterConfig::STOP_DIST_RATIO;
		break;
	}

	case MonsterState::Search:
	{
		if (ShouldGiveUp(s, def, in))
		{
			EnterReturn(s, now, out);
			break;
		}

		// T6 - 위협표 속 누군가를 다시 본다 (감지 반경 × 1.5 안)
		const BrainPlayer* seen = nullptr;
		float seenScore = -1.f;
		for (const ThreatEntry& t : s.threat)
		{
			const BrainPlayer* p = FindPlayer(players, t.id);
			if (p == nullptr)
				continue;

			const float d = Dist(in.x, in.y, p->x, p->y);
			if (d > def.aggroRadiusCm * MonsterConfig::SEARCH_REACQUIRE_MULT || LineClear(in, p->x, p->y) == false)
				continue;

			const float score = Score(s, def, in, *p);
			if (score > seenScore)
			{
				seen = p;
				seenScore = score;
			}
		}

		if (seen != nullptr)
		{
			EnterChase(s, seen->id, now);
			s.lastSeenX = seen->x;
			s.lastSeenY = seen->y;
			out.hasGoal = true;
			out.goalX = seen->x;
			out.goalY = seen->y;
			out.stopDistCm = def.attackRangeCm * MonsterConfig::STOP_DIST_RATIO;
			break;
		}

		// T9 - 수색 중 맞았다
		const uint64 attackerId = s.pendingAttackerId;
		s.pendingAttackerId = 0;
		if (attackerId != 0)
		{
			if (Dist(s.pendingAttackerX, s.pendingAttackerY, in.homeX, in.homeY) > def.leashRadiusCm)
			{
				EnterReturn(s, now, out);
				break;
			}

			EnterChase(s, attackerId, now);
			s.lastSeenX = s.pendingAttackerX;
			s.lastSeenY = s.pendingAttackerY;
			out.hasGoal = true;
			out.goalX = s.lastSeenX;
			out.goalY = s.lastSeenY;
			out.stopDistCm = def.attackRangeCm * MonsterConfig::STOP_DIST_RATIO;
			break;
		}

		// T7 - 마지막 본 자리에 닿고 search_s 가 지났다
		if (Dist(in.x, in.y, s.lastSeenX, s.lastSeenY) <= MonsterConfig::SEARCH_ARRIVE_CM)
		{
			if (s.searchArrivedUs == 0)
				s.searchArrivedUs = now;

			if (now - s.searchArrivedUs >= static_cast<uint64>(def.searchS * 1'000'000.0))
			{
				EnterReturn(s, now, out);
				break;
			}
		}

		out.hasGoal = true;
		out.goalX = s.lastSeenX;
		out.goalY = s.lastSeenY;
		break;
	}

	case MonsterState::Return:
	{
		s.pendingAttackerId = 0;

		// T11 - 집에 닿았다. 만피 · CC 해제 · 1 초 잠금
		if (Dist(in.x, in.y, in.homeX, in.homeY) <= MonsterConfig::ARRIVE_TOL_CM)
		{
			EnterIdle(s, now);
			out.resetOnArrive = true;
			break;
		}

		// T12 - 10 초 안에 못 돌아왔다 (넉백 뒤 · 미끄러짐 실패로 갇힘). 집으로 순간 이동
		if (now - s.returnStartUs >= MonsterConfig::RETURN_TIMEOUT_US)
		{
			EnterIdle(s, now);
			out.resetOnArrive = true;
			out.snapHome = true;
			break;
		}

		out.hasGoal = true;
		out.goalX = in.homeX;
		out.goalY = in.homeY;
		out.speedMult = MonsterConfig::RETURN_SPEED_MULT;
		break;
	}

	default:
		break;
	}

	return out;
}

namespace
{
	// § K - 직선이 막혔으면 길의 다음 꺾는 점을 걸음의 목표로 바꾼다. 길이 없거나 못 구하면 그대로 (직선 + 미끄러짐)
	void Steer(MonsterBrainState& s, const StepInput& in, StepOutput& out, float& goalX, float& goalY, float& stopDist)
	{
		const uint64 now = in.nowUs;

		// 목표가 많이 움직였거나 길이 오래됐다 - 버리고 다시 (임시 벽 · 밀려난 자리)
		if (s.path.empty() == false
			&& (Dist(in.goalX, in.goalY, s.pathGoalX, s.pathGoalY) > MonsterConfig::PATH_GOAL_SLACK_CM || now - s.pathBuiltUs >= MonsterConfig::PATH_MAX_AGE_US))
			s.path.clear();

		if (now >= s.directCheckUs)
		{
			s.directCheckUs = now + MonsterConfig::PATH_CHECK_US;
			s.directClear = (in.blocked(in.x, in.y, in.goalX, in.goalY, in.radiusCm) == false);
		}

		if (s.directClear)
		{
			s.path.clear();
			return;
		}

		if (s.path.empty() && now >= s.pathRetryUs && (in.pathBudget == nullptr || *in.pathBudget > 0))
		{
			if (in.pathBudget != nullptr)
				(*in.pathBudget)--;

			NavGrid::PathResult r = in.nav->FindPath(in.x, in.y, in.goalX, in.goalY, in.radiusCm, in.blocked);
			out.pathSearched = true;
			out.pathFound = r.found;
			out.pathExpansions = r.expansions;

			if (r.found && r.points.empty() == false)
			{
				s.path = move(r.points);
				s.pathIndex = 0;
				s.pathGoalX = in.goalX;
				s.pathGoalY = in.goalY;
				s.pathBuiltUs = now;
			}
			else
			{
				s.pathRetryUs = now + MonsterConfig::PATH_RETRY_US;
			}
		}

		if (s.path.empty())
			return;

		const uint32 last = static_cast<uint32>(s.path.size()) - 1;
		while (s.pathIndex < last && Dist(in.x, in.y, s.path[s.pathIndex].first, s.path[s.pathIndex].second) <= MonsterConfig::PATH_REACH_CM)
			s.pathIndex++;

		// 한 점 앞이 이미 트였으면 건너뛴다 (밀려났거나 비껴 걸어 지름길이 생겼다) - 걸음당 한 번만 쓸어 본다
		if (s.pathIndex < last)
		{
			const float nx = (s.pathIndex + 1 == last) ? in.goalX : s.path[s.pathIndex + 1].first;
			const float ny = (s.pathIndex + 1 == last) ? in.goalY : s.path[s.pathIndex + 1].second;
			if (in.blocked(in.x, in.y, nx, ny, in.radiusCm) == false)
				s.pathIndex++;
		}

		out.onPath = true;
		if (s.pathIndex == last)
			return;		// 마지막 구간 = 지금 목표 그대로 (움직이는 목표를 따라간다)

		goalX = s.path[s.pathIndex].first;
		goalY = s.path[s.pathIndex].second;
		stopDist = 0.f;
	}
}

StepOutput MonsterMover::Step(MonsterBrainState& s, const StepInput& in)
{
	StepOutput out;
	out.x = in.x;
	out.y = in.y;

	// 닿았거나 서 있어야 한다 - 막힘이 아니다
	if (Dist(in.x, in.y, in.goalX, in.goalY) <= in.stopDistCm + 0.5f)
	{
		out.arrived = true;
		ResetStuck(s);
		s.path.clear();
		return out;
	}

	const uint64 dtUs = min(in.dtUs, MonsterConfig::MAX_STEP_US);
	if (in.speedCms <= 0.f || dtUs == 0)
	{
		s.windowStartUs = 0;
		s.windowProgressCm = 0.f;
		return out;
	}

	// 걸음의 목표 - 길을 따르면 다음 꺾는 점 (§ K)
	float goalX = in.goalX;
	float goalY = in.goalY;
	float stopDist = in.stopDistCm;
	if (in.nav != nullptr && in.blocked != nullptr)
		Steer(s, in, out, goalX, goalY, stopDist);

	const float dx = goalX - in.x;
	const float dy = goalY - in.y;
	const float dist = ::sqrtf(dx * dx + dy * dy);
	if (dist <= stopDist + 0.5f)
		return out;

	const float len = min(in.speedCms * static_cast<float>(dtUs) / 1'000'000.f, dist - stopDist);
	const float hx = dx / dist;
	const float hy = dy / dist;

	const float limit = FloorBounds::HALF_CM - in.radiusCm;

	auto tryMove = [&](float rx, float ry)
	{
		const float bx = in.x + rx * len;
		const float by = in.y + ry * len;

		if (std::isfinite(bx) == false || std::isfinite(by) == false || ::fabsf(bx) > limit || ::fabsf(by) > limit)
			return false;

		if (in.blocked != nullptr && in.blocked(in.x, in.y, bx, by, in.radiusCm))
			return false;

		out.x = bx;
		out.y = by;
		out.moved = true;
		out.yawDeg = ::atan2f(ry, rx) * 180.f / 3.14159265f;
		return true;
	};

	const bool sticky = in.nowUs < s.slideUntilUs;

	if (tryMove(hx, hy) == false)
	{
		// 직전에 비껴간 월드 방향을 그대로 - 목표 방향 기준 ±90° 를 매번 다시 재면 벽면과 평행에서 아주 조금 벽 쪽으로
		// 기운 방향이 막혀 위아래로 떨었다 (2026-10-07 테스트에서 발견). 목표에서 135° 넘게 벗어나는 방향만 버린다 -
		// 벽 끝을 돌 때는 목표와의 각이 커진다(2 m 벽 끝에서 104°). 긴 벽은 진척 0 이라 막힘(T3)이 끊는다
		if (sticky && (s.slideDirX * hx + s.slideDirY * hy) > -0.7f && tryMove(s.slideDirX, s.slideDirY))
		{
			s.slideUntilUs = in.nowUs + MonsterConfig::SLIDE_STICKY_US;
		}
		else
		{
			// ±30 → ±60 → ±90. 직전에 비껴간 쪽을 먼저 (벽을 따라 한쪽으로 미끄러진다)
			const float sign = sticky ? static_cast<float>(s.slideSign) : 1.f;
			const float angles[] = { 30.f, -30.f, 60.f, -60.f, 90.f, -90.f };

			for (float a : angles)
			{
				const float rad = a * sign * 3.14159265f / 180.f;
				const float rx = hx * ::cosf(rad) - hy * ::sinf(rad);
				const float ry = hx * ::sinf(rad) + hy * ::cosf(rad);

				if (tryMove(rx, ry) == false)
					continue;

				s.slideSign = (a * sign > 0.f) ? 1 : -1;
				s.slideUntilUs = in.nowUs + MonsterConfig::SLIDE_STICKY_US;
				s.slideDirX = rx;
				s.slideDirY = ry;
				break;
			}
		}
	}

	// 진척 = 목표 방향으로 간 거리 (투영). 1 초 창마다 50 cm 미만이면 그 창만큼 막힘이 쌓인다
	const float progress = out.moved ? ((out.x - in.x) * hx + (out.y - in.y) * hy) : 0.f;

	if (s.windowStartUs == 0)
		s.windowStartUs = in.nowUs - dtUs;

	s.windowProgressCm += progress;

	if (in.nowUs - s.windowStartUs >= MonsterConfig::STUCK_WINDOW_US)
	{
		if (s.windowProgressCm < MonsterConfig::STUCK_PROGRESS_CM)
			s.stuckAccumUs += in.nowUs - s.windowStartUs;
		else
			s.stuckAccumUs = 0;

		s.windowStartUs = in.nowUs;
		s.windowProgressCm = 0.f;
	}

	return out;
}

vector<ItemEntry> MonsterDrop::Roll(const MonsterTypeDef& type, const ChestPool* pool, mt19937& rng, const InstanceIdIssuer& nextId)
{
	vector<ItemEntry> items;
	if (type.drops == false || type.dropRolls == 0 || pool == nullptr)
		return items;

	if (uniform_int_distribution<uint32>(1, 100)(rng) > type.dropChancePct)
		return items;

	for (uint32 r = 0; r < type.dropRolls; r++)
	{
		vector<ItemEntry> rolled = ItemRoller::RollChest(*pool, rng, nextId);
		items.insert(items.end(), rolled.begin(), rolled.end());
	}

	return items;
}
