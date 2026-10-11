#include "pch.h"
#include "Room.h"
#include "Player.h"
#include "Monster.h"
#include "Decoy.h"
#include "SkillTable.h"
#include "SkillKinematics.h"
#include "WallTable.h"
#include "Vision.h"
#include "GameSession.h"

// 2차 스킬 로스터 (src/스킬_목록.csv 게임 ID 1115 ~ 1137 + FoW 를 전제로 한 기존 스킬, 2026-10-11 사용자 지시 · YOLO).
// 결정표: design/quick-specs/skill-roster-2026-10-10.md § 2차 로스터. 수치는 전부 Server/Data/skill_effects.csv.

namespace
{
	constexpr float PI_F = 3.14159265f;

	// 조준 방향 - 조준점이 발밑이면 바라보는 방향 (CollectSkillTargets 와 같은 규칙)
	void AimDir(const CreatureRef& caster, const SkillContext& ctx, OUT float& dirX, OUT float& dirY)
	{
		dirX = ctx.aimX - ctx.originX;
		dirY = ctx.aimY - ctx.originY;
		const float len = ::sqrtf(dirX * dirX + dirY * dirY);
		if (len > 1.f)
		{
			dirX /= len;
			dirY /= len;
			return;
		}
		const float yawRad = caster->posInfo->yaw() * PI_F / 180.f;
		dirX = ::cosf(yawRad);
		dirY = ::sinf(yawRad);
	}

	bool IsEnemyCreature(const ObjectRef& object)
	{
		return object->IsPlayer() || object->IsMonster() || object->IsDecoy();
	}
}

// ── 임시 벽 (지각변동 1122) ─────────────────────────────────────────────

const WallMap& Room::Walls() const
{
	return (_wallsWithDynamic != nullptr) ? *_wallsWithDynamic : WallTable::Get(_floorId);
}

void Room::RebuildWalls()
{
	// 지금 지도를 쥔 참조(const WallMap& walls = Walls())가 이 잡 안에 남아 있을 수 있다 - 바로 지우지 않고 다음 Flush 에서 버린다
	if (_wallsWithDynamic != nullptr)
		_retiredWalls.push_back(_wallsWithDynamic);

	if (_dynWalls.empty())
	{
		_wallsWithDynamic.reset();
		return;
	}

	// 층 벽(771) + 임시 벽으로 지도를 다시 짓는다 - 격자 CSR 이라 O(n), 1 ms 미만. 벽이 생기고 사라질 때만 (쿨다운 15 s)
	vector<WallBox> all = WallTable::Get(_floorId).Walls();
	for (const DynamicWall& w : _dynWalls)
		all.push_back(w.box);

	auto map = make_shared<WallMap>();
	string error;
	if (map->Build(move(all), OUT error) == false)
	{
		LOG_LINE << "[WALL] rebuild failed - " << error << " (temporary walls ignored)\n";
		_wallsWithDynamic.reset();
		return;
	}
	_wallsWithDynamic = map;
}

void Room::RaiseWall(const CreatureRef& caster, const SkillDef& def, const SkillEffect& e, const SkillContext& ctx, uint64 nowUs)
{
	float dirX = 1.f;
	float dirY = 0.f;
	AimDir(caster, ctx, OUT dirX, OUT dirY);

	// 바라보는 방향으로 일직선 - 시전자 앞 radius_cm 에서 시작해 dist_cm 길이 (설명 "바라보는 방향으로 일직선의 벽이 솟아오른다")
	const float startCm = e.radiusCm > 0.f ? e.radiusCm : 150.f;
	const float halfLength = e.distCm * 0.5f;
	const float halfThickness = (std::max)(10.f, e.magnitude * 0.5f);

	DynamicWall w;
	w.box.kind = WallKind::Wall;
	w.box.cx = ctx.originX + dirX * (startCm + halfLength);
	w.box.cy = ctx.originY + dirY * (startCm + halfLength);
	w.box.halfLength = halfLength;
	w.box.halfThickness = halfThickness;
	w.box.yawDeg = ::atan2f(dirY, dirX) * 180.f / PI_F;
	w.zoneId = _nextZoneId++;
	w.endUs = nowUs + static_cast<uint64>(e.durationMs) * 1000;
	w.casterId = ctx.casterId;
	w.effectId = def.effectId;

	_dynWalls.push_back(w);
	RebuildWalls();
	if (_wallsWithDynamic == nullptr)
	{
		_dynWalls.pop_back();		// 층 밖으로 나가는 자리 등 - 짓지 못했다
		return;
	}

	// 벽이 솟는 자리에 서 있던 생물은 가장 가까운 빈 자리로 밀려난다 - 벽 속에 갇히지 않게
	WallBox probe = w.box;
	WallGeom::ComputeAxis(probe);
	const WallMap& walls = Walls();
	for (auto& item : _objects)
	{
		CreatureRef c = dynamic_pointer_cast<Creature>(item.second);
		if (c == nullptr || c->IsAlive() == false)
			continue;
		if (WallGeom::SignedDistance(probe, c->posInfo->x(), c->posInfo->y()) >= SkillKinematics::CAPSULE_RADIUS_CM)
			continue;

		float outX = 0.f;
		float outY = 0.f;
		if (walls.FindNearestFree(c->posInfo->x(), c->posInfo->y(), SkillKinematics::CAPSULE_RADIUS_CM, 400.f, OUT outX, OUT outY) == false)
			continue;

		c->posInfo->set_x(outX);
		c->posInfo->set_y(outY);
		_dirtyMovers.insert(item.first);
		if (PlayerRef p = dynamic_pointer_cast<Player>(c))
		{
			Protocol::S_MOVE snap;
			snap.set_correction(true);
			snap.add_infos()->CopyFrom(*p->posInfo);
			if (GameSessionRef session = p->session.lock())
			{
				SEND_PACKET(snap);
			}
		}
		LOG_LINE << "[WALL] shove id=" << item.first << " -> (" << ::lroundf(outX) << "," << ::lroundf(outY) << ")" << '\n';
	}

	// 클라 - S_ZONE param wall: 그 자리에 막히는 상자를 세우고(콜리전) 안개 벽에도 넣는다
	Protocol::ZoneInfo& info = _pendingZoneSpawns.emplace_back();
	info.set_zone_id(w.zoneId);
	info.set_caster_id(ctx.casterId);
	info.set_effect_id(def.effectId);
	info.set_x(w.box.cx);
	info.set_y(w.box.cy);
	info.set_radius_cm(halfLength);
	info.set_duration_ms(e.durationMs);
	info.set_param("wall");
	info.set_yaw(w.box.yawDeg);
	info.set_half_length(halfLength);
	info.set_half_thickness(halfThickness);

	LOG_LINE << "[WALL] raise char=" << ctx.casterId << " id=" << w.zoneId << " at=(" << ::lroundf(w.box.cx) << "," << ::lroundf(w.box.cy)
		<< ") len=" << ::lroundf(e.distCm) << " yaw=" << ::lroundf(w.box.yawDeg) << " for=" << e.durationMs << "ms" << '\n';
}

void Room::SendDynamicWalls(const PlayerRef& player, uint64 nowUs)
{
	if (_dynWalls.empty() || player == nullptr)
		return;

	GameSessionRef session = player->session.lock();
	if (session == nullptr)
		return;

	Protocol::S_ZONE pkt;
	for (const DynamicWall& w : _dynWalls)
	{
		if (nowUs >= w.endUs)
			continue;

		Protocol::ZoneInfo* info = pkt.add_spawned();
		info->set_zone_id(w.zoneId);
		info->set_caster_id(w.casterId);
		info->set_effect_id(w.effectId);
		info->set_x(w.box.cx);
		info->set_y(w.box.cy);
		info->set_radius_cm(w.box.halfLength);
		info->set_duration_ms(static_cast<uint32>((w.endUs - nowUs) / 1000));
		info->set_param("wall");
		info->set_yaw(w.box.yawDeg);
		info->set_half_length(w.box.halfLength);
		info->set_half_thickness(w.box.halfThickness);
	}

	if (pkt.spawned_size() > 0)
	{
		SEND_PACKET(pkt);
		LOG_LINE << "[WALL] sent " << pkt.spawned_size() << " standing wall(s) to char=" << player->objectInfo->object_id() << '\n';
	}
}

void Room::UpdateDynamicWalls(uint64 nowUs)
{
	if (_dynWalls.empty())
		return;

	bool changed = false;
	for (size_t i = 0; i < _dynWalls.size(); )
	{
		if (nowUs >= _dynWalls[i].endUs)
		{
			_pendingZoneEnds.push_back(_dynWalls[i].zoneId);
			LOG_LINE << "[WALL] fall id=" << _dynWalls[i].zoneId << '\n';
			_dynWalls.erase(_dynWalls.begin() + i);
			changed = true;
			continue;
		}
		i++;
	}

	if (changed)
		RebuildWalls();
}

// ── 연사 (그림자 던지기 1125 · 그림자 퍼붓기 1127 · 천방지축 1136) ──────────

void Room::StartVolley(const CreatureRef& caster, const SkillDef& def, const SkillEffect& e, const SkillContext& ctx, uint64 nowUs)
{
	// 발 수 = hits + per_level × 강화 레벨 (그림자 던지기 "아이템 레벨이 높을수록 단검 수가 늘어난다"). 상한 12
	const uint32 count = (std::min)(12u, e.hits + static_cast<uint32>(e.perLevel * static_cast<float>(ctx.itemLevel)));

	PendingVolley v;
	v.casterId = ctx.casterId;
	v.skillId = def.skillId;
	v.spreadDeg = e.magnitude;
	v.intervalMs = e.intervalMs;
	v.nextAtUs = nowUs + static_cast<uint64>(e.durationMs) * 1000;		// 천방지축 - 돌진이 끝난 뒤
	v.ctx = ctx;
	v.ctx.targetsOnly = true;
	AimDir(caster, ctx, OUT v.aimDirX, OUT v.aimDirY);

	if (e.intervalMs > 0)
	{
		// 한 발씩 - 그때그때 바라보는 방향 (그림자 퍼붓기 "던지는 동안에도 움직일 수 있다")
		v.remaining = count;
		v.perShot = 1;
		v.followFacing = true;
	}
	else
	{
		// 한 번에 부채꼴 (그림자 던지기 · 천방지축 360°)
		v.remaining = 1;
		v.perShot = count;
		v.followFacing = false;
	}

	_volleys.push_back(v);
	LOG_LINE << "[VOLLEY] start char=" << ctx.casterId << " skill=" << def.skillId << " shots=" << count << " spread=" << e.magnitude
		<< " interval=" << e.intervalMs << "ms delay=" << e.durationMs << "ms" << '\n';

	if (v.nextAtUs <= nowUs)
		UpdateVolleys(nowUs);
}

void Room::LaunchVolleyShot(const CreatureRef& caster, const SkillDef& def, float dirX, float dirY, const SkillContext& ctx, uint64 nowUs)
{
	Projectile p;
	p.casterId = ctx.casterId;
	p.skillId = def.skillId;
	p.effectId = def.effectId;
	p.x = caster->posInfo->x();
	p.y = caster->posInfo->y();
	p.dirX = dirX;
	p.dirY = dirY;
	p.speedCms = def.projectileSpeedCms;
	p.radiusCm = (std::max)(SkillTable::Rules().projectileRadiusCm, def.widthCm * 0.5f);
	p.remainingCm = def.rangeCm;
	p.pierce = def.pierce;
	p.ctx = ctx;
	p.ctx.originX = p.x;
	p.ctx.originY = p.y;
	p.ctx.targetsOnly = true;
	p.lastStepUs = nowUs;

	// 포착 (tags homing:arrow) - 시야 안에 확신의 표식이 남은 적이 있으면 어느 방향으로 쏘든 그 적에게 (가장 가까운 적, 2026-10-11).
	// 없으면 쏜 방향으로 흩어진다. 날아가는 중 벽에 막힌다 (StepHoming)
	const string homingMark = HomingMarkOf(def);
	if (homingMark.empty() == false)
	{
		p.homingTargetId = FindHomingMarkTarget(caster, homingMark, p.x, p.y, 0.f, nowUs);
		if (p.homingTargetId != 0)
			p.remainingCm = HOMING_SKILL_MAX_CM;
	}

	LaunchProjectile(move(p));
}

void Room::UpdateVolleys(uint64 nowUs)
{
	if (_volleys.empty())
		return;

	for (size_t i = 0; i < _volleys.size(); )
	{
		PendingVolley& v = _volleys[i];
		if (nowUs < v.nextAtUs)
		{
			i++;
			continue;
		}

		auto casterIt = _objects.find(v.casterId);
		CreatureRef caster = (casterIt != _objects.end()) ? dynamic_pointer_cast<Creature>(casterIt->second) : nullptr;
		const SkillDef* def = SkillTable::Find(v.skillId);

		// 시전자가 사라졌거나 죽었거나 행동 불능이면 남은 발은 버린다
		if (caster == nullptr || caster->IsAlive() == false || def == nullptr || caster->IsHardCced(nowUs))
		{
			LOG_LINE << "[VOLLEY] stop char=" << v.casterId << " skill=" << v.skillId << " left=" << v.remaining << '\n';
			_volleys.erase(_volleys.begin() + i);
			continue;
		}

		float baseX = v.aimDirX;
		float baseY = v.aimDirY;
		if (v.followFacing)
		{
			const float yawRad = caster->posInfo->yaw() * PI_F / 180.f;
			baseX = ::cosf(yawRad);
			baseY = ::sinf(yawRad);
		}

		// 부채꼴 - perShot 발을 spread 안에 고르게. 360° 는 끝과 처음이 겹치지 않게 n 등분
		const float baseRad = ::atan2f(baseY, baseX);
		const float spreadRad = v.spreadDeg * PI_F / 180.f;
		for (uint32 k = 0; k < v.perShot; k++)
		{
			float offset = 0.f;
			if (v.perShot > 1)
			{
				if (v.spreadDeg >= 359.f)
					offset = spreadRad * static_cast<float>(k) / static_cast<float>(v.perShot);
				else
					offset = -spreadRad * 0.5f + spreadRad * static_cast<float>(k) / static_cast<float>(v.perShot - 1);
			}
			LaunchVolleyShot(caster, *def, ::cosf(baseRad + offset), ::sinf(baseRad + offset), v.ctx, nowUs);
		}

		if (--v.remaining == 0)
		{
			_volleys.erase(_volleys.begin() + i);
			continue;
		}

		v.nextAtUs = nowUs + static_cast<uint64>(v.intervalMs) * 1000;
		i++;
	}
}

// ── 그림자 분신 (그림자 보내기 1126) ─────────────────────────────────────

void Room::SpawnClone(const CreatureRef& caster, const SkillDef& def, const SkillEffect& e, const SkillContext& ctx, uint64 nowUs)
{
	PlayerRef owner = dynamic_pointer_cast<Player>(caster);
	if (owner == nullptr)
		return;

	float dirX = 1.f;
	float dirY = 0.f;
	AimDir(caster, ctx, OUT dirX, OUT dirY);

	// 떡밥과 같은 인형 - 체력 1 (맞으면 사라진다). 다른 사람에겐 시전자와 똑같은 플레이어로 보인다 (S_SPAWN CREATE_TYPE_PLAYER)
	auto clone = make_shared<Decoy>(GNextDecoyId.fetch_add(1), ctx.casterId, nowUs + static_cast<uint64>(e.durationMs) * 1000, 1);
	clone->posInfo->set_x(caster->posInfo->x());
	clone->posInfo->set_y(caster->posInfo->y());
	clone->posInfo->set_z(caster->posInfo->z());
	clone->posInfo->set_yaw(::atan2f(dirY, dirX) * 180.f / PI_F);
	clone->posInfo->set_state(Protocol::MOVE_STATE_RUN);
	clone->walkDirX = dirX;
	clone->walkDirY = dirY;
	clone->walkSpeedCms = e.speedCms;
	clone->walkRemainingCm = e.distCm;
	clone->isClone = true;

	if (AddObject(clone) == false)
		return;

	const uint64 id = clone->objectInfo->object_id();
	_decoys.insert(id);
	_clones.insert(id);
	_pendingSpawns.push_back(id);

	// 분신의 시야를 본체와 공유한다 - 분신을 따라가는 추가 시야원 (FoW). 분신이 사라지면 다음 갱신이 뺀다
	VisionExtra extra;
	extra.sourceId = id;
	extra.followId = id;
	extra.radiusCm = e.radiusCm;
	extra.needsLos = true;
	extra.kind = "clone";
	AddVisionExtra(owner, extra);

	PushStatus(true, ctx.casterId, Protocol::STATUS_DECOY, "clone", 1, e.durationMs, 0.f, ctx.casterId);
	LOG_LINE << "[CLONE] spawn id=" << id << " owner=" << ctx.casterId << " walk=" << ::lroundf(e.distCm) << "cm @" << ::lroundf(e.speedCms)
		<< " sight=" << ::lroundf(e.radiusCm) << '\n';
}

void Room::StepClones(uint64 nowUs)
{
	if (_clones.empty())
		return;

	const WallMap& walls = Walls();
	const float dtSec = 0.033f;		// FlushMoves 주기 (MOVE_FLUSH_MS) - 분신은 정확한 시계가 필요 없다

	const vector<uint64> ids(_clones.begin(), _clones.end());
	for (uint64 id : ids)
	{
		auto it = _objects.find(id);
		if (it == _objects.end() || it->second->IsDecoy() == false)
		{
			_clones.erase(id);
			continue;
		}

		Decoy* clone = static_cast<Decoy*>(it->second.get());
		if (clone->walkRemainingCm <= 0.f)
			continue;

		const float step = (std::min)(clone->walkRemainingCm, clone->walkSpeedCms * dtSec);
		const float x0 = clone->posInfo->x();
		const float y0 = clone->posInfo->y();
		const float x1 = x0 + clone->walkDirX * step;
		const float y1 = y0 + clone->walkDirY * step;

		// 벽에 막히면 거기서 끝 - 걸어갈 만큼 걸어갔다
		const float t = walls.SweepFirstHit(x0, y0, x1, y1, SkillKinematics::CAPSULE_RADIUS_CM);
		clone->posInfo->set_x(x0 + (x1 - x0) * t);
		clone->posInfo->set_y(y0 + (y1 - y0) * t);
		clone->walkRemainingCm -= step;
		_dirtyMovers.insert(id);

		if (t < 1.f || clone->walkRemainingCm <= 0.f)
		{
			clone->walkRemainingCm = 0.f;
			clone->expiresUs = (std::min)(clone->expiresUs, nowUs + 300'000);		// 멈춘 자리에서 잠깐 서 있다가 사라진다 (UpdateDecoys)
			clone->posInfo->set_state(Protocol::MOVE_STATE_IDLE);
			LOG_LINE << "[CLONE] stop id=" << id << (t < 1.f ? " why=wall" : " why=distance") << '\n';
		}
	}
}

// ── 종이매 (1135) · 맹수의 탐색 (1032) ───────────────────────────────────

void Room::CastHawk(const CreatureRef& caster, const SkillDef& def, const SkillEffect& e, uint64 nowUs)
{
	PlayerRef owner = dynamic_pointer_cast<Player>(caster);
	if (owner == nullptr)
		return;

	const uint64 ownerId = owner->objectInfo->object_id();
	const float ox = owner->posInfo->x();
	const float oy = owner->posInfo->y();

	// 가장 가까운 적 (플레이어 · 몬스터 · 인형). 은신은 매도 못 찾는다
	CreatureRef nearest = nullptr;
	float bestSq = FLT_MAX;
	for (auto& item : _objects)
	{
		if (item.first == ownerId || IsEnemyCreature(item.second) == false)
			continue;
		CreatureRef c = static_pointer_cast<Creature>(item.second);
		if (c->IsAlive() == false || c->IsStealthed(nowUs))
			continue;
		if (c->IsDecoy() && static_pointer_cast<Decoy>(c)->ownerId == ownerId)
			continue;

		const float dx = c->posInfo->x() - ox;
		const float dy = c->posInfo->y() - oy;
		const float d2 = dx * dx + dy * dy;
		if (d2 < bestSq)
		{
			bestSq = d2;
			nearest = c;
		}
	}

	if (nearest == nullptr)
	{
		LOG_LINE << "[HAWK] char=" << ownerId << " nobody on the floor" << '\n';
		return;
	}

	const uint64 targetId = nearest->objectInfo->object_id();

	if (bestSq <= e.distCm * e.distCm)
	{
		// 주변에 적이 있다 - 그 적 둘레를 맴돌며 시야를 밝힌다 (공중이라 벽을 넘는다)
		VisionExtra extra;
		extra.sourceId = GNextDecoyId.fetch_add(1);		// 표시용 id - 객체는 아니다
		extra.followId = targetId;
		extra.radiusCm = e.radiusCm;
		extra.untilUs = nowUs + static_cast<uint64>(e.durationMs) * 1000;
		extra.needsLos = false;
		extra.kind = "hawk";		// 클라가 그 적 머리 위를 맴도는 매를 그린다 (2026-10-11 "종이매가 안 보여")
		AddVisionExtra(owner, extra);
		LOG_LINE << "[HAWK] circle char=" << ownerId << " over=" << targetId << " dist=" << ::lroundf(::sqrtf(bestSq)) << " for=" << e.durationMs << "ms" << '\n';
		return;
	}

	// 없다 - 가장 가까운 적에게 날아가 머리 위에서 사라진다: 그 적을 잠깐 공개하고 그 자리를 알린다
	RevealTo(owner, targetId, nowUs + static_cast<uint64>(e.intervalMs) * 1000);

	Protocol::S_SKILL_HIT ping;
	Protocol::SkillHitInfo* hit = ping.add_hits();
	hit->set_caster_id(ownerId);
	hit->set_effect_id(def.effectId);
	hit->set_target_id(targetId);
	hit->set_impact_x(nearest->posInfo->x());
	hit->set_impact_y(nearest->posInfo->y());
	hit->set_aim_x(nearest->posInfo->x());
	hit->set_aim_y(nearest->posInfo->y());
	if (GameSessionRef session = owner->session.lock())
	{
		SEND_PACKET(ping);
	}

	LOG_LINE << "[HAWK] fly char=" << ownerId << " to=" << targetId << " dist=" << ::lroundf(::sqrtf(bestSq)) << " reveal=" << e.intervalMs << "ms" << '\n';
}

void Room::StartVisionArea(const CreatureRef& caster, const SkillDef& def, const SkillEffect& e, uint64 nowUs)
{
	PlayerRef owner = dynamic_pointer_cast<Player>(caster);
	if (owner == nullptr)
		return;

	// 맹수의 탐색 - 땅 울림이 닿는 둘레. param walls = 벽 너머, stealth = 은신까지 ("보이지 않는 적들을 탐지한다")
	VisionExtra extra;
	extra.sourceId = GNextDecoyId.fetch_add(1);
	extra.x = owner->posInfo->x();
	extra.y = owner->posInfo->y();
	extra.radiusCm = e.radiusCm;
	extra.untilUs = nowUs + static_cast<uint64>(e.durationMs) * 1000;
	extra.needsLos = (e.param.find("walls") == string::npos);
	extra.seesStealth = (e.param.find("stealth") != string::npos);
	extra.kind = "detect";
	AddVisionExtra(owner, extra);

	LOG_LINE << "[DETECT] char=" << owner->objectInfo->object_id() << " skill=" << def.skillId << " r=" << ::lroundf(e.radiusCm)
		<< " for=" << e.durationMs << "ms walls=" << (extra.needsLos ? 0 : 1) << " stealth=" << (extra.seesStealth ? 1 : 0) << '\n';

	// param ping - 둘레의 적 자리에 표시를 띄운다 (2026-10-11 "적이 있는 위치를 표시해 줬으면")
	if (e.param.find("ping") != string::npos)
		SendDetectionPings(owner, def, e.radiusCm, e.durationMs, nowUs);
}

// ── 그림자 밟기 (1128) ─────────────────────────────────────────────────

void Room::MoveBehindTarget(const CreatureRef& caster, const SkillDef& def, const SkillEffect& e, const SkillContext& ctx, uint64 nowUs)
{
	PlayerRef player = dynamic_pointer_cast<Player>(caster);
	auto targetIt = _objects.find(ctx.targetId);
	CreatureRef target = (targetIt != _objects.end()) ? dynamic_pointer_cast<Creature>(targetIt->second) : nullptr;
	if (player == nullptr || target == nullptr || target->IsAlive() == false)
		return;

	// 등 뒤 = 대상이 바라보는 방향의 반대쪽 dist_cm
	const float yawRad = target->posInfo->yaw() * PI_F / 180.f;
	const float wantX = target->posInfo->x() - ::cosf(yawRad) * e.distCm;
	const float wantY = target->posInfo->y() - ::sinf(yawRad) * e.distCm;

	float outX = wantX;
	float outY = wantY;
	if (Walls().FindNearestFree(wantX, wantY, SkillKinematics::CAPSULE_RADIUS_CM, 200.f, OUT outX, OUT outY) == false)
	{
		LOG_LINE << "[BEHIND] failed - no free spot char=" << ctx.casterId << '\n';
		return;
	}

	player->posInfo->set_x(outX);
	player->posInfo->set_y(outY);
	player->posInfo->set_yaw(target->posInfo->yaw());		// 대상과 같은 쪽을 본다 = 등을 마주 본다
	_dirtyMovers.insert(ctx.casterId);

	Protocol::S_MOVE snap;
	snap.set_correction(true);
	snap.add_infos()->CopyFrom(*player->posInfo);
	if (GameSessionRef session = player->session.lock())
	{
		SEND_PACKET(snap);
	}

	LOG_LINE << "[BEHIND] char=" << ctx.casterId << " -> " << ctx.targetId << " at=(" << ::lroundf(outX) << "," << ::lroundf(outY) << ")" << '\n';
}

// ── 보호막 (전사의 다짐 1079) ──────────────────────────────────────────

void Room::ApplyShield(const CreatureRef& caster, const SkillDef& def, const SkillEffect& e, uint64 nowUs)
{
	const uint64 id = caster->objectInfo->object_id();
	caster->shieldAmount = (std::max)(1, static_cast<int32>(::lroundf(caster->maxHp * e.magnitude)));
	caster->shieldUntilUs = nowUs + static_cast<uint64>(e.durationMs) * 1000;
	caster->shieldSkillId = def.skillId;
	_windowOwners.insert(id);

	PushStatus(true, id, Protocol::STATUS_BUFF, "shield", 1, e.durationMs, static_cast<float>(caster->shieldAmount), id);
	LOG_LINE << "[SHIELD] char=" << id << " amount=" << caster->shieldAmount << " for=" << e.durationMs << "ms" << '\n';
}

// ── 평타 모드 (혼원일체 1115 · 여진 1116 · 난무 1120 · 고슴도치 1121) ─────────

void Room::ApplyBasicProcs(const CreatureRef& attacker, const CreatureRef& target, uint64 nowUs)
{
	const uint64 attackerId = attacker->objectInfo->object_id();

	// basic_proc 버프마다 그 스킬의 on_basic 효과 - radius_cm > 0 이면 맞은 적 둘레 전부(충격파)
	vector<uint32> procSkills;
	for (const BuffInstance& b : attacker->status.Buffs())
	{
		if (b.stat == SkillBuffStat::BasicProc && nowUs < b.endUs && find(procSkills.begin(), procSkills.end(), b.skillId) == procSkills.end())
			procSkills.push_back(b.skillId);
	}

	for (uint32 skillId : procSkills)
	{
		const SkillDef* def = SkillTable::Find(skillId);
		if (def == nullptr)
			continue;

		SkillContext ctx;
		ctx.casterId = attackerId;
		ctx.originX = attacker->posInfo->x();
		ctx.originY = attacker->posInfo->y();
		ctx.aimX = target->posInfo->x();
		ctx.aimY = target->posInfo->y();
		ctx.targetsOnly = true;

		float shockRadius = 0.f;
		bool touchedTarget = false;
		for (uint32 i = 0; i < def->effects.size(); i++)
		{
			const SkillEffect& e = def->effects[i];
			if (e.condition != SkillRequire::OnBasic || e.self)
				continue;

			if (e.radiusCm <= 0.f)
			{
				if (target->IsAlive())
				{
					ApplyTargetEffect(attacker, *def, i, target, ctx, nowUs);
					touchedTarget = true;
				}
				continue;
			}

			// 충격파 - 맞은 적 둘레의 적 전부 (맞은 적 포함)
			shockRadius = (std::max)(shockRadius, e.radiusCm);
			const float cx = target->posInfo->x();
			const float cy = target->posInfo->y();
			vector<CreatureRef> around;
			for (auto& item : _objects)
			{
				if (item.first == attackerId || IsEnemyCreature(item.second) == false)
					continue;
				CreatureRef c = static_pointer_cast<Creature>(item.second);
				if (c->IsAlive() == false || CanProjectileHit(attacker, c, nowUs) == false)
					continue;
				const float dx = c->posInfo->x() - cx;
				const float dy = c->posInfo->y() - cy;
				if (dx * dx + dy * dy <= e.radiusCm * e.radiusCm)
					around.push_back(c);
			}
			for (const CreatureRef& c : around)
				ApplyTargetEffect(attacker, *def, i, c, ctx, nowUs);
		}

		if (shockRadius > 0.f)
			PushVisualZone(attackerId, def->effectId, target->posInfo->x(), target->posInfo->y(), shockRadius, 450, "shock");

		// 혼원일체 - 원혼이 덮치는 연출을 맞은 적에게 (2026-10-11 "추가 피해가 안 들어간다" - 들어갔지만 보이지 않았다)
		if (touchedTarget)
			PushTargetVisual(attackerId, def->effectId, target);

		LOG_LINE << "[PROC] char=" << attackerId << " skill=" << skillId << " on=" << target->objectInfo->object_id()
			<< (shockRadius > 0.f ? " shock r=" : "") << (shockRadius > 0.f ? to_string(::lroundf(shockRadius)) : string()) << '\n';
	}
}

void Room::ApplyBasicSpin(const CreatureRef& attacker, const CreatureRef& mainTarget, float radiusCm, uint64 nowUs)
{
	const uint64 attackerId = attacker->objectInfo->object_id();
	const float cx = attacker->posInfo->x();
	const float cy = attacker->posInfo->y();

	int32 hits = 0;
	for (auto& item : _objects)
	{
		if (item.first == attackerId || IsEnemyCreature(item.second) == false)
			continue;
		CreatureRef c = static_pointer_cast<Creature>(item.second);
		if (c == mainTarget || c->IsAlive() == false || CanProjectileHit(attacker, c, nowUs) == false)
			continue;

		const float dx = c->posInfo->x() - cx;
		const float dy = c->posInfo->y() - cy;
		if (dx * dx + dy * dy > radiusCm * radiusCm)
			continue;
		if (HasLineOfAttack(attacker, cx, cy, c) == false)
			continue;

		bool isCrit = false;
		const int32 damage = attacker->ComputeDamageTo(*c, attacker->autoAttackType, OUT isCrit);
		DamageSource src;
		src.basic = true;
		DealDamage(attacker, attackerId, c, damage, attacker->autoAttackType, isCrit, nowUs, src);
		hits++;
	}

	const BuffInstance* spin = attacker->status.FindBuff(SkillBuffStat::BasicSpin);
	PushVisualZone(attackerId, spin != nullptr ? spin->skillId : 0, cx, cy, radiusCm, 300, "spin");
	if (hits > 0)
		LOG_LINE << "[SPIN] char=" << attackerId << " extra=" << hits << " r=" << ::lroundf(radiusCm) << '\n';
}

void Room::TryThorns(const CreatureRef& attacker, const CreatureRef& target, int32 damageTaken, uint64 nowUs)
{
	if (damageTaken <= 0 || attacker == nullptr || attacker->IsAlive() == false || target->IsAlive() == false)
		return;

	const BuffInstance* thorns = target->status.FindBuff(SkillBuffStat::Thorns);
	if (thorns == nullptr || nowUs >= thorns->endUs)
		return;

	// 받은 피해의 일부를 마법 피해로 (사용자 결정 10 %). 방어 계산은 하지 않는다 - 되돌린 몫 그대로
	const int32 reflected = (std::max)(1, static_cast<int32>(::lroundf(damageTaken * thorns->magnitude)));
	DamageSource src;
	src.skillId = thorns->skillId;
	DealDamage(target, target->objectInfo->object_id(), attacker, reflected, Protocol::DAMAGE_TYPE_MAGIC, false, nowUs, src);
	LOG_LINE << "[THORNS] char=" << target->objectInfo->object_id() << " -> " << attacker->objectInfo->object_id() << " " << reflected << '\n';
}

// ── 명경지수 (1130) ────────────────────────────────────────────────────

void Room::UpdatePuddle(Zone& z, uint64 nowUs)
{
	auto it = _objects.find(z.casterId);
	if (it == _objects.end() || it->second->IsPlayer() == false)
		return;

	PlayerRef owner = static_pointer_cast<Player>(it->second);
	const float dx = owner->posInfo->x() - z.x;
	const float dy = owner->posInfo->y() - z.y;
	if (dx * dx + dy * dy > z.radiusCm * z.radiusCm)
		return;

	// 안에 서 있는 동안 평타 사거리 + magnitude - 틱 간격 + 여유만큼, 나가면 곧 풀린다
	BuffInstance buff;
	buff.skillId = z.skillId;
	buff.stat = SkillBuffStat::AttackRange;
	buff.magnitude = z.ccMagnitude;
	buff.endUs = nowUs + static_cast<uint64>(z.intervalMs + 200) * 1000;
	const bool fresh = owner->status.AddBuff(buff);
	owner->OnBuffsChanged();
	_statusTargets.insert(z.casterId);

	if (fresh)
	{
		PushStatus(true, z.casterId, Protocol::STATUS_BUFF, SkillTable::BuffStatName(SkillBuffStat::AttackRange), 1, z.intervalMs + 200, z.ccMagnitude, z.casterId);
		LOG_LINE << "[PUDDLE] char=" << z.casterId << " in zone " << z.id << " - attack range +" << ::lroundf(z.ccMagnitude) << " -> " << ::lroundf(owner->attackRange) << '\n';
	}
}

// ── 연출만 하는 장판 (충격파 · 사방 베기) ────────────────────────────────

void Room::PushVisualZone(uint64 casterId, uint32 effectId, float x, float y, float radiusCm, uint32 durationMs, const char* param)
{
	Protocol::ZoneInfo& info = _pendingZoneSpawns.emplace_back();
	info.set_zone_id(_nextZoneId++);
	info.set_caster_id(casterId);
	info.set_effect_id(effectId);
	info.set_x(x);
	info.set_y(y);
	info.set_radius_cm(radiusCm);
	info.set_duration_ms(durationMs);
	info.set_param(param);
	// 끝 패킷은 보내지 않는다 - 클라가 duration 뒤에 지운다
}
