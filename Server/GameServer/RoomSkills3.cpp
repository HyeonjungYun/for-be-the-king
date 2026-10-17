#include "pch.h"
#include "Room.h"
#include "Player.h"
#include "Monster.h"
#include "Decoy.h"
#include "SkillTable.h"
#include "SkillKinematics.h"
#include "Vision.h"
#include "GameSession.h"

// 스킬 개선 (2026-10-11 저녁, 사용자 플레이 피드백 "스킬 개선" · YOLO).
// 결정표: design/quick-specs/skill-polish-2026-10-11.md. 수치는 전부 Server/Data/skills.csv · skill_effects.csv.

namespace
{
	constexpr float		PI_F = 3.14159265f;
	constexpr float		STILL_TOLERANCE_CM = 30.f;		// 건곤일척 - 이만큼은 움직여도 제자리 (회전 · 보정 떨림)
	constexpr uint32	PATH_PAINT_GRACE_MS = 250;		// 열화 - 돌진이 끝난 뒤 늦게 도착하는 C_MOVE 를 기다린다
	constexpr float		PATH_JUMP_CM = 700.f;			// 열화 - 한 번에 이보다 멀리 튀면 걸어간 길이 아니다 (보정 · 순간이동)
	constexpr float		PING_RADIUS_CM = 70.f;			// 맹수의 탐색 - 적 자리 표시 크기
	constexpr float		SLAM_RATIO_CAP = 3.f;			// 저 멀리 - 벽 충돌 피해 계수 상한 (skill_effects S3 계수 상한과 같다)
	constexpr float		DASH_SLASH_ANGLE_DEG = 60.f;	// 일보일검 - 질주 끝의 횡베기 각 (사용자 2026-10-11 "60도")
	constexpr uint32	DASH_SLASH_LAG_MS = 60;			// 일보일검 - 질주가 끝나고 베기까지
	constexpr uint32	SPECIAL_BASIC_VISUAL_MS = 300;	// 사방 베기 · 횡베기 연출

	bool IsEnemyCreature(const ObjectRef& object)
	{
		return object->IsPlayer() || object->IsMonster() || object->IsDecoy();
	}

	// from 에서 aim 쪽 단위 벡터. aim 이 발밑이면(또는 0,0 = 조준 없음) 바라보는 방향
	void DirToward(const CreatureRef& c, float fromX, float fromY, float aimX, float aimY, OUT float& dirX, OUT float& dirY)
	{
		dirX = aimX - fromX;
		dirY = aimY - fromY;
		const float len = ::sqrtf(dirX * dirX + dirY * dirY);
		if ((aimX != 0.f || aimY != 0.f) && len > 1.f)
		{
			dirX /= len;
			dirY /= len;
			return;
		}
		const float yawRad = c->posInfo->yaw() * PI_F / 180.f;
		dirX = ::cosf(yawRad);
		dirY = ::sinf(yawRad);
	}
}

// ── 오버로드 1073 · 니힐 1074 - "속성이 없는 상태에서 사용하면 효과가 끝날 때 기절" ──
// 예전엔 발동 순간 걸렸다 (오버로드 = 버프 중에 기절, 니힐 = 자기 무적에 막혀 기절이 안 걸림). 이제 효과가 끝나는 때 건다

void Room::ScheduleSelfCcAtEnd(const CreatureRef& caster, const SkillDef& def, const SkillEffect& e, uint64 nowUs)
{
	// 효과가 끝나는 때 = 이 스킬의 다른 자기 효과 중 가장 긴 지속 (오버로드 = 피해 증가 5 s · 니힐 = 무적 · 은신 3 s)
	uint32 endMs = 0;
	for (const SkillEffect& other : def.effects)
	{
		if (&other == &e || other.self == false || other.type == SkillEffectType::SelfCc)
			continue;
		endMs = (std::max)(endMs, other.durationMs);
	}

	PendingSelfCc p;
	p.casterId = caster->objectInfo->object_id();
	p.skillId = def.skillId;
	p.type = e.ccType;
	p.durationMs = e.durationMs;
	p.magnitude = e.magnitude;
	p.atUs = nowUs + static_cast<uint64>(endMs) * 1000;

	// 같은 스킬을 다시 썼다 - 예약을 새로 (중복 기절 없음)
	bool replaced = false;
	for (PendingSelfCc& q : _pendingSelfCcs)
	{
		if (q.casterId == p.casterId && q.skillId == p.skillId)
		{
			q = p;
			replaced = true;
			break;
		}
	}
	if (replaced == false)
		_pendingSelfCcs.push_back(p);

	LOG_LINE << "[SELF CC] scheduled char=" << p.casterId << " skill=" << def.skillId << " in=" << endMs << "ms for=" << e.durationMs << "ms" << '\n';
}

void Room::UpdatePendingSelfCcs(uint64 nowUs)
{
	if (_pendingSelfCcs.empty())
		return;

	for (size_t i = 0; i < _pendingSelfCcs.size(); )
	{
		if (nowUs < _pendingSelfCcs[i].atUs)
		{
			i++;
			continue;
		}

		const PendingSelfCc due = _pendingSelfCcs[i];
		_pendingSelfCcs.erase(_pendingSelfCcs.begin() + i);

		auto it = _objects.find(due.casterId);
		CreatureRef caster = (it != _objects.end()) ? AsCreature(it->second) : nullptr;
		if (caster == nullptr || caster->IsAlive() == false)
			continue;

		ApplyCc(due.casterId, due.casterId, due.type, due.durationMs, due.magnitude);
		LOG_LINE << "[SELF CC] at_end char=" << due.casterId << " skill=" << due.skillId << " cc=" << static_cast<int32>(due.type) << " for=" << due.durationMs << "ms" << '\n';
	}
}

// ── 열화 1063 - 지나간 길에만 불타는 땅 (작은 원 여러 개). 적에 막혀 멈추면 거기까지 ──

void Room::StartPathPainter(const CreatureRef& caster, const SkillDef& def, uint32 effectIndex, const SkillContext& ctx, uint64 nowUs)
{
	const SkillEffect& e = def.effects[effectIndex];
	if (e.radiusCm <= 0.f || e.durationMs == 0 || e.distCm <= 0.f)
		return;

	// 칠하는 시간 = 돌진 시간 + 여유. 클라가 움직이고 서버는 C_MOVE 로 받은 자리를 따라간다
	const SkillEffect* dash = def.FindEffect(SkillEffectType::Movement);
	const uint32 dashMs = (dash != nullptr) ? SkillKinematics::TravelMs(dash->distCm, dash->speedCms) : 0;
	const uint32 intervalMs = e.intervalMs > 0 ? e.intervalMs : SkillTable::Rules().dotTickMs;

	PathPainter p;
	p.casterId = ctx.casterId;
	p.skillId = def.skillId;
	p.effectIndex = effectIndex;
	p.untilUs = nowUs + static_cast<uint64>(dashMs + PATH_PAINT_GRACE_MS) * 1000;
	p.lastX = ctx.originX;
	p.lastY = ctx.originY;
	p.stepCm = e.distCm;
	p.groupId = _nextZoneId++;
	p.tickAtUs = nowUs + static_cast<uint64>(intervalMs) * 1000;
	p.ctx = ctx;

	// 출발점부터 - 서 있던 자리도 지나간 길이다
	PlaceZone(caster, def, effectIndex, p.lastX, p.lastY, ctx, nowUs);
	_zones.back().groupId = p.groupId;
	_zones.back().nextTickUs = p.tickAtUs;

	_pathPainters.push_back(p);
	LOG_LINE << "[ZONE] path start char=" << ctx.casterId << " skill=" << def.skillId << " step=" << ::lroundf(e.distCm) << " r=" << ::lroundf(e.radiusCm)
		<< " for=" << (dashMs + PATH_PAINT_GRACE_MS) << "ms" << '\n';
}

void Room::UpdatePathPainters(uint64 nowUs)
{
	if (_pathPainters.empty())
		return;

	for (size_t i = 0; i < _pathPainters.size(); )
	{
		PathPainter& p = _pathPainters[i];

		auto it = _objects.find(p.casterId);
		CreatureRef caster = (it != _objects.end()) ? AsCreature(it->second) : nullptr;
		const SkillDef* def = SkillTable::Find(p.skillId);
		if (caster == nullptr || caster->IsAlive() == false || def == nullptr || p.effectIndex >= def->effects.size())
		{
			_pathPainters.erase(_pathPainters.begin() + i);
			continue;
		}

		const float cx = caster->posInfo->x();
		const float cy = caster->posInfo->y();
		float dx = cx - p.lastX;
		float dy = cy - p.lastY;
		float dist = ::sqrtf(dx * dx + dy * dy);

		if (dist > PATH_JUMP_CM)
		{
			// 보정 · 순간이동으로 튀었다 - 그 사이는 지나간 길이 아니다
			p.lastX = cx;
			p.lastY = cy;
			dist = 0.f;
		}

		int32 placed = 0;
		while (dist >= p.stepCm && dist > 0.f)
		{
			p.lastX += dx / dist * p.stepCm;
			p.lastY += dy / dist * p.stepCm;

			PlaceZone(caster, *def, p.effectIndex, p.lastX, p.lastY, p.ctx, nowUs);
			Zone& z = _zones.back();
			z.groupId = p.groupId;
			z.nextTickUs = p.tickAtUs;
			while (z.nextTickUs <= nowUs)
				z.nextTickUs += static_cast<uint64>(z.intervalMs) * 1000;
			placed++;

			dx = cx - p.lastX;
			dy = cy - p.lastY;
			dist = ::sqrtf(dx * dx + dy * dy);
		}

		if (nowUs >= p.untilUs)
		{
			LOG_LINE << "[ZONE] path end char=" << p.casterId << " skill=" << p.skillId << " at=(" << ::lroundf(cx) << "," << ::lroundf(cy) << ")" << '\n';
			_pathPainters.erase(_pathPainters.begin() + i);
			continue;
		}
		i++;
	}
}

// ── 네불라 아르카나 1069 - 시전자 바로 앞 부채꼴 장판 ──

void Room::PlaceConeZone(const CreatureRef& caster, const SkillDef& def, uint32 effectIndex, const SkillContext& ctx, uint64 nowUs)
{
	float dirX = 1.f;
	float dirY = 0.f;
	DirToward(caster, ctx.originX, ctx.originY, ctx.aimX, ctx.aimY, OUT dirX, OUT dirY);

	PlaceZone(caster, def, effectIndex, ctx.originX, ctx.originY, ctx, nowUs);

	Zone& z = _zones.back();
	z.cone = true;
	z.dirX = dirX;
	z.dirY = dirY;
	z.angleDeg = def.angleDeg;
	z.halfCos = ::cosf(def.angleDeg * 0.5f * PI_F / 180.f);

	Protocol::ZoneInfo& info = _pendingZoneSpawns.back();
	info.set_yaw(::atan2f(dirY, dirX) * 180.f / PI_F);
	info.set_angle_deg(def.angleDeg);

	LOG_LINE << "[ZONE] cone id=" << z.id << " angle=" << ::lroundf(def.angleDeg) << " yaw=" << ::lroundf(info.yaw()) << '\n';
}

// ── 포착 1055 · 약속의 한 발 1057 - 확신의 표식이 남은 적에게 날아간다 ──

string Room::HomingMarkOf(const SkillDef& def) const
{
	// skills.csv tags 의 "homing:<표식>"
	for (const string& tag : def.tags)
	{
		if (tag.rfind("homing:", 0) == 0)
			return tag.substr(7);
	}
	return string();
}

uint64 Room::FindHomingMarkTarget(const CreatureRef& caster, const string& markKind, float fromX, float fromY, float maxRangeCm, uint64 nowUs) const
{
	if (markKind.empty())
		return 0;

	const uint64 casterId = caster->objectInfo->object_id();
	PlayerRef viewer = caster->IsPlayer() ? static_pointer_cast<Player>(caster) : nullptr;

	uint64 best = 0;
	float bestSq = (maxRangeCm > 0.f) ? maxRangeCm * maxRangeCm : FLT_MAX;

	for (auto& item : _objects)
	{
		if (item.first == casterId)
			continue;

		CreatureRef c = AsCreature(item.second);
		if (c == nullptr || c->IsAlive() == false || c->status.HasMark(markKind, casterId) == false)
			continue;
		if (CanProjectileHit(caster, c, nowUs) == false || c->IsStealthed(nowUs))
			continue;

		// 시야에 없으면 그쪽으로 날아가지 않는다 (사용자 결정 2026-10-11)
		if (viewer != nullptr && CanSee(viewer, item.first) == false)
			continue;

		// 여럿이면 투사체 기준 가장 가까운 적 (사용자 결정 2026-10-11)
		const float dx = c->posInfo->x() - fromX;
		const float dy = c->posInfo->y() - fromY;
		const float d2 = dx * dx + dy * dy;
		if (d2 < bestSq)
		{
			bestSq = d2;
			best = item.first;
		}
	}

	return best;
}

// ── 맹수의 탐색 1032 - 둘레의 적 자리에 표시 (주인에게만) ──

void Room::SendDetectionPings(const PlayerRef& owner, const SkillDef& def, float radiusCm, uint32 durationMs, uint64 nowUs)
{
	GameSessionRef session = owner->session.lock();
	if (session == nullptr)
		return;

	const uint64 ownerId = owner->objectInfo->object_id();
	const float ox = owner->posInfo->x();
	const float oy = owner->posInfo->y();

	Protocol::S_ZONE pkt;
	for (auto& item : _objects)
	{
		if (item.first == ownerId || IsEnemyCreature(item.second) == false)
			continue;

		CreatureRef c = static_pointer_cast<Creature>(item.second);
		if (c->IsAlive() == false)
			continue;
		if (c->IsDecoy() && static_pointer_cast<Decoy>(c)->ownerId == ownerId)
			continue;

		const float dx = c->posInfo->x() - ox;
		const float dy = c->posInfo->y() - oy;
		if (dx * dx + dy * dy > radiusCm * radiusCm)
			continue;

		// 끝 패킷은 없다 - 클라가 duration 뒤에 지운다. 그 생물을 알면 따라가고, 모르면 그 자리에
		Protocol::ZoneInfo* info = pkt.add_spawned();
		info->set_zone_id(_nextZoneId++);
		info->set_caster_id(ownerId);
		info->set_effect_id(def.effectId);
		info->set_x(c->posInfo->x());
		info->set_y(c->posInfo->y());
		info->set_radius_cm(PING_RADIUS_CM);
		info->set_duration_ms(durationMs);
		info->set_param("ping");
		info->set_follow_id(item.first);
	}

	if (pkt.spawned_size() > 0)
	{
		SEND_PACKET(pkt);
	}

	LOG_LINE << "[DETECT] ping char=" << ownerId << " enemies=" << pkt.spawned_size() << " r=" << ::lroundf(radiusCm) << '\n';
}

// ── 염두 1090 · 맹수의 본능 1034 - 패시브 쿨다운이 시작됐다고 주인 클라에 (2026-10-11 "염두 쿨다운 적용되는 거 안 보여") ──

void Room::NotifyPassiveTrigger(const CreatureRef& owner, uint32 skillId)
{
	PlayerRef player = AsPlayer(owner);
	if (player == nullptr)
		return;

	GameSessionRef session = player->session.lock();
	const SkillDef* def = SkillTable::Find(skillId);
	if (session == nullptr || def == nullptr)
		return;

	// 쿨다운 = skills.csv cooldown_ms (클라 HUD 와 같은 값). 비어 있으면 효과의 duration (재발동 간격)
	uint32 cooldownMs = def->cooldownMs;
	if (cooldownMs == 0)
	{
		for (const SkillEffect& e : def->effects)
		{
			if (e.type == SkillEffectType::DamageReduceOnce || e.type == SkillEffectType::LethalSave)
				cooldownMs = (std::max)(cooldownMs, e.durationMs);
		}
	}

	NotifyCooldown(player, skillId, cooldownMs);
	LOG_LINE << "[PASSIVE] trigger char=" << player->objectInfo->object_id() << " skill=" << skillId << " cd=" << cooldownMs << "ms" << '\n';
}

void Room::NotifyCooldown(const CreatureRef& owner, uint32 skillId, uint32 remainingMs)
{
	PlayerRef player = AsPlayer(owner);
	GameSessionRef session = (player != nullptr) ? player->session.lock() : nullptr;
	if (session == nullptr)
		return;

	// S_STATUS 쿨다운 - 클라는 그 스킬이 걸린 슬롯의 쿨다운을 지금부터 remaining 으로 맞춘다 (StartCooldownForSkill).
	// 템푸스(2026-10-11)가 남은 쿨다운을 줄일 때도 같은 길 - 클라가 예측한 값을 서버 값으로 덮는다
	const uint64 id = player->objectInfo->object_id();
	Protocol::S_STATUS pkt;
	Protocol::StatusInfo* info = pkt.add_applied();
	info->set_target_id(id);
	info->set_kind(Protocol::STATUS_COOLDOWN);
	info->set_param(to_string(skillId));
	info->set_stacks(1);
	info->set_remaining_ms(remainingMs);
	info->set_instigator_id(id);
	SEND_PACKET(pkt);
}

// ── 건곤일척 1111 - 쓰고 움직이지 않은 채 쓴 망치 스킬 한 번만 피해 증가, 쓰고 나면 사라진다 ──

void Room::OnStillBuffApplied(const CreatureRef& caster, uint32 skillId)
{
	caster->stillSkillId = skillId;
	caster->stillAnchorX = caster->posInfo->x();
	caster->stillAnchorY = caster->posInfo->y();
}

float Room::TakeStillBonus(const CreatureRef& caster, const SkillDef& def, uint64 nowUs)
{
	if (caster->stillSkillId == 0 || def.skillId == caster->stillSkillId)
		return 0.f;

	// 피해를 주는 액티브만 쓴다 - 유지형 · 패시브 · 피해 없는 스킬에 버프를 날리지 않게
	if (def.kind == SkillKind::Hold || def.kind == SkillKind::Passive || def.FindEffect(SkillEffectType::Damage) == nullptr)
		return 0.f;

	const uint64 casterId = caster->objectInfo->object_id();
	const BuffInstance* still = nullptr;
	for (const BuffInstance& b : caster->status.Buffs())
	{
		if (b.skillId == caster->stillSkillId && b.stat == SkillBuffStat::DamageMod && nowUs < b.endUs)
		{
			still = &b;
			break;
		}
	}
	if (still == nullptr)
	{
		caster->stillSkillId = 0;		// 시간이 다 됐다
		return 0.f;
	}

	// 선택자 - "tag:hammer;still" 의 앞부분. 망치 스킬이 아니면 남겨 둔다
	const string selector = still->param.substr(0, still->param.find(';'));
	if (selector.rfind("tag:", 0) != 0 || def.HasTag(selector.substr(4)) == false)
		return 0.f;

	// 제자리인가 - 걸어서든 밀려서든 질주해서든 움직였으면 없다
	const float dx = caster->posInfo->x() - caster->stillAnchorX;
	const float dy = caster->posInfo->y() - caster->stillAnchorY;
	if (dx * dx + dy * dy > STILL_TOLERANCE_CM * STILL_TOLERANCE_CM)
	{
		BreakStillBuff(caster, "moved", nowUs);
		return 0.f;
	}

	const float bonus = still->magnitude;
	const uint32 stillSkillId = caster->stillSkillId;
	const string label = string(SkillTable::BuffStatName(SkillBuffStat::DamageMod)) + ":" + still->param;

	caster->status.RemoveBuff(stillSkillId, SkillBuffStat::DamageMod);
	caster->stillSkillId = 0;
	PushStatus(false, casterId, Protocol::STATUS_BUFF, label, 0, 0, 0.f, casterId);

	LOG_LINE << "[STILL] used char=" << casterId << " skill=" << def.skillId << " bonus=+" << bonus << '\n';
	return bonus;
}

void Room::BreakStillBuff(const CreatureRef& caster, const char* why, uint64 nowUs)
{
	if (caster == nullptr || caster->stillSkillId == 0)
		return;

	const uint64 id = caster->objectInfo->object_id();
	const uint32 skillId = caster->stillSkillId;
	caster->stillSkillId = 0;

	string label;
	bool found = false;
	for (const BuffInstance& b : caster->status.Buffs())
	{
		if (b.skillId == skillId && b.stat == SkillBuffStat::DamageMod && nowUs < b.endUs)
		{
			label = string(SkillTable::BuffStatName(SkillBuffStat::DamageMod)) + ":" + b.param;
			found = true;
			break;
		}
	}
	if (found == false)
		return;

	caster->status.RemoveBuff(skillId, SkillBuffStat::DamageMod);
	PushStatus(false, id, Protocol::STATUS_BUFF, label, 0, 0, 0.f, id);
	LOG_LINE << "[STILL] broken char=" << id << " skill=" << skillId << " why=" << why << '\n';
}

// ── 일보일검 1119 · 난무 1120 - 대상 없는 평타 (주위에 적이 없어도 나간다) ──

bool Room::StartSpecialBasic(const CreatureRef& attacker, float aimX, float aimY, uint64 nowUs)
{
	const float dashCm = attacker->status.BuffMax(SkillBuffStat::BasicDash);
	const float spinCm = attacker->status.BuffMax(SkillBuffStat::BasicSpin);
	if (dashCm <= 0.f && spinCm <= 0.f)
		return false;

	const uint64 attackerId = attacker->objectInfo->object_id();
	float dirX = 1.f;
	float dirY = 0.f;
	DirToward(attacker, attacker->posInfo->x(), attacker->posInfo->y(), aimX, aimY, OUT dirX, OUT dirY);

	// 공격하면 무적 · 은신이 풀린다 (보통 평타와 같다)
	if (attacker->IsPlayer())
		ClearSpawnInvulnerability(static_pointer_cast<Player>(attacker), "attack");
	BreakStealth(attacker, "attack", nowUs);

	// 조준 쪽을 본다 - 다른 사람 화면에서도 그쪽을 벤다
	attacker->posInfo->set_yaw(::atan2f(dirY, dirX) * 180.f / PI_F);
	_dirtyMovers.insert(attackerId);

	uint32 windupMs = 0;
	if (spinCm > 0.f)
	{
		// 난무 - 사방 베기는 즉발 (캐스트 없음, 사용자 2026-10-11). 다음 전투 Flush 에 판정한다
		attacker->attackMode = 2;
	}
	else
	{
		// 일보일검 - 조준 쪽으로 질주(서버가 옮기고 클라는 KnockbackInfo 로 같이 미끄러진다)한 뒤 60° 횡베기
		const float speed = SkillTable::Rules().basicDashSpeedCms;
		float moved = 0.f;
		PushCreature(attacker, attackerId, dirX, dirY, dashCm, speed, /*wallStun*/ false, nowUs, &moved);
		windupMs = (std::max)(attacker->GetAttackWindupMs(), SkillKinematics::TravelMs(moved, speed) + DASH_SLASH_LAG_MS);
		attacker->attackMode = 1;
	}

	attacker->attackDirX = dirX;
	attacker->attackDirY = dirY;
	attacker->attackTargetId = 0;
	attacker->attackHitAtUs = nowUs + static_cast<uint64>(windupMs) * 1000;
	_attackers.insert(attackerId);

	Protocol::AttackInfo& info = _pendingAttacks.emplace_back();
	info.set_attacker_id(attackerId);
	info.set_target_id(0);
	info.set_windup_ms(windupMs);
	info.set_interval_ms(attacker->GetAttackIntervalMs());

	LOG_LINE << "[SPECIAL BASIC] start char=" << attackerId << " mode=" << (spinCm > 0.f ? "spin" : "dash_slash")
		<< " windup=" << windupMs << "ms" << '\n';
	return true;
}

void Room::ResolveSpecialBasic(const CreatureRef& attacker, uint64 nowUs)
{
	const uint8 mode = attacker->attackMode;
	attacker->attackMode = 0;

	const uint64 attackerId = attacker->objectInfo->object_id();
	const float cx = attacker->posInfo->x();
	const float cy = attacker->posInfo->y();

	// 난무 = 버프 크기 반경의 사방, 일보일검 = 원래 평타 사거리의 60° 부채꼴 (사용자 2026-10-11 "일보일검 이전 사거리만큼")
	const BuffInstance* buff = attacker->status.FindBuff(mode == 2 ? SkillBuffStat::BasicSpin : SkillBuffStat::BasicDash);
	const float radius = (mode == 2) ? attacker->status.BuffMax(SkillBuffStat::BasicSpin) : attacker->attackRange;
	const float halfCos = (mode == 2) ? -2.f : ::cosf(DASH_SLASH_ANGLE_DEG * 0.5f * PI_F / 180.f);
	const float dirX = attacker->attackDirX;
	const float dirY = attacker->attackDirY;

	int32 hits = 0;
	if (radius > 0.f)
	{
		vector<CreatureRef> victims;
		for (auto& item : _objects)
		{
			if (item.first == attackerId || IsEnemyCreature(item.second) == false)
				continue;

			CreatureRef c = static_pointer_cast<Creature>(item.second);
			if (CanProjectileHit(attacker, c, nowUs) == false)
				continue;
			if (c->IsDecoy() && static_pointer_cast<Decoy>(c)->ownerId == attackerId)
				continue;

			const float dx = c->posInfo->x() - cx;
			const float dy = c->posInfo->y() - cy;
			const float d2 = dx * dx + dy * dy;
			if (d2 > radius * radius)
				continue;

			const float d = ::sqrtf(d2);
			if (d > 1.f && (dx * dirX + dy * dirY) / d < halfCos)
				continue;

			if (HasLineOfAttack(attacker, cx, cy, c) == false)
				continue;

			victims.push_back(c);
		}

		for (const CreatureRef& target : victims)
		{
			if (target->IsAlive() == false)
				continue;

			// 반격 · 받아넘기기 - 보통 평타와 같다
			if (TryCounter(target, attacker, attackerId, nowUs) || TryParry(target, cx, cy, /*projectile*/ false, nowUs))
				continue;

			bool isCrit = false;
			int32 damage = attacker->ComputeDamageTo(*target, attacker->autoAttackType, OUT isCrit);
			if (target->status.HasMark("spear", attackerId))
				damage = static_cast<int32>(damage * (1.f + SkillTable::Rules().markSpearBasicBonus));

			DamageSource src;
			src.basic = true;
			const int32 hpBefore = target->hp;
			const int32 remainingHp = DealDamage(attacker, attackerId, target, damage, attacker->autoAttackType, isCrit, nowUs, src);
			hits++;

			TryThorns(attacker, target, hpBefore - (std::max)(0, remainingHp), nowUs);
			if (remainingHp > 0 && attacker->IsPlayer())
				ApplyOnHitPassives(attacker, target, nowUs);

			const uint64 targetId = target->objectInfo->object_id();
			if (remainingHp > 0 && nowUs < attacker->petrifyEndUs
				&& find(attacker->petrifyTargets.begin(), attacker->petrifyTargets.end(), targetId) == attacker->petrifyTargets.end())
				attacker->petrifyTargets.push_back(targetId);
		}
	}

	// 후딜 - 난무는 시전이 없으니 간격 전체, 일보일검은 보통 평타처럼 남은 몫
	const uint32 recoverMs = (mode == 2) ? attacker->GetAttackIntervalMs() : attacker->GetAttackRecoveryMs();
	attacker->attackReadyAtUs = nowUs + static_cast<uint64>(recoverMs) * 1000;

	// 연출 - 사방 = 원, 횡베기 = 부채꼴
	Protocol::ZoneInfo& info = _pendingZoneSpawns.emplace_back();
	info.set_zone_id(_nextZoneId++);
	info.set_caster_id(attackerId);
	info.set_effect_id(buff != nullptr ? buff->skillId : 0);
	info.set_x(cx);
	info.set_y(cy);
	info.set_radius_cm(radius);
	info.set_duration_ms(SPECIAL_BASIC_VISUAL_MS);
	info.set_param(mode == 2 ? "spin" : "slash");
	if (mode == 1)
	{
		info.set_yaw(::atan2f(dirY, dirX) * 180.f / PI_F);
		info.set_angle_deg(DASH_SLASH_ANGLE_DEG);
	}

	LOG_LINE << "[SPECIAL BASIC] hit char=" << attackerId << " mode=" << (mode == 2 ? "spin" : "dash_slash") << " r=" << ::lroundf(radius)
		<< " hits=" << hits << '\n';
}

// ── 스킬도 벽에 막힌다 (2026-10-11 "유령 신부의 축복을 안 써도 벽 너머 적에게 공격이 가능해") ──

bool Room::HasLineOfAttack(const CreatureRef& caster, float fromX, float fromY, const CreatureRef& target) const
{
	if (target == nullptr)
		return false;
	if (IgnoresWalls(caster))
		return true;
	return Walls().SweepBlocked(fromX, fromY, target->posInfo->x(), target->posInfo->y(), 0.f) == false;
}

// ── 저 멀리 1109 - 날아가던 적이 벽에 부딪히면 기절 + 날아간 거리에 비례한 피해 ──

void Room::ScheduleSlam(const CreatureRef& caster, const CreatureRef& target, const SkillDef& def, const SkillEffect& e, float flownCm, uint32 travelMs, const SkillContext& ctx, uint64 nowUs)
{
	PendingSlam s;
	s.casterId = ctx.casterId;
	s.targetId = target->objectInfo->object_id();
	s.skillId = def.skillId;
	s.atUs = nowUs + static_cast<uint64>(travelMs) * 1000;
	s.flownCm = flownCm;
	s.adPer100Cm = e.adRatio;
	s.bonusDamageMod = ctx.bonusDamageMod;
	s.x = target->posInfo->x();		// PushCreature 가 이미 끝점으로 옮겼다 = 부딪힌 자리
	s.y = target->posInfo->y();
	_pendingSlams.push_back(s);

	LOG_LINE << "[SLAM] planned target=" << s.targetId << " by=" << s.casterId << " flown=" << ::lroundf(flownCm) << " in=" << travelMs << "ms" << '\n';
}

void Room::UpdateSlams(uint64 nowUs)
{
	if (_pendingSlams.empty())
		return;

	for (size_t i = 0; i < _pendingSlams.size(); )
	{
		if (nowUs < _pendingSlams[i].atUs)
		{
			i++;
			continue;
		}

		const PendingSlam s = _pendingSlams[i];
		_pendingSlams.erase(_pendingSlams.begin() + i);

		auto targetIt = _objects.find(s.targetId);
		CreatureRef target = (targetIt != _objects.end()) ? AsCreature(targetIt->second) : nullptr;
		if (target == nullptr || target->IsAlive() == false)
			continue;

		auto casterIt = _objects.find(s.casterId);
		CreatureRef caster = (casterIt != _objects.end()) ? AsCreature(casterIt->second) : nullptr;

		// 벽 기절 (skill_rules wall_stun_ms, 저항이 깎는다)
		ApplyCc(s.targetId, s.casterId, Protocol::CC_TYPE_STUN, SkillTable::Rules().wallStunMs, 0.f);

		int32 damage = 0;
		const float ratio = (std::min)(SLAM_RATIO_CAP, s.adPer100Cm * s.flownCm / 100.f);
		if (caster != nullptr && ratio > 0.f)
		{
			bool isCrit = false;
			damage = caster->ComputeSkillDamageTo(*target, ratio, 0.f, OUT isCrit);
			DamageSource src;
			src.skillId = s.skillId;
			src.extraMult = s.bonusDamageMod;
			DealDamage(caster, s.casterId, target, damage, Protocol::DAMAGE_TYPE_PHYSICAL, isCrit, nowUs, src);
		}

		if (const SkillDef* def = SkillTable::Find(s.skillId))
		{
			Protocol::SkillHitInfo& hit = _pendingSkillHits.emplace_back();
			hit.set_caster_id(s.casterId);
			hit.set_effect_id(def->effectId);
			hit.set_target_id(s.targetId);
			hit.set_impact_x(s.x);
			hit.set_impact_y(s.y);
			hit.set_aim_x(s.x);
			hit.set_aim_y(s.y);
		}

		LOG_LINE << "[SLAM] wall target=" << s.targetId << " by=" << s.casterId << " flown=" << ::lroundf(s.flownCm) << " ratio=" << ratio << " dmg=" << damage << '\n';
	}
}

void Room::PushTargetVisual(uint64 casterId, uint32 effectId, const CreatureRef& target)
{
	if (target == nullptr)
		return;

	Protocol::SkillHitInfo& hit = _pendingSkillHits.emplace_back();
	hit.set_caster_id(casterId);
	hit.set_effect_id(effectId);
	hit.set_target_id(target->objectInfo->object_id());
	hit.set_impact_x(target->posInfo->x());
	hit.set_impact_y(target->posInfo->y());
	hit.set_aim_x(target->posInfo->x());
	hit.set_aim_y(target->posInfo->y());
}
