#include "pch.h"
#include "MonsterPack.h"
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

	bool Clear(const PackInput& in, float ax, float ay, float bx, float by)
	{
		return in.lineClear == nullptr || in.lineClear(ax, ay, bx, by);
	}

	// 목표가 타워 둘레 안이면 테두리까지만 (무리는 탈출 구역에 들어가지 않는다) - 몸이 있는 쪽 테두리
	void ClampOutOfNoGo(const PackInput& in, float fromX, float fromY, IN OUT float& gx, IN OUT float& gy)
	{
		if (in.noGo == nullptr)
			return;

		for (const pair<float, float>& t : *in.noGo)
		{
			const float d = Dist(gx, gy, t.first, t.second);
			if (d >= MonsterConfig::TOWER_NO_GO_CM)
				continue;

			float ox = fromX - t.first;
			float oy = fromY - t.second;
			float od = ::sqrtf(ox * ox + oy * oy);
			if (od < 1.f)
			{
				ox = gx - t.first;
				oy = gy - t.second;
				od = d;
			}
			if (od < 1.f)
			{
				ox = 1.f;
				oy = 0.f;
				od = 1.f;
			}
			gx = t.first + ox / od * MonsterConfig::TOWER_NO_GO_CM;
			gy = t.second + oy / od * MonsterConfig::TOWER_NO_GO_CM;
		}
	}

	// 교전 시작 - 지금 자리가 각자의 앵커 ("처음 어그로가 끌린 위치")
	void EnterEngage(PackState& p, const PackInput& in, uint64 targetId)
	{
		p.mode = PackMode::Engage;
		p.modeSinceUs = in.nowUs;
		p.targetId = targetId;
		p.lastEngagedUs = in.nowUs;
		p.lastRetargetUs = in.nowUs;
		p.allStuckSinceUs = 0;

		for (const PackMemberView& view : *in.living)
		{
			PackMember& m = p.members[view.index];
			m.anchorX = view.x;
			m.anchorY = view.y;
			m.arrived = false;
		}
	}

	// 무리 전체가 목표에게 - 어그로를 함께 쓰므로 목표가 어디 있는지 안다
	void FillChase(PackState& p, const PackInput& in, const BrainPlayer& target, OUT PackOutput& out)
	{
		const vector<PackMemberView>& living = *in.living;
		for (size_t k = 0; k < living.size(); k++)
		{
			const PackMemberView& view = living[k];
			BrainOutput& o = out.perMember[k];

			float gx = target.x;
			float gy = target.y;
			ClampOutOfNoGo(in, view.x, view.y, gx, gy);

			o.hasGoal = true;
			o.goalX = gx;
			o.goalY = gy;
			o.stopDistCm = view.attackRangeCm * MonsterConfig::STOP_DIST_RATIO;
			o.speedMult = 1.f;

			const float d = Dist(view.x, view.y, target.x, target.y);
			if (d <= view.attackRangeCm)
			{
				p.lastEngagedUs = in.nowUs;
				if (view.windingUp == false && view.casting == false && Clear(in, view.x, view.y, target.x, target.y))
					o.attackTargetId = target.id;
			}
		}
	}

	void EnterReturn(PackState& p, const PackInput& in, const char* reason, OUT PackOutput& out)
	{
		p.mode = PackMode::Return;
		p.modeSinceUs = in.nowUs;
		p.targetId = 0;
		p.pendingAttackerId = 0;
		p.allStuckSinceUs = 0;

		for (const PackMemberView& view : *in.living)
		{
			PackMember& m = p.members[view.index];
			m.arrived = false;
			// 돌아가는 시간 + 여유 - 예전 고정 10 초는 긴 추격 뒤에 늘 순간 이동이 됐다
			const float dist = Dist(view.x, view.y, m.anchorX, m.anchorY);
			const float speed = (std::max)(1.f, view.moveSpeedCms * MonsterConfig::RETURN_SPEED_MULT);
			m.returnDeadlineUs = in.nowUs + MonsterConfig::RETURN_TIMEOUT_US + static_cast<uint64>(1.5f * dist / speed * 1'000'000.f);
		}

		out.enteredReturn = true;
		out.returnReason = reason;
	}
}

// ── PackTrail ───────────────────────────────────────────────────────────

void PackTrail::Reset(float x, float y)
{
	_pts.clear();
	_pts.push_back({ x, y, 0.f });
	_headX = x;
	_headY = y;
}

void PackTrail::Seed(const vector<pair<float, float>>& tailToHead)
{
	_pts.clear();
	float s = 0.f;
	for (size_t i = 0; i < tailToHead.size(); i++)
	{
		if (i > 0)
			s += Dist(tailToHead[i - 1].first, tailToHead[i - 1].second, tailToHead[i].first, tailToHead[i].second);
		_pts.push_back({ tailToHead[i].first, tailToHead[i].second, s });
	}
	if (_pts.empty() == false)
	{
		_headX = _pts.back().x;
		_headY = _pts.back().y;
	}
}

float PackTrail::HeadS() const
{
	if (_pts.empty())
		return 0.f;
	const P& b = _pts.back();
	return b.s + Dist(b.x, b.y, _headX, _headY);
}

void PackTrail::Record(float x, float y)
{
	_headX = x;
	_headY = y;

	if (_pts.empty())
	{
		_pts.push_back({ x, y, 0.f });
		return;
	}

	const P& b = _pts.back();
	const float d = Dist(b.x, b.y, x, y);
	if (d >= MonsterConfig::TRAIL_STEP_CM)
		_pts.push_back({ x, y, b.s + d });

	// 맨 뒤 몸이 밟을 만큼만 남긴다
	const float minS = HeadS() - _maxLenCm;
	while (_pts.size() > 2 && _pts[1].s <= minS)
		_pts.pop_front();
}

bool PackTrail::PointAt(float s, OUT float& x, OUT float& y) const
{
	if (_pts.empty())
		return false;

	const float headS = HeadS();
	if (s >= headS)
	{
		x = _headX;
		y = _headY;
		return true;
	}

	if (s <= _pts.front().s)
	{
		x = _pts.front().x;
		y = _pts.front().y;
		return true;
	}

	const P& b = _pts.back();
	if (s >= b.s)
	{
		const float span = headS - b.s;
		const float t = (span > 0.0001f) ? (s - b.s) / span : 0.f;
		x = b.x + (_headX - b.x) * t;
		y = b.y + (_headY - b.y) * t;
		return true;
	}

	// s 를 품은 칸 - 점이 30 개 안팎이라 이진 탐색
	size_t lo = 0;
	size_t hi = _pts.size() - 1;
	while (hi - lo > 1)
	{
		const size_t mid = (lo + hi) / 2;
		if (_pts[mid].s <= s)
			lo = mid;
		else
			hi = mid;
	}

	const P& a = _pts[lo];
	const P& c = _pts[hi];
	const float span = c.s - a.s;
	const float t = (span > 0.0001f) ? (s - a.s) / span : 0.f;
	x = a.x + (c.x - a.x) * t;
	y = a.y + (c.y - a.y) * t;
	return true;
}

void PackTrail::TruncateTo(float s)
{
	float x = 0.f;
	float y = 0.f;
	if (PointAt(s, OUT x, OUT y) == false)
		return;

	while (_pts.size() > 1 && _pts.back().s > s)
		_pts.pop_back();
	if (_pts.back().s > s)
		_pts.back() = { x, y, s };

	_headX = x;
	_headY = y;
}

// ── PackBrain ───────────────────────────────────────────────────────────

const char* PackModeName(PackMode mode)
{
	switch (mode)
	{
	case PackMode::Patrol:	return "patrol";
	case PackMode::Engage:	return "engage";
	case PackMode::Return:	return "return";
	default:				return "?";
	}
}

vector<pair<float, float>> PackBrain::FilePositions(const PatrolRoute& route, uint32 startSeq, uint32 count, float spacingCm)
{
	vector<pair<float, float>> out;
	const size_t n = route.points.size();
	if (n == 0 || count == 0)
		return out;

	uint32 idx = startSeq % static_cast<uint32>(n);
	float cx = route.points[idx].first;
	float cy = route.points[idx].second;
	out.emplace_back(cx, cy);

	// 뒤의 몸은 길을 거슬러 spacing 간격 - 머리가 다음 점으로 걸어가면 그 발자국을 그대로 밟게 된다
	for (uint32 r = 1; r < count; r++)
	{
		float remaining = spacingCm;
		for (size_t guard = 0; guard < n * 4 && remaining > 0.f; guard++)
		{
			const uint32 prev = static_cast<uint32>((idx + n - 1) % n);
			const float px = route.points[prev].first;
			const float py = route.points[prev].second;
			const float seg = Dist(cx, cy, px, py);
			if (seg >= remaining)
			{
				const float t = remaining / (std::max)(seg, 0.0001f);
				cx += (px - cx) * t;
				cy += (py - cy) * t;
				remaining = 0.f;
				break;
			}
			remaining -= seg;
			cx = px;
			cy = py;
			idx = prev;
		}
		out.emplace_back(cx, cy);
	}
	return out;
}

void PackBrain::Spawn(PackState& p, const MonsterPackDef& def, const vector<uint64>& ids, const vector<pair<float, float>>& positions, uint64 nowUs)
{
	p = PackState();
	p.mode = PackMode::Patrol;
	p.modeSinceUs = nowUs;
	p.lockoutUntilUs = nowUs + MonsterConfig::REAGGRO_LOCKOUT_US;
	p.spacingCm = def.spacingCm;

	const size_t n = (def.route != nullptr) ? def.route->points.size() : 0;
	p.waypoint = (n > 0) ? (def.startSeq + 1) % static_cast<uint32>(n) : 0;

	p.members.resize(ids.size());
	for (size_t i = 0; i < ids.size(); i++)
	{
		p.members[i].id = ids[i];
		if (i < positions.size())
		{
			p.members[i].anchorX = positions[i].first;
			p.members[i].anchorY = positions[i].second;
		}
	}

	// 발자국 = 줄 선 자리를 꼬리 → 머리 순으로
	vector<pair<float, float>> tailToHead(positions.rbegin(), positions.rend());
	p.trail.Seed(tailToHead);
	p.trail.SetMaxLength(static_cast<float>(ids.size()) * def.spacingCm + 300.f);
}

void PackBrain::OnMemberDamaged(PackState& p, uint64 attackerId, float attackerX, float attackerY, uint64 nowUs)
{
	// 복귀 중에는 어그로가 없다 (예전 몬스터와 같다 - 끈 끊고 찌르기를 막는다)
	if (p.mode == PackMode::Return || attackerId == 0)
		return;

	p.pendingAttackerId = attackerId;
	p.pendingAttackerX = attackerX;
	p.pendingAttackerY = attackerY;
	if (p.mode == PackMode::Engage)
		p.lastEngagedUs = nowUs;
}

void PackBrain::OnEngaged(PackState& p, uint64 nowUs)
{
	p.lastEngagedUs = nowUs;
}

int32 PackBrain::HeadIndex(const PackState& p)
{
	for (size_t i = 0; i < p.members.size(); i++)
	{
		if (p.members[i].id != 0)
			return static_cast<int32>(i);
	}
	return -1;
}

uint32 PackBrain::LivingCount(const PackState& p)
{
	uint32 n = 0;
	for (const PackMember& m : p.members)
		n += (m.id != 0) ? 1 : 0;
	return n;
}

bool PackBrain::OnMemberDied(PackState& p, uint32 memberIndex, uint64 nowUs)
{
	if (memberIndex >= p.members.size())
		return LivingCount(p) > 0;

	const int32 headBefore = HeadIndex(p);
	p.members[memberIndex].id = 0;

	// 머리가 죽었다 - 다음 몸이 머리 ("우두머리가 죽어도 남은 기본 몬스터는 하던 대로"). 그 몸은 머리 한 칸 뒤에 있었으니
	// 발자국을 그만큼 자른다 - 뒤의 몸들의 목표(머리 - rank × spacing)가 한 칸씩 당겨져 제자리 그대로다
	if (headBefore == static_cast<int32>(memberIndex) && HeadIndex(p) >= 0)
		p.trail.TruncateTo(p.trail.HeadS() - p.spacingCm);

	return LivingCount(p) > 0;
}

BrainOutput PackBrain::PatrolGoal(PackState& p, const MonsterPackDef& def, uint32 livingRank, float x, float y)
{
	BrainOutput o;
	o.hasGoal = true;
	o.stopDistCm = 0.f;
	o.speedMult = 1.f;

	if (livingRank == 0)
	{
		// 머리 - 길의 다음 점. 다가가면 멈추지 않고 그다음 점으로 (점에서 서지 않는다)
		const vector<pair<float, float>>& pts = def.route->points;
		const uint32 n = static_cast<uint32>(pts.size());
		p.waypoint %= n;
		if (Dist(x, y, pts[p.waypoint].first, pts[p.waypoint].second) <= MonsterConfig::WAYPOINT_ARRIVE_CM)
			p.waypoint = (p.waypoint + 1) % n;

		o.goalX = pts[p.waypoint].first;
		o.goalY = pts[p.waypoint].second;
		return o;
	}

	// 뒤의 몸 - 머리 발자국에서 rank × spacing 뒤
	float gx = x;
	float gy = y;
	p.trail.PointAt(p.trail.HeadS() - static_cast<float>(livingRank) * def.spacingCm, OUT gx, OUT gy);
	o.goalX = gx;
	o.goalY = gy;
	return o;
}

PackOutput PackBrain::Think(PackState& p, const PackInput& in)
{
	PackOutput out;
	const vector<PackMemberView>& living = *in.living;
	const vector<BrainPlayer>& players = *in.players;
	const uint64 nowUs = in.nowUs;

	out.perMember.resize(living.size());
	if (living.empty())
		return out;

	// 플레이어가 아닌 공격자(몬스터 · 미끼 · 은신 · 체취 · 무적)는 버린다 - 몬스터끼리는 적이 아니다
	const BrainPlayer* attacker = (p.pendingAttackerId != 0) ? FindPlayer(players, p.pendingAttackerId) : nullptr;
	p.pendingAttackerId = 0;

	switch (p.mode)
	{
	case PackMode::Patrol:
	{
		if (nowUs < p.lockoutUntilUs)
			break;

		// 맞았다 - 탐지형 · 반응형 모두 무리 전체가 그 사람에게 (이 틱부터 쫓는다)
		if (attacker != nullptr)
		{
			EnterEngage(p, in, attacker->id);
			out.aggroTargetId = attacker->id;
			out.aggroBy = "attack";
			FillChase(p, in, *attacker, out);
			break;
		}

		// 탐지형 - 몸 하나라도 감지 반경 안에서 시선이 닿는 가장 가까운 플레이어
		if (in.def != nullptr && in.def->kind == MonsterAggroKind::Detect)
		{
			const BrainPlayer* best = nullptr;
			float bestDist = FLT_MAX;
			for (const PackMemberView& view : living)
			{
				for (const BrainPlayer& pl : players)
				{
					const float d = Dist(view.x, view.y, pl.x, pl.y);
					if (d > view.aggroRadiusCm || d >= bestDist)
						continue;
					if (Clear(in, view.x, view.y, pl.x, pl.y) == false)
						continue;
					best = &pl;
					bestDist = d;
				}
			}

			if (best != nullptr)
			{
				EnterEngage(p, in, best->id);
				out.aggroTargetId = best->id;
				out.aggroBy = "detect";
				FillChase(p, in, *best, out);
			}
		}
		break;
	}

	case PackMode::Engage:
	{
		const BrainPlayer* target = FindPlayer(players, p.targetId);

		// 다른 플레이어가 무리를 때렸다 - 어그로가 그 사람으로 옮겨 간다 (사용자 결정 - 최신 공격자)
		if (attacker != nullptr && attacker->id != p.targetId && nowUs >= p.lastRetargetUs + MonsterConfig::PACK_RETARGET_COOLDOWN_US)
		{
			p.targetId = attacker->id;
			p.lastRetargetUs = nowUs;
			p.lastEngagedUs = nowUs;
			target = attacker;
			out.retargeted = true;
		}

		// 목표가 죽었거나 · 떠났거나 · 숨었다 (은신 · 체취)
		if (target == nullptr)
		{
			EnterReturn(p, in, "target_gone", out);
			break;
		}

		// 반응형 - 몸 하나라도 앵커에서 끈을 벗어나면 그 순간 돌아간다
		if (in.def != nullptr && in.def->kind == MonsterAggroKind::Reactive)
		{
			bool overLeash = false;
			for (const PackMemberView& view : living)
			{
				const PackMember& m = p.members[view.index];
				if (view.leashRadiusCm > 0.f && Dist(view.x, view.y, m.anchorX, m.anchorY) > view.leashRadiusCm)
					overLeash = true;
			}
			if (overLeash)
			{
				EnterReturn(p, in, "leash", out);
				break;
			}
		}

		// 포기 시간 - 반응형만 (탐지형 0 = 둘 중 하나가 죽을 때까지)
		const float giveUpS = living[0].giveUpS;
		if (giveUpS > 0.f && nowUs - p.lastEngagedUs > static_cast<uint64>(giveUpS * 1'000'000.f))
		{
			EnterReturn(p, in, "giveup", out);
			break;
		}

		// 전원이 오래 막혔다 - 길찾기가 없다. 벽 하나를 사이에 두고 영영 비비지 않게
		bool allStuck = true;
		for (const PackMemberView& view : living)
		{
			if (view.stuckAccumUs < MonsterConfig::STUCK_US)
				allStuck = false;
		}
		if (allStuck)
		{
			if (p.allStuckSinceUs == 0)
				p.allStuckSinceUs = nowUs;
			else if (nowUs - p.allStuckSinceUs >= MonsterConfig::PACK_STUCK_GIVEUP_US)
			{
				EnterReturn(p, in, "stuck", out);
				break;
			}
		}
		else
		{
			p.allStuckSinceUs = 0;
		}

		FillChase(p, in, *target, out);
		break;
	}

	case PackMode::Return:
	{
		bool allArrived = true;
		for (size_t k = 0; k < living.size(); k++)
		{
			const PackMemberView& view = living[k];
			PackMember& m = p.members[view.index];

			if (Dist(view.x, view.y, m.anchorX, m.anchorY) <= MonsterConfig::ARRIVE_TOL_CM)
				m.arrived = true;
			if (m.arrived)
				continue;

			allArrived = false;
			BrainOutput& o = out.perMember[k];
			o.hasGoal = true;
			o.goalX = m.anchorX;
			o.goalY = m.anchorY;
			o.stopDistCm = 0.f;
			o.speedMult = MonsterConfig::RETURN_SPEED_MULT;

			// 막혔거나 너무 오래 걸린다 - Room 이 (아무도 안 볼 때) 앵커로 옮긴다
			if (view.stuckAccumUs >= MonsterConfig::STUCK_US || nowUs >= m.returnDeadlineUs || nowUs - p.modeSinceUs >= MonsterConfig::RETURN_HARD_TIMEOUT_US)
				out.snapMembers.push_back(view.index);
		}

		if (allArrived)
		{
			// 다시 걷던 길을 마저 걷는다 - 머리가 향하던 점 그대로
			p.mode = PackMode::Patrol;
			p.modeSinceUs = nowUs;
			p.lockoutUntilUs = nowUs + MonsterConfig::REAGGRO_LOCKOUT_US;
			p.targetId = 0;
			p.cohesionWaitSinceUs = 0;
			out.resumed = true;
		}
		break;
	}
	}

	return out;
}
