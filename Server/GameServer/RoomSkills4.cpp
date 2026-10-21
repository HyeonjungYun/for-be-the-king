#include "pch.h"
#include "Room.h"
#include "Player.h"
#include "Monster.h"
#include "Decoy.h"
#include "SkillTable.h"
#include "SkillKinematics.h"
#include "ItemTable.h"
#include "GameSession.h"

// 3차 로스터 (게임 ID 1138 ~ 1189, 2026-10-11 사용자 "스킬 목록에 추가된 스킬과 바뀐 스킬에 필요한 기능을 전부 구현 · YOLO").
// 결정표: design/quick-specs/skill-roster-3-2026-10-11.md. 수치는 전부 Server/Data/skills.csv · skill_effects.csv.

namespace
{
	constexpr float		ANCHOR_MARK_RADIUS_CM = 60.f;		// 포르타 문 · 비창 자리 표시
	constexpr float		NEAR_WALL_CM = 110.f;				// 벽 차기 - 중심에서 이 안에 벽면이 있으면 "붙어 있다" (캡슐 42 + 여유)
	constexpr int32		WALL_PROBE_DIRS = 16;				// 벽 방향을 찾는 살
	constexpr uint64	FOOTPRINT_SPACING_US = 500'000;	// 발자국 - 0.5 s 마다 하나
	constexpr float		FOOTPRINT_RADIUS_CM = 18.f;
	constexpr float		FOOTPRINT_MIN_STEP_CM = 40.f;		// 서 있던 자리는 한 번만 (같은 자리 발자국을 겹쳐 찍지 않는다)
	constexpr float		FRONT_GUARD_COS = 0.5f;			// 철벽진 - 앞 120° (반각 60°)
	constexpr uint32	LANDING_GRACE_MS = 80;				// 착지 충격 - 이동 시간 뒤 늦게 오는 C_MOVE 를 기다린다
	constexpr uint32	LANDING_VISUAL_MS = 400;

	// PI_F · ARRIVE_STOP_CM · IsEnemyCreature 는 Room.h 공용, 보정 S_MOVE 는 Room::SendCorrection (TD-011, 2026-10-21)
}

// ── 비창 1138 · 포르타 1150 - 재입력으로 돌아갈 자리 ──

void Room::SetAnchor(const CreatureRef& caster, const SkillDef& def, const SkillEffect& e, const SkillContext& ctx, uint64 nowUs)
{
	// 한 사람에 한 자리 - 예전 표시는 지운다
	if (caster->anchorZoneId != 0)
		_pendingZoneEnds.push_back(caster->anchorZoneId);

	caster->anchorSkillId = def.skillId;
	caster->anchorUntilUs = nowUs + static_cast<uint64>(e.durationMs) * 1000;
	caster->anchorZoneId = 0;
	caster->anchorSet = false;

	// projectile - 던진 창이 멈출 때 OnAnchorProjectileEnd 가 놓는다
	if (e.param != "point")
		return;

	// 포르타 - 조준 지점. 사거리 밖을 찍었으면 사거리 끝 (판정 · 장판과 같은 규칙). 벽 안이어도 된다 - 순간이동 때 빈 자리로 보정
	float x = ctx.originX;
	float y = ctx.originY;
	const float dx = ctx.aimX - ctx.originX;
	const float dy = ctx.aimY - ctx.originY;
	const float len = ::sqrtf(dx * dx + dy * dy);
	if (len > 1.f)
	{
		const float reach = (std::min)(len, def.rangeCm);
		x += dx / len * reach;
		y += dy / len * reach;
	}

	caster->anchorX = x;
	caster->anchorY = y;
	caster->anchorSet = true;
	caster->anchorZoneId = _nextZoneId;
	PushVisualZone(ctx.casterId, def.effectId, x, y, ANCHOR_MARK_RADIUS_CM, e.durationMs, "portal");

	LOG_LINE << "[ANCHOR] char=" << ctx.casterId << " skill=" << def.skillId << " at=(" << ::lroundf(x) << "," << ::lroundf(y) << ") for=" << e.durationMs << "ms" << '\n';
}

void Room::OnAnchorProjectileEnd(const Projectile& p)
{
	if (p.skillId == 0)
		return;

	const SkillDef* def = SkillTable::Find(p.skillId);
	const SkillEffect* anchor = (def != nullptr) ? def->FindEffect(SkillEffectType::Anchor) : nullptr;
	if (anchor == nullptr || anchor->param != "projectile")
		return;

	CreatureRef caster = FindCreature(p.casterId);
	const uint64 nowUs = Utils::NowMicroseconds();
	if (caster == nullptr || caster->anchorSkillId != p.skillId || nowUs >= caster->anchorUntilUs)
		return;

	caster->anchorX = p.x;
	caster->anchorY = p.y;
	caster->anchorSet = true;
	caster->anchorZoneId = _nextZoneId;
	const uint32 leftMs = static_cast<uint32>((caster->anchorUntilUs - nowUs) / 1000);
	PushVisualZone(p.casterId, def->effectId, p.x, p.y, ANCHOR_MARK_RADIUS_CM, leftMs, "spear");

	LOG_LINE << "[ANCHOR] char=" << p.casterId << " skill=" << p.skillId << " spear at=(" << ::lroundf(p.x) << "," << ::lroundf(p.y) << ") for=" << leftMs << "ms" << '\n';
}

void Room::TeleportToAnchor(const CreatureRef& caster, const SkillDef& def, const SkillEffect& e, uint64 nowUs)
{
	PlayerRef player = AsPlayer(caster);
	if (player == nullptr)
		return;

	const uint64 id = player->objectInfo->object_id();
	if (caster->anchorSkillId != def.skillId || caster->anchorSet == false || nowUs >= caster->anchorUntilUs)
	{
		LOG_LINE << "[ANCHOR] none char=" << id << " skill=" << def.skillId << (caster->anchorSet ? " expired" : " not landed") << '\n';
		return;
	}

	const float dx = caster->anchorX - player->posInfo->x();
	const float dy = caster->anchorY - player->posInfo->y();
	if (dx * dx + dy * dy > e.distCm * e.distCm)
	{
		LOG_LINE << "[ANCHOR] too far char=" << id << " dist=" << ::lroundf(::sqrtf(dx * dx + dy * dy)) << " max=" << e.distCm << '\n';
		return;
	}

	// 벽 · 경계 보정 - 가장 가까운 빈 자리 (블링크와 같은 규칙). 포르타는 벽 너머로도 (자리만 보고 길은 보지 않는다)
	float outX = caster->anchorX;
	float outY = caster->anchorY;
	if (Walls().FindNearestFree(caster->anchorX, caster->anchorY, SkillKinematics::CAPSULE_RADIUS_CM, 200.f, OUT outX, OUT outY) == false)
	{
		LOG_LINE << "[ANCHOR] no free spot char=" << id << '\n';
		return;
	}

	player->posInfo->set_x(outX);
	player->posInfo->set_y(outY);
	_dirtyMovers.insert(id);
	SendCorrection(player);

	if (caster->anchorZoneId != 0)
		_pendingZoneEnds.push_back(caster->anchorZoneId);
	caster->anchorSkillId = 0;
	caster->anchorSet = false;
	caster->anchorZoneId = 0;
	caster->anchorUntilUs = 0;

	LOG_LINE << "[ANCHOR] teleport char=" << id << " skill=" << def.skillId << " to=(" << ::lroundf(outX) << "," << ::lroundf(outY) << ")" << '\n';
}

// ── 회귀 1167 - 3 초 전에 서 있던 자리로 ──

void Room::Rewind(const CreatureRef& caster, const SkillEffect& e, uint64 nowUs)
{
	PlayerRef player = AsPlayer(caster);
	if (player == nullptr)
		return;

	const uint64 backUs = static_cast<uint64>(e.durationMs) * 1000;
	float x = 0.f;
	float y = 0.f;
	if (caster->history.At(nowUs > backUs ? nowUs - backUs : 0, OUT x, OUT y) == false)
		return;

	const uint64 id = player->objectInfo->object_id();
	const float dx = x - player->posInfo->x();
	const float dy = y - player->posInfo->y();
	if (dx * dx + dy * dy > e.distCm * e.distCm)
	{
		LOG_LINE << "[REWIND] too far char=" << id << " dist=" << ::lroundf(::sqrtf(dx * dx + dy * dy)) << '\n';
		return;
	}

	// 그 사이 그 자리에 벽이 솟았을 수 있다 (지각변동) - 가장 가까운 빈 자리
	float outX = x;
	float outY = y;
	if (Walls().FindNearestFree(x, y, SkillKinematics::CAPSULE_RADIUS_CM, 200.f, OUT outX, OUT outY) == false)
		return;

	player->posInfo->set_x(outX);
	player->posInfo->set_y(outY);
	_dirtyMovers.insert(id);
	SendCorrection(player);

	LOG_LINE << "[REWIND] char=" << id << " back=" << e.durationMs << "ms to=(" << ::lroundf(outX) << "," << ::lroundf(outY) << ") moved=" << ::lroundf(::sqrtf(dx * dx + dy * dy)) << '\n';
}

// ── 벽 차기 1163 - 벽에 붙어 있을 때만, 벽을 박차고 반대쪽으로 ──

bool Room::IsNearWall(const CreatureRef& creature) const
{
	return Walls().Overlaps(creature->posInfo->x(), creature->posInfo->y(), NEAR_WALL_CM);
}

void Room::WallKick(const CreatureRef& caster, const SkillEffect& e, uint64 nowUs)
{
	const uint64 id = caster->objectInfo->object_id();
	const float x = caster->posInfo->x();
	const float y = caster->posInfo->y();
	const WallMap& walls = Walls();

	// 벽이 있는 쪽 = 살이 벽에 닿는 방향들의 평균. 뛰는 쪽은 그 반대
	float sx = 0.f;
	float sy = 0.f;
	int32 hits = 0;
	for (int32 k = 0; k < WALL_PROBE_DIRS; k++)
	{
		const float a = 2.f * PI_F * static_cast<float>(k) / WALL_PROBE_DIRS;
		const float cx = ::cosf(a);
		const float cy = ::sinf(a);
		if (walls.SweepBlocked(x, y, x + cx * NEAR_WALL_CM, y + cy * NEAR_WALL_CM, 0.f))
		{
			sx += cx;
			sy += cy;
			hits++;
		}
	}

	float dirX = -sx;
	float dirY = -sy;
	const float len = ::sqrtf(dirX * dirX + dirY * dirY);
	if (hits == 0 || len < 0.01f)
	{
		// 사방이 막혔거나(구석) 벽이 사라졌다 - 바라보는 반대쪽
		const float yawRad = caster->posInfo->yaw() * PI_F / 180.f;
		dirX = -::cosf(yawRad);
		dirY = -::sinf(yawRad);
	}
	else
	{
		dirX /= len;
		dirY /= len;
	}

	const float dist = e.distCm * MoveDistScale(caster);
	float moved = 0.f;
	PushCreature(caster, id, dirX, dirY, dist, e.speedCms, /*wallStun*/ false, nowUs, &moved);
	ScheduleLanding(caster, SkillKinematics::TravelMs(moved, e.speedCms), nowUs);

	LOG_LINE << "[WALL KICK] char=" << id << " probes=" << hits << " dir=(" << dirX << "," << dirY << ") moved=" << ::lroundf(moved) << '\n';
}

// ── 템푸스 1152 - 스태프 스킬의 남은 쿨다운을 크게 줄인다 ──

void Room::CutCooldowns(const CreatureRef& caster, const SkillDef& def, const SkillEffect& e, uint64 nowUs)
{
	PlayerRef player = AsPlayer(caster);
	if (player == nullptr)
		return;

	// "스태프 스킬" = 스태프를 들고 있을 때의 무기 두 칸 + 스태프 조건이 붙은 갑옷 · 모자 스킬. 템푸스 자신은 빼다
	const ItemEntry& weapon = player->equipped[Protocol::SLOT_WEAPON_PRIMARY - 1];
	const ItemTypeDef* weaponType = (weapon.instanceId != 0) ? ItemTable::Find(weapon.itemTypeId) : nullptr;
	const bool holdingFamily = (weaponType != nullptr && weaponType->skillPoolId == e.param);
	const float keep = 1.f - e.magnitude;

	int32 cut = 0;
	for (int32 i = 0; i < SKILL_SLOT_COUNT; i++)
	{
		SkillSlot& slot = player->skillSlots[i];
		if (slot.bindState != Protocol::BIND_BOUND || slot.skillId == 0 || slot.skillId == def.skillId || slot.cooldownEndUs <= nowUs)
			continue;

		const SkillDef* s = SkillTable::Find(slot.skillId);
		if (s == nullptr)
			continue;

		const bool weaponSlot = (i == Protocol::SLOT_WEAPON_PRIMARY - 1 || i == Protocol::SLOT_WEAPON_SECONDARY - 1);
		if ((weaponSlot && holdingFamily) == false && s->requiresWeapon != e.param)
			continue;

		const uint64 left = slot.cooldownEndUs - nowUs;
		const uint64 newLeft = static_cast<uint64>(static_cast<double>(left) * keep);
		slot.cooldownEndUs = nowUs + newLeft;
		NotifyCooldown(player, slot.skillId, static_cast<uint32>(newLeft / 1000));
		cut++;

		LOG_LINE << "[TEMPUS] char=" << player->objectInfo->object_id() << " skill=" << slot.skillId << " " << (left / 1000) << " -> " << (newLeft / 1000) << "ms" << '\n';
	}

	if (cut == 0)
		LOG_LINE << "[TEMPUS] char=" << player->objectInfo->object_id() << " nothing on cooldown" << '\n';
}

// ── 발자국 추적 1187 - 둘레 적들이 지나간 자리 (주인에게만) ──

void Room::SendFootprints(const CreatureRef& caster, const SkillDef& def, const SkillEffect& e, uint64 nowUs)
{
	PlayerRef owner = AsPlayer(caster);
	GameSessionRef session = (owner != nullptr) ? owner->session.lock() : nullptr;
	if (session == nullptr)
		return;

	const uint64 ownerId = owner->objectInfo->object_id();
	const float ox = owner->posInfo->x();
	const float oy = owner->posInfo->y();
	const uint64 sinceUs = (nowUs > static_cast<uint64>(e.intervalMs) * 1000) ? nowUs - static_cast<uint64>(e.intervalMs) * 1000 : 0;
	const float r2 = e.radiusCm * e.radiusCm;

	// 보이지 않는 적의 발자국도 보인다 - 그것이 이 스킬의 뜻이다 (은신 중인 적도 땅에 발자국은 남는다)
	Protocol::S_ZONE pkt;
	int32 enemies = 0;
	for (auto& item : _objects)
	{
		if (item.first == ownerId || IsEnemyCreature(item.second) == false || item.second->IsDecoy())
			continue;

		CreatureRef c = static_pointer_cast<Creature>(item.second);
		if (c->IsAlive() == false)
			continue;

		int32 placed = 0;
		float lastX = 0.f;
		float lastY = 0.f;
		c->history.ForEachSince(sinceUs, FOOTPRINT_SPACING_US, [&](const PositionHistory::Sample& s)
			{
				const float dx = s.x - ox;
				const float dy = s.y - oy;
				if (dx * dx + dy * dy > r2)
					return;
				if (placed > 0 && (s.x - lastX) * (s.x - lastX) + (s.y - lastY) * (s.y - lastY) < FOOTPRINT_MIN_STEP_CM * FOOTPRINT_MIN_STEP_CM)
					return;

				Protocol::ZoneInfo* info = pkt.add_spawned();
				info->set_zone_id(_nextZoneId++);
				info->set_caster_id(ownerId);
				info->set_effect_id(def.effectId);
				info->set_x(s.x);
				info->set_y(s.y);
				info->set_radius_cm(FOOTPRINT_RADIUS_CM);
				info->set_duration_ms(e.durationMs);
				info->set_param("footprint");
				// 걸어간 방향 - 다음 발자국 쪽 (첫 것은 0)
				if (placed > 0)
					info->set_yaw(::atan2f(s.y - lastY, s.x - lastX) * 180.f / PI_F);
				lastX = s.x;
				lastY = s.y;
				placed++;
			});

		if (placed > 0)
			enemies++;
	}

	if (pkt.spawned_size() > 0)
	{
		SEND_PACKET(pkt);
	}

	LOG_LINE << "[FOOTPRINTS] char=" << ownerId << " enemies=" << enemies << " prints=" << pkt.spawned_size() << " r=" << ::lroundf(e.radiusCm) << '\n';
}

// ── 추격 1165 - 도착하면 그 적이 느려진다 ──

void Room::ScheduleCcOnArrive(const CreatureRef& caster, const SkillDef& def, const SkillEffect& e, const CreatureRef& target, uint64 nowUs)
{
	// 질주 시간 = 대상 바로 앞까지의 거리 / 속도 (클라 to_target 과 같은 계산). 이동 효과가 없으면 지금
	uint32 travelMs = 0;
	if (const SkillEffect* move = def.FindEffect(SkillEffectType::Movement); move != nullptr && move->speedCms > 0.f)
	{
		const float dx = target->posInfo->x() - caster->posInfo->x();
		const float dy = target->posInfo->y() - caster->posInfo->y();
		const float dist = (std::clamp)(::sqrtf(dx * dx + dy * dy) - ARRIVE_STOP_CM, 0.f, move->distCm * MoveDistScale(caster));
		travelMs = SkillKinematics::TravelMs(dist, move->speedCms);
	}

	PendingCc p;
	p.targetId = target->objectInfo->object_id();
	p.instigatorId = caster->objectInfo->object_id();
	p.type = e.ccType;
	p.durationMs = e.durationMs;
	p.magnitude = e.magnitude;
	p.atUs = nowUs + static_cast<uint64>(travelMs) * 1000;
	_pendingCcs.push_back(p);

	LOG_LINE << "[ARRIVE] char=" << p.instigatorId << " -> " << p.targetId << " cc=" << static_cast<int32>(e.ccType) << " in=" << travelMs << "ms" << '\n';
}

void Room::UpdatePendingCcs(uint64 nowUs)
{
	for (size_t i = 0; i < _pendingCcs.size(); )
	{
		if (nowUs < _pendingCcs[i].atUs)
		{
			i++;
			continue;
		}

		const PendingCc due = _pendingCcs[i];
		_pendingCcs.erase(_pendingCcs.begin() + i);
		ApplyCc(due.targetId, due.instigatorId, due.type, due.durationMs, due.magnitude);
	}
}

// ── 착지 충격 1182 - 질주 · 도약이 끝나는 자리 둘레 적이 느려진다 ──

void Room::ScheduleLanding(const CreatureRef& caster, uint32 travelMs, uint64 nowUs)
{
	if (caster == nullptr || caster->IsPlayer() == false)
		return;

	// 착지 패시브가 있을 때만 - 없는 사람의 이동마다 예약을 쌓지 않는다
	bool wants = false;
	ForEachBoundPassive(*caster, [&](int32, SkillSlot&, const SkillDef& def)
		{
			for (const SkillEffect& e : def.effects)
				wants |= (e.condition == SkillRequire::OnMoveEnd);
		});
	if (wants == false)
		return;

	PendingLanding p;
	p.casterId = caster->objectInfo->object_id();
	p.atUs = nowUs + static_cast<uint64>(travelMs + LANDING_GRACE_MS) * 1000;
	_pendingLandings.push_back(p);
}

void Room::UpdateLandings(uint64 nowUs)
{
	for (size_t i = 0; i < _pendingLandings.size(); )
	{
		if (nowUs < _pendingLandings[i].atUs)
		{
			i++;
			continue;
		}

		const PendingLanding due = _pendingLandings[i];
		_pendingLandings.erase(_pendingLandings.begin() + i);

		CreatureRef owner = FindCreature(due.casterId);
		if (owner == nullptr || owner->IsAlive() == false)
			continue;

		const float cx = owner->posInfo->x();
		const float cy = owner->posInfo->y();

		for (int32 s = 0; s < SKILL_SLOT_COUNT; s++)
		{
			const SkillDef* def = BoundPassiveOf(owner->skillSlots[s]);
			if (def == nullptr || MeetsWeaponRequirement(owner, *def) == false)
				continue;

			for (uint32 k = 0; k < def->effects.size(); k++)
			{
				const SkillEffect& e = def->effects[k];
				if (e.condition != SkillRequire::OnMoveEnd || e.self || e.radiusCm <= 0.f)
					continue;

				int32 hit = 0;
				for (auto& item : _objects)
				{
					if (item.first == due.casterId || IsEnemyCreature(item.second) == false)
						continue;
					CreatureRef c = static_pointer_cast<Creature>(item.second);
					if (c->IsAlive() == false || CanProjectileHit(owner, c, nowUs) == false)
						continue;
					const float dx = c->posInfo->x() - cx;
					const float dy = c->posInfo->y() - cy;
					if (dx * dx + dy * dy > e.radiusCm * e.radiusCm)
						continue;
					ApplyPassiveEffect(owner, *def, k, c, static_cast<Protocol::EquipSlot>(s + 1), 1.f, nowUs);
					hit++;
				}

				PushVisualZone(due.casterId, def->effectId, cx, cy, e.radiusCm, LANDING_VISUAL_MS, "shock");
				LOG_LINE << "[LANDING] char=" << due.casterId << " skill=" << def->skillId << " slowed=" << hit << " at=(" << ::lroundf(cx) << "," << ::lroundf(cy) << ")" << '\n';
			}
		}
	}
}

float Room::MoveDistScale(const CreatureRef& creature) const
{
	if (creature == nullptr)
		return 1.f;
	return (std::max)(0.f, 1.f + creature->PassiveStatSum(SkillBuffStat::MoveDist));
}

// ── 반격의 걸음 1180 (on_damaged) · 사냥꾼의 걸음 1181 (on_kill) - 자기 효과 ──

void Room::TriggerSelfPassives(const CreatureRef& owner, SkillRequire condition, uint64 nowUs)
{
	if (owner == nullptr || owner->IsAlive() == false || owner->IsPlayer() == false)
		return;

	for (int32 s = 0; s < SKILL_SLOT_COUNT; s++)
	{
		const SkillDef* def = BoundPassiveOf(owner->skillSlots[s]);
		if (def == nullptr || MeetsWeaponRequirement(owner, *def) == false)
			continue;

		for (uint32 k = 0; k < def->effects.size(); k++)
		{
			const SkillEffect& e = def->effects[k];
			if (e.condition == condition && e.self)
				ApplyPassiveEffect(owner, *def, k, nullptr, static_cast<Protocol::EquipSlot>(s + 1), 1.f, nowUs);
		}
	}
}

// ── 플럭스 1151 - 속성이 바뀔 때마다 둘레 적에게 그 속성 피해 ──

void Room::TriggerStancePassives(const CreatureRef& owner, const string& element, uint64 nowUs)
{
	if (owner == nullptr || owner->IsPlayer() == false || element.empty())
		return;

	const uint64 ownerId = owner->objectInfo->object_id();
	const float cx = owner->posInfo->x();
	const float cy = owner->posInfo->y();

	for (int32 s = 0; s < SKILL_SLOT_COUNT; s++)
	{
		const SkillDef* def = BoundPassiveOf(owner->skillSlots[s]);
		if (def == nullptr || MeetsWeaponRequirement(owner, *def) == false)
			continue;

		for (uint32 k = 0; k < def->effects.size(); k++)
		{
			const SkillEffect& e = def->effects[k];
			if (e.condition != SkillRequire::OnStance || e.self || e.radiusCm <= 0.f)
				continue;

			int32 hit = 0;
			// 후보를 먼저 모은다 - 효과의 처치가 _objects 를 바꾼다 (2026-10-17 리뷰)
			for (const CreatureRef& c : SnapshotCreatures(ownerId))
			{
				if (c->IsAlive() == false || CanProjectileHit(owner, c, nowUs) == false)
					continue;
				const float dx = c->posInfo->x() - cx;
				const float dy = c->posInfo->y() - cy;
				if (dx * dx + dy * dy > e.radiusCm * e.radiusCm || HasLineOfAttack(owner, cx, cy, c) == false)
					continue;
				// 피해 param "stance" - 지금 속성(막 바뀐 것)이 피해 속성 (ApplyTargetEffect)
				ApplyPassiveEffect(owner, *def, k, c, static_cast<Protocol::EquipSlot>(s + 1), 1.f, nowUs);
				hit++;
			}

			PushVisualZone(ownerId, def->effectId, cx, cy, e.radiusCm, LANDING_VISUAL_MS, element.c_str());
			LOG_LINE << "[FLUX] char=" << ownerId << " " << element << " hit=" << hit << '\n';
		}
	}
}

// ── 줄행랑 1183 - 체력이 일정 아래로 떨어지면 저절로 뒤로 질주 + 이속 ──

void Room::TryLowHpEscape(const CreatureRef& victim, const CreatureRef& attacker, uint64 nowUs)
{
	if (victim == nullptr || victim->IsAlive() == false || victim->IsPlayer() == false || victim->maxHp <= 0)
		return;

	PlayerRef player = static_pointer_cast<Player>(victim);
	for (int32 s = 0; s < SKILL_SLOT_COUNT; s++)
	{
		SkillSlot& slot = player->skillSlots[s];
		const SkillDef* def = BoundPassiveOf(slot);
		if (def == nullptr || nowUs < slot.cooldownEndUs)
			continue;

		const SkillEffect* move = nullptr;
		for (const SkillEffect& e : def->effects)
		{
			if (e.condition == SkillRequire::LowHp && e.type == SkillEffectType::Movement)
				move = &e;
		}
		if (move == nullptr || static_cast<float>(victim->hp) > static_cast<float>(victim->maxHp) * move->magnitude)
			continue;

		slot.cooldownEndUs = nowUs + static_cast<uint64>(def->cooldownMs) * 1000;
		NotifyPassiveTrigger(player, def->skillId);

		// 뒤로 = 가해자 반대쪽. 모르거나 겹쳐 있으면 바라보는 반대쪽 (맹수의 본능과 같은 규칙)
		const uint64 id = player->objectInfo->object_id();
		const float x = player->posInfo->x();
		const float y = player->posInfo->y();
		float dx = 0.f;
		float dy = 0.f;
		if (attacker != nullptr && attacker != victim)
		{
			dx = x - attacker->posInfo->x();
			dy = y - attacker->posInfo->y();
		}
		if (dx * dx + dy * dy < 1.f)
		{
			const float yawRad = player->posInfo->yaw() * PI_F / 180.f;
			dx = -::cosf(yawRad);
			dy = -::sinf(yawRad);
		}
		PushCreature(player, id, dx, dy, move->distCm * MoveDistScale(player), move->speedCms, /*wallStun*/ false, nowUs);

		for (uint32 k = 0; k < def->effects.size(); k++)
		{
			const SkillEffect& e = def->effects[k];
			if (e.condition == SkillRequire::LowHp && e.type == SkillEffectType::SelfBuff)
				ApplyPassiveEffect(player, *def, k, nullptr, static_cast<Protocol::EquipSlot>(s + 1), 1.f, nowUs);
		}

		PushVisualZone(id, def->effectId, x, y, 80.f, LANDING_VISUAL_MS, "shock");
		LOG_LINE << "[ESCAPE] char=" << id << " skill=" << def->skillId << " hp=" << player->hp << "/" << player->maxHp << '\n';
		return;
	}
}

// ── 행군 1179 - 잠시 피해를 주고받지 않으면 이속 (UpdateTick 10 Hz) ──

void Room::UpdateIdleCombat(uint64 nowUs)
{
	for (auto& item : _objects)
	{
		if (item.second->IsPlayer() == false)
			continue;
		PlayerRef player = static_pointer_cast<Player>(item.second);
		if (player->IsAlive() == false)
			continue;

		for (int32 s = 0; s < SKILL_SLOT_COUNT; s++)
		{
			const SkillDef* def = BoundPassiveOf(player->skillSlots[s]);
			if (def == nullptr)
				continue;

			for (uint32 k = 0; k < def->effects.size(); k++)
			{
				const SkillEffect& e = def->effects[k];
				if (e.condition != SkillRequire::IdleCombat || e.type != SkillEffectType::SelfBuff)
					continue;
				if (nowUs < player->lastCombatUs + static_cast<uint64>(e.intervalMs) * 1000)
					continue;

				// 짧은 버프를 계속 갱신한다 - 싸우기 시작하면 갱신이 멈추고 곧 풀린다. 새로 걸릴 때만 알린다.
				// 클라에는 길게(1 시간) 알린다 - 갱신마다 보내지 않으니 클라가 1 s 뒤 스스로 지우면 안 된다. 끝은 만료 패킷이 알린다
				constexpr uint32 SHOWN_MS = 3'600'000;
				BuffInstance buff;
				buff.skillId = def->skillId;
				buff.stat = e.stat;
				buff.magnitude = e.magnitude;
				buff.endUs = nowUs + static_cast<uint64>(e.durationMs) * 1000;
				const bool fresh = player->status.AddBuff(buff);
				player->OnBuffsChanged();
				_statusTargets.insert(item.first);
				if (fresh)
				{
					PushStatus(true, item.first, Protocol::STATUS_BUFF, SkillTable::BuffStatName(e.stat), 1, SHOWN_MS, e.magnitude, item.first);
					LOG_LINE << "[MARCH] char=" << item.first << " skill=" << def->skillId << " +" << e.magnitude << '\n';
				}
			}
		}
	}
}

void Room::RecordHistories(uint64 nowUs)
{
	for (auto& item : _objects)
	{
		if (item.second->IsPlayer() == false && item.second->IsMonster() == false)
			continue;
		CreatureRef c = static_pointer_cast<Creature>(item.second);
		if (c->IsAlive())
			c->history.Record(nowUs, c->posInfo->x(), c->posInfo->y());
	}
}

// ── 스쿠툼 1149 - 속성이 깃든 채 방벽이 깨지면 둘레에 그 속성 피해 ──

void Room::BurstShield(const CreatureRef& owner, uint64 nowUs)
{
	const float ratio = owner->shieldBurstRatio;
	const float radius = owner->shieldBurstRadiusCm;
	owner->shieldBurstRatio = 0.f;
	owner->shieldBurstRadiusCm = 0.f;

	if (ratio <= 0.f || radius <= 0.f || owner->stance.empty() || owner->IsAlive() == false)
		return;

	const uint64 ownerId = owner->objectInfo->object_id();
	const float cx = owner->posInfo->x();
	const float cy = owner->posInfo->y();
	int32 hit = 0;

	// 후보를 먼저 모은다 - DealDamage 의 처치가 _objects 를 바꾼다 (ApplyThrow 와 같다, 2026-10-17 리뷰)
	for (const CreatureRef& c : SnapshotCreatures(ownerId))
	{
		if (c->IsAlive() == false || CanProjectileHit(owner, c, nowUs) == false)
			continue;
		const float dx = c->posInfo->x() - cx;
		const float dy = c->posInfo->y() - cy;
		if (dx * dx + dy * dy > radius * radius || HasLineOfAttack(owner, cx, cy, c) == false)
			continue;

		bool isCrit = false;
		const int32 damage = owner->ComputeSkillDamageTo(*c, 0.f, ratio, OUT isCrit);
		DamageSource src;
		src.skillId = owner->shieldSkillId;
		src.elem = owner->stance;
		DealDamage(owner, ownerId, c, damage, Protocol::DAMAGE_TYPE_MAGIC, isCrit, nowUs, src);
		hit++;
	}

	PushVisualZone(ownerId, owner->shieldSkillId, cx, cy, radius, LANDING_VISUAL_MS, owner->stance.c_str());
	LOG_LINE << "[SCUTUM] burst char=" << ownerId << " " << owner->stance << " hit=" << hit << '\n';
}

bool Room::DodgesProjectiles(const CreatureRef& target) const
{
	return target != nullptr && target->status.BuffMax(SkillBuffStat::ProjectileDodge) > 0.f;
}

bool Room::IsUnmovable(const CreatureRef& target) const
{
	return target != nullptr && target->status.BuffMax(SkillBuffStat::Unmovable) > 0.f;
}

float Room::IncomingDamageMult(const CreatureRef& attacker, const CreatureRef& target) const
{
	float mult = 1.f + target->status.BuffSum(SkillBuffStat::DamageTaken);

	// 철벽진 - 바라보는 쪽 120° 에서 온 피해만 (공격자 자리 기준). 지속 피해 · 장판(공격자가 떠난 것)도 공격자 자리로 본다
	const float guard = target->status.BuffMax(SkillBuffStat::FrontGuard);
	if (guard > 0.f && attacker != nullptr && attacker != target)
	{
		const float yawRad = target->posInfo->yaw() * PI_F / 180.f;
		const float dx = attacker->posInfo->x() - target->posInfo->x();
		const float dy = attacker->posInfo->y() - target->posInfo->y();
		const float len = ::sqrtf(dx * dx + dy * dy);
		if (len < 1.f || (dx * ::cosf(yawRad) + dy * ::sinf(yawRad)) / len >= FRONT_GUARD_COS)
			mult *= (std::max)(0.f, 1.f - guard);
	}
	return (std::max)(0.f, mult);
}

// ── 바쿠움 1154 - 지점으로 끌어모은다 ──

void Room::PullToPoint(const CreatureRef& target, uint64 instigatorId, float x, float y, float speedCms, uint64 nowUs)
{
	if (target == nullptr || target->IsAlive() == false || speedCms <= 0.f || target->IsDecoy())
		return;
	if (target->IsPlayer() && static_pointer_cast<Player>(target)->IsSpawnInvulnerable(nowUs))
		return;
	if (target->IsInvulnerable(nowUs))
		return;

	const uint64 targetId = target->objectInfo->object_id();
	if (IsUnmovable(target))
	{
		LOG_LINE << "[PULL] target=" << targetId << " by=" << instigatorId << " blocked=unmovable" << '\n';
		return;
	}

	// 중심에서 몸 하나만큼 - 다 같은 점에 겹치지 않게. 벽 너머는 끌리지 않는다 (끌기 규칙과 같다)
	const float startX = target->posInfo->x();
	const float startY = target->posInfo->y();
	const SkillKinematics::PushResult r = SkillKinematics::PullToward(Walls(), startX, startY, x, y, SkillKinematics::CAPSULE_RADIUS_CM, false);
	if (r.moved == false)
		return;

	target->posInfo->set_x(r.endX);
	target->posInfo->set_y(r.endY);
	_dirtyMovers.insert(targetId);

	const uint32 travelMs = SkillKinematics::TravelMs(r.distCm, speedCms);
	if (travelMs > 0)
		ApplyCcRaw(target, instigatorId, Protocol::CC_TYPE_KNOCKBACK, travelMs, 0.f, nowUs);

	float dirX = r.endX - startX;
	float dirY = r.endY - startY;
	const float len = ::sqrtf(dirX * dirX + dirY * dirY);
	if (len > 0.0001f)
	{
		dirX /= len;
		dirY /= len;
	}

	Protocol::KnockbackInfo& info = _pendingKnockbacks.emplace_back();
	info.set_target_id(targetId);
	info.set_instigator_id(instigatorId);
	info.set_dir_x(dirX);
	info.set_dir_y(dirY);
	info.set_distance(r.distCm);
	info.set_speed(speedCms);

	LOG_LINE << "[VACUUM] target=" << targetId << " by=" << instigatorId << " dist=" << ::lroundf(r.distCm) << '\n';
}

bool Room::IsSprinting(const CreatureRef& creature) const
{
	if (creature == nullptr || creature->IsHolding() == false)
		return false;
	const SkillDef* def = SkillTable::Find(creature->holdSkillId);
	return def != nullptr && creature->holdEffectIndex < def->effects.size() && def->effects[creature->holdEffectIndex].param == "sprint";
}
