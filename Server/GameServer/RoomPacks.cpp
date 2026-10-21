#include "pch.h"
#include "Room.h"
#include "Player.h"
#include "Monster.h"
#include "MonsterTable.h"
#include "PatrolRouteGen.h"
#include "TowerTable.h"
#include "SkillKinematics.h"
#include "SkillTable.h"
#include "Vision.h"
#include "GameSession.h"

// 몬스터 무리 - 우두머리 1 + 기본 3 이 한 줄로 순찰 길을 걷는다 (2026-10-11 사용자 지시 "몬스터 개선").
// 판단은 순수 로직 PackBrain(MonsterPack.cpp), 여기는 방 쪽 적용 - 스폰 · 판단 결과 적용 · 걸음 · 피격 · 사망 · 리스폰.
// 규칙: design/gdd/monster-ai.md § J. 예전 몬스터(집 지키는 병사 · 보스 · 테스트 더미)는 MonsterBrain 그대로다.

namespace
{
	constexpr float		MONSTER_Z = 100.f;			// monster_spawns.csv 와 같은 높이
	constexpr float		TEST_ROUTE_HALF_CM = 1000.f;	// /t pack - 내 둘레 20 m 네모

	float Dist(float ax, float ay, float bx, float by)
	{
		const float dx = ax - bx;
		const float dy = ay - by;
		return ::sqrtf(dx * dx + dy * dy);
	}
}

PackSlot* Room::PackOf(const Monster& monster)
{
	if (monster.packSlot == 0 || monster.packSlot > _packs.size())
		return nullptr;
	return _packs[monster.packSlot - 1].get();
}

void Room::InitPacks(uint64 nowUs)
{
	_packs.clear();

	// 생성 길 묶음이 있는 층 - 방마다 하나를 고른다 (월드 방 · 인스턴스마다 다른 동선, pack-routes spec R14).
	// 없으면 monster_packs.csv 그대로 (pack_rules.csv 가 없는 층)
	const size_t sets = MonsterTable::RouteSetCount(_floorId);
	if (sets > 0)
	{
		const uint64 pick = (nowUs ^ (static_cast<uint64>(_instanceId) * 0x9E3779B97F4A7C15ull) ^ (static_cast<uint64>(_floorId) << 32)) % sets;
		const RouteSet* set = MonsterTable::RouteSetAt(_floorId, pick);
		for (const GeneratedPack& gp : set->packs)
		{
			auto slot = make_unique<PackSlot>();
			slot->def = gp.def;
			slot->route = gp.route;
			slot->def.route = &slot->route;
			_packs.push_back(move(slot));
			SpawnPack(static_cast<uint32>(_packs.size() - 1), nowUs);
		}
		LOG_LINE << "[ROUTE] " << Tag() << " floor=" << _floorId << " set=" << pick << '/' << sets << " seed=" << set->seed
			<< " packs=" << _packs.size() << " crossings=" << set->stats.crossings << " coverage=" << ::lroundf(set->stats.coveragePct) << "%\n";
	}
	else
	{
		for (const MonsterPackDef& def : MonsterTable::Packs())
		{
			if (def.floorId != _floorId || def.route == nullptr)
				continue;

			auto slot = make_unique<PackSlot>();
			slot->def = def;
			slot->route = *def.route;
			slot->def.route = &slot->route;
			_packs.push_back(move(slot));
			SpawnPack(static_cast<uint32>(_packs.size() - 1), nowUs);
		}
	}

	if (_packs.empty() == false)
		LOG_LINE << "[PACK] floor=" << _floorId << " packs=" << _packs.size() << '\n';
}

void Room::SpawnPack(uint32 slotIndex, uint64 nowUs)
{
	PackSlot& slot = *_packs[slotIndex];
	const MonsterPackDef& def = slot.def;
	const MonsterTypeDef* leader = MonsterTable::FindType(def.leaderTypeId);
	const MonsterTypeDef* member = MonsterTable::FindType(def.memberTypeId);
	if (leader == nullptr || member == nullptr || slot.route.points.empty())
		return;

	const uint32 count = 1 + def.memberCount;
	vector<pair<float, float>> positions = PackBrain::FilePositions(slot.route, def.startSeq, count, def.spacingCm);

	// 길이 가리키는 방향 - 머리 → 다음 점
	const auto& pts = slot.route.points;
	const auto& next = pts[(def.startSeq + 1) % pts.size()];
	const float yaw = ::atan2f(next.second - positions[0].second, next.first - positions[0].first) * 180.f / PI_F;

	// 자리 정의는 처음 한 번만 만든다 - 몬스터가 가리킨다 (크기를 바꾸지 않는다)
	if (slot.spawns.size() != count)
	{
		slot.spawns.clear();
		slot.spawns.reserve(count);
		slot.spawns.resize(count);
	}

	vector<uint64> ids;
	ids.reserve(count);
	for (uint32 i = 0; i < count; i++)
	{
		const MonsterTypeDef& type = (i == 0) ? *leader : *member;

		// 테스트 무리의 길은 검사를 안 거쳤다 - 벽에 걸리면 가까운 빈 자리
		float x = positions[i].first;
		float y = positions[i].second;
		if (Walls().Overlaps(x, y, type.radiusCm))
			Walls().FindNearestFree(x, y, type.radiusCm, 400.f, OUT x, OUT y);
		positions[i] = { x, y };

		MonsterSpawnDef& sp = slot.spawns[i];
		sp.spawnId = def.packId * 10 + i;
		sp.floorId = _floorId;
		sp.typeId = type.typeId;
		sp.x = x;
		sp.y = y;
		sp.z = MONSTER_Z;
		sp.yaw = yaw;
		sp.groupId = 0;
		sp.respawnSeconds = def.respawnSeconds;

		const uint64 id = GNextMonsterId.fetch_add(1);
		MonsterRef monster = make_shared<Monster>(id, type, sp);
		MonsterBrain::Reset(monster->brain, nowUs);
		monster->lastStepUs = nowUs;
		monster->packSlot = slotIndex + 1;
		monster->packIndex = static_cast<uint8>(i);
		if (slot.isTest)
		{
			// 테스트 무리 - 드랍 · 리스폰 없음, /t clear · 주인이 떠나면 치운다 (더미 규칙 그대로)
			monster->isDummy = true;
			monster->dummyOwnerId = slot.testOwnerId;
		}

		if (AddObject(monster) == false)
			continue;

		_monsters[id] = monster;
		_pendingSpawns.push_back(id);
		GMonsterStats.alive.fetch_add(1);
		ids.push_back(id);
	}

	PackBrain::Spawn(slot.state, def, ids, positions, nowUs);
	slot.wiped = ids.empty();
	slot.respawnAtUs = 0;
	slot.deferStartUs = 0;

	LOG_LINE << "[PACK] spawn id=" << def.packId << " route=" << def.routeId << " kind=" << MonsterTable::AggroKindName(def.kind)
		<< " members=" << ids.size() << " head=(" << ::lroundf(positions[0].first) << "," << ::lroundf(positions[0].second) << ")"
		<< (slot.isTest ? " test" : "") << '\n';
}

bool Room::SeenByAnyone(uint64 objectId) const
{
	// FoW 를 끄면 모두가 모두를 본다 - 순간 이동은 시간 상한에서만
	if (Vision::Enabled() == false)
		return true;

	for (const auto& item : _objects)
	{
		if (item.second->IsPlayer() == false)
			continue;
		const Player* player = static_cast<const Player*>(item.second.get());
		if (player->fowKnown.find(objectId) != player->fowKnown.end())
			return true;
	}
	return false;
}

void Room::SnapPackMember(const MonsterRef& monster, float x, float y, uint64 nowUs)
{
	float outX = x;
	float outY = y;
	Walls().FindNearestFree(x, y, monster->type->radiusCm, 300.f, OUT outX, OUT outY);

	monster->posInfo->set_x(outX);
	monster->posInfo->set_y(outY);
	monster->posInfo->set_state(Protocol::MOVE_STATE_IDLE);
	monster->brain.stuckAccumUs = 0;
	monster->brain.windowStartUs = 0;
	monster->brain.windowProgressCm = 0.f;
	_dirtyMovers.insert(monster->objectInfo->object_id());
	GMonsterStats.resetSnaps.fetch_add(1);

	LOG_LINE << "[PACK] snap id=" << monster->objectInfo->object_id() << " to=(" << ::lroundf(outX) << "," << ::lroundf(outY) << ")" << '\n';
}

void Room::UpdatePacks(uint64 nowUs, const LineClearFn& lineClear)
{
	if (_packs.empty())
		return;

	// 타워 둘레 - 무리는 탈출 구역 안으로 쫓아 들어가지 않는다
	vector<pair<float, float>> noGo;
	for (const TowerDef& t : TowerTable::All())
	{
		if (t.floorId == _floorId)
			noGo.emplace_back(t.x, t.y);
	}

	vector<PackMemberView> views;
	vector<MonsterRef> refs;

	for (uint32 si = 0; si < _packs.size(); si++)
	{
		PackSlot& slot = *_packs[si];

		// 테스트 무리의 주인이 층을 떠났다 - 그 사람의 더미와 함께 치운다
		if (slot.isTest && slot.wiped == false && _objects.find(slot.testOwnerId) == _objects.end())
		{
			LOG_LINE << "[TEST] owner " << slot.testOwnerId << " left the floor - removing their test pack\n";
			TestClearDummies(slot.testOwnerId);
			continue;
		}

		// ── 전멸 → 통째로 다시 (자리 곁 12 m 에 플레이어가 있으면 최대 60 초 미룬다, F9) ──
		if (slot.wiped)
		{
			if (slot.isTest || slot.respawnAtUs == 0 || nowUs < slot.respawnAtUs)
				continue;

			const auto& start = slot.route.points[slot.def.startSeq % slot.route.points.size()];
			bool playerClose = false;
			for (const BrainPlayer& p : _brainPlayers)
			{
				if (Dist(p.x, p.y, start.first, start.second) <= MonsterConfig::RESPAWN_DEFER_RADIUS_CM)
				{
					playerClose = true;
					break;
				}
			}
			if (playerClose && nowUs < slot.respawnAtUs + MonsterConfig::RESPAWN_DEFER_MAX_US)
			{
				if (slot.deferStartUs == 0)
				{
					slot.deferStartUs = nowUs;
					LOG_LINE << "[PACK] respawn deferred id=" << slot.def.packId << " - player within 12 m of the start\n";
				}
				continue;
			}

			SpawnPack(si, nowUs);
			continue;
		}

		// ── 살아 있는 몸 (줄 순서) ──
		views.clear();
		refs.clear();
		for (uint32 i = 0; i < slot.state.members.size(); i++)
		{
			const uint64 id = slot.state.members[i].id;
			if (id == 0)
				continue;
			auto it = _monsters.find(id);
			if (it == _monsters.end() || it->second->diedAtUs != 0)
				continue;

			const MonsterRef& m = it->second;
			PackMemberView v;
			v.index = i;
			v.x = m->posInfo->x();
			v.y = m->posInfo->y();
			v.windingUp = (m->attackHitAtUs != 0);
			v.casting = m->IsCasting() || nowUs < m->holdUntilUs;		// 돌진 예고 · 날아가는 동안도 (§ L)
			v.stuckAccumUs = m->brain.stuckAccumUs;
			v.attackRangeCm = m->type->attackRangeCm;
			// 실명한 몸은 아무도 보지 못한다 (뇌섬 · 번-쩍) - 감지에서 빠진다
			v.aggroRadiusCm = (nowUs < m->blindEndUs) ? 0.f : m->type->aggroRadiusCm;
			v.leashRadiusCm = m->type->leashRadiusCm;
			v.giveUpS = m->type->giveUpS;
			v.moveSpeedCms = m->type->moveSpeedCms;
			views.push_back(v);
			refs.push_back(m);
		}
		if (views.empty())
			continue;

		PackInput in;
		in.nowUs = nowUs;
		in.def = &slot.def;
		in.living = &views;
		in.players = &_brainPlayers;
		in.lineClear = lineClear;
		in.noGo = &noGo;

		const PackOutput out = PackBrain::Think(slot.state, in);

		if (out.aggroTargetId != 0)
		{
			LOG_LINE << "[PACK] aggro id=" << slot.def.packId << " by=" << out.aggroBy << " target=" << out.aggroTargetId
				<< " kind=" << MonsterTable::AggroKindName(slot.def.kind) << " members=" << views.size() << '\n';
		}
		if (out.retargeted)
			LOG_LINE << "[PACK] retarget id=" << slot.def.packId << " to=" << slot.state.targetId << '\n';
		if (out.enteredReturn)
			LOG_LINE << "[PACK] return id=" << slot.def.packId << " reason=" << (out.returnReason ? out.returnReason : "?") << '\n';

		// 복귀가 끝났다 - 만피 · CC 해제 (예전 T11 과 같다). 클라는 피해 0 · 남은 체력 = 최대 로 체력바를 되돌린다
		if (out.resumed)
		{
			for (const MonsterRef& m : refs)
			{
				m->RestoreFull();
				Protocol::DamageInfo& heal = _pendingDamages.emplace_back();
				heal.set_attacker_id(0);
				heal.set_target_id(m->objectInfo->object_id());
				heal.set_damage(0);
				heal.set_remaining_hp(m->hp);
			}
			LOG_LINE << "[PACK] resume id=" << slot.def.packId << " wp=" << slot.state.waypoint << '\n';
		}

		// 복귀가 막혔다 - 아무도 안 볼 때만 앵커로 옮긴다 (보는 앞에서 순간 이동하지 않게). 시간 상한을 넘으면 그냥
		for (uint32 memberIndex : out.snapMembers)
		{
			for (const MonsterRef& m : refs)
			{
				if (m->packIndex != memberIndex)
					continue;
				const bool hardTimeout = (nowUs - slot.state.modeSinceUs >= MonsterConfig::RETURN_HARD_TIMEOUT_US);
				if (hardTimeout || SeenByAnyone(m->objectInfo->object_id()) == false)
				{
					const PackMember& pm = slot.state.members[memberIndex];
					SnapPackMember(m, pm.anchorX, pm.anchorY, nowUs);
				}
				break;
			}
		}

		// 결과 적용 - 순찰은 StepPacks 가 길 · 발자국으로 직접 걷는다. 교전 · 복귀는 몸마다 목표
		for (size_t k = 0; k < refs.size(); k++)
		{
			const MonsterRef& m = refs[k];
			m->brain.state = (slot.state.mode == PackMode::Engage) ? MonsterState::Chase
				: (slot.state.mode == PackMode::Return) ? MonsterState::Return : MonsterState::Idle;

			if (slot.state.mode == PackMode::Patrol || nowUs < m->blindEndUs)
			{
				m->decision = BrainOutput();
				continue;
			}

			m->decision = out.perMember[k];

			// 몬스터 스킬 (§ L - 멧돼지 우두머리 돌진 9002). 교전 중 · 무리의 목표에게만. 쓰면 이번 틱 평타는 없다
			if (slot.state.mode == PackMode::Engage && m->type->skillId != 0 && nowUs >= m->brain.skillReadyAtUs
				&& views[k].casting == false && views[k].windingUp == false)
			{
				const SkillDef* skill = SkillTable::Find(m->type->skillId);
				const BrainPlayer* target = nullptr;
				for (const BrainPlayer& p : _brainPlayers)
				{
					if (p.id == slot.state.targetId)
					{
						target = &p;
						break;
					}
				}
				if (skill != nullptr && target != nullptr)
				{
					const MonsterCastDecision c = MonsterSkillUse::Decide(*skill, views[k].x, views[k].y, target, _brainPlayers, lineClear);
					if (c.cast && BeginMonsterCast(m, *skill, nowUs, c.targetId, c.aimX, c.aimY))
						continue;
				}
			}

			if (m->decision.attackTargetId != 0)
				HandleAttack(m->objectInfo->object_id(), m->decision.attackTargetId);
		}
	}
}

void Room::StepPacks(uint64 nowUs)
{
	if (_packs.empty())
		return;

	const WallMap& walls = Walls();
	auto blocked = [&walls](float ax, float ay, float bx, float by, float r) { return walls.SweepBlocked(ax, ay, bx, by, r); };
	const NavGrid* nav = Nav();

	// 몸 하나 한 걸음 - 예전 StepMonsters 와 같은 규칙 (CC · windup · 시전이면 0, 움직였으면 RUN, 처음 멈춘 걸음에 IDLE 한 번).
	// 길찾기는 교전 · 복귀 때만 (§ K) - 순찰 길은 생성기가 이미 몸 반지름으로 검사했다
	auto stepOne = [&](const MonsterRef& m, const BrainOutput& goal, float speed, bool usePath) -> bool
		{
			const uint64 id = m->objectInfo->object_id();
			const uint64 dtUs = (m->lastStepUs != 0 && nowUs > m->lastStepUs) ? nowUs - m->lastStepUs : 0;
			m->lastStepUs = nowUs;
			Protocol::PosInfo* pos = m->posInfo;

			// 몬스터 스킬의 예고 · 돌진 중 - 그 자리에 선다 (§ L 돌진, 2026-10-17)
			if (nowUs < m->holdUntilUs)
			{
				if (pos->state() == Protocol::MOVE_STATE_RUN)
				{
					pos->set_state(Protocol::MOVE_STATE_IDLE);
					_dirtyMovers.insert(id);
					GMonsterStats.moveEntries.fetch_add(1);
				}
				return false;
			}

			if (goal.hasGoal == false)
			{
				if (pos->state() == Protocol::MOVE_STATE_RUN)
				{
					pos->set_state(Protocol::MOVE_STATE_IDLE);
					_dirtyMovers.insert(id);
					GMonsterStats.moveEntries.fetch_add(1);
				}
				return false;
			}

			if (m->CanMove(nowUs) == false || m->attackHitAtUs != 0 || m->IsCasting())
				speed = 0.f;

			StepInput in;
			in.nowUs = nowUs;
			in.dtUs = dtUs;
			in.x = pos->x();
			in.y = pos->y();
			in.goalX = goal.goalX;
			in.goalY = goal.goalY;
			in.stopDistCm = goal.stopDistCm;
			in.speedCms = speed;
			in.radiusCm = m->type->radiusCm;
			in.blocked = blocked;
			if (usePath)
			{
				in.nav = nav;
				in.pathBudget = &_pathBudget;
			}

			const StepOutput o = StepMonsterBody(m, in);
			if (o.moved)
			{
				pos->set_x(o.x);
				pos->set_y(o.y);
				pos->set_yaw(o.yawDeg);
				pos->set_state(Protocol::MOVE_STATE_RUN);
				_dirtyMovers.insert(id);
				GMonsterStats.moveEntries.fetch_add(1);
				return true;
			}

			const float faceYaw = ::atan2f(goal.goalY - pos->y(), goal.goalX - pos->x()) * 180.f / PI_F;
			const float diff = ::fmodf(faceYaw - pos->yaw() + 540.f, 360.f) - 180.f;
			const bool turn = ::fabsf(diff) > 10.f && (goal.goalX != pos->x() || goal.goalY != pos->y());
			if (pos->state() == Protocol::MOVE_STATE_RUN || turn)
			{
				if (turn)
					pos->set_yaw(faceYaw);
				pos->set_state(Protocol::MOVE_STATE_IDLE);
				_dirtyMovers.insert(id);
				GMonsterStats.moveEntries.fetch_add(1);
			}
			return false;
		};

	vector<MonsterRef> refs;
	for (unique_ptr<PackSlot>& slotPtr : _packs)
	{
		PackSlot& slot = *slotPtr;
		if (slot.wiped)
			continue;

		refs.clear();
		for (const PackMember& pm : slot.state.members)
		{
			if (pm.id == 0)
				continue;
			auto it = _monsters.find(pm.id);
			if (it != _monsters.end() && it->second->diedAtUs == 0)
				refs.push_back(it->second);
		}
		if (refs.empty())
			continue;

		if (slot.state.mode != PackMode::Patrol)
		{
			for (const MonsterRef& m : refs)
			{
				const BrainOutput& d = m->decision;
				stepOne(m, d, m->type->moveSpeedCms * (1.f - m->activeSlow) * d.speedMult, /*usePath*/ true);
			}
			continue;
		}

		// ── 순찰 - 머리는 길을, 뒤는 머리 발자국을 ──
		// 맨 뒤 몸이 처지면 머리가 늦추거나 기다린다 (최대 5 초 - 묶인 몸을 영영 기다리지 않게)
		float cohesion = 1.f;
		if (refs.size() > 1)
		{
			const MonsterRef& tail = refs.back();
			float tx = tail->posInfo->x();
			float ty = tail->posInfo->y();
			slot.state.trail.PointAt(slot.state.trail.HeadS() - static_cast<float>(refs.size() - 1) * slot.def.spacingCm, OUT tx, OUT ty);
			const float lag = Dist(tail->posInfo->x(), tail->posInfo->y(), tx, ty);

			if (lag > MonsterConfig::COHESION_STOP_CM)
			{
				if (slot.state.cohesionWaitSinceUs == 0)
					slot.state.cohesionWaitSinceUs = nowUs;
				if (nowUs - slot.state.cohesionWaitSinceUs < MonsterConfig::COHESION_MAX_WAIT_US)
					cohesion = 0.f;
			}
			else
			{
				slot.state.cohesionWaitSinceUs = 0;
				if (lag > MonsterConfig::COHESION_SLOW_CM)
					cohesion = 0.5f;
			}
		}

		for (uint32 rank = 0; rank < refs.size(); rank++)
		{
			const MonsterRef& m = refs[rank];
			const float x = m->posInfo->x();
			const float y = m->posInfo->y();
			const BrainOutput goal = PackBrain::PatrolGoal(slot.state, slot.def, rank, x, y);

			float speed = 0.f;
			if (rank == 0)
				speed = slot.def.patrolSpeedCms * cohesion;
			else
				speed = (std::min)(m->type->moveSpeedCms, slot.def.patrolSpeedCms + MonsterConfig::FOLLOW_GAIN_PER_S * Dist(x, y, goal.goalX, goal.goalY));
			speed *= (1.f - m->activeSlow);

			stepOne(m, goal, speed, /*usePath*/ false);

			// 머리의 발자국 - 뒤의 몸이 밟는다 (멈춰 있어도 지금 자리를 머리로)
			if (rank == 0)
				slot.state.trail.Record(m->posInfo->x(), m->posInfo->y());
		}
	}
}

void Room::OnPackMemberDamaged(const MonsterRef& monster, uint64 attackerId, float ax, float ay, uint64 nowUs)
{
	if (PackSlot* slot = PackOf(*monster))
		PackBrain::OnMemberDamaged(slot->state, attackerId, ax, ay, nowUs);
}

void Room::OnPackMemberDied(const MonsterRef& monster, uint64 nowUs)
{
	PackSlot* slot = PackOf(*monster);
	if (slot == nullptr)
		return;

	const int32 headBefore = PackBrain::HeadIndex(slot->state);
	const bool anyLeft = PackBrain::OnMemberDied(slot->state, monster->packIndex, nowUs);

	if (anyLeft == false)
	{
		slot->wiped = true;
		slot->respawnAtUs = slot->isTest ? 0 : nowUs + static_cast<uint64>(slot->def.respawnSeconds) * 1'000'000;
		LOG_LINE << "[PACK] wiped id=" << slot->def.packId << (slot->isTest ? " (test - no respawn)" : "")
			<< " respawn_in=" << (slot->isTest ? 0 : slot->def.respawnSeconds) << "s" << '\n';
		return;
	}

	const int32 headAfter = PackBrain::HeadIndex(slot->state);
	if (headAfter != headBefore)
		LOG_LINE << "[PACK] head id=" << slot->def.packId << " leader down - new head=" << slot->state.members[headAfter].id << " (keeps walking)" << '\n';
}

void Room::DropTestPacks(uint64 ownerId)
{
	for (unique_ptr<PackSlot>& slot : _packs)
	{
		if (slot->isTest && slot->testOwnerId == ownerId && slot->wiped == false)
		{
			slot->wiped = true;
			slot->respawnAtUs = 0;
			for (PackMember& pm : slot->state.members)
				pm.id = 0;
		}
	}
}

void Room::TestSpawnPack(uint64 playerId, bool reactive)
{
	JOB_TAG();
	auto it = _objects.find(playerId);
	PlayerRef player = (it != _objects.end()) ? AsPlayer(it->second) : nullptr;
	if (player == nullptr || player->IsAlive() == false)
		return;

	// 종류 · 수치는 표의 같은 종류 무리에서 빌린다 (없으면 4 · 5 / 6 · 7)
	const MonsterAggroKind kind = reactive ? MonsterAggroKind::Reactive : MonsterAggroKind::Detect;
	MonsterPackDef def;
	def.leaderTypeId = reactive ? 7 : 5;
	def.memberTypeId = reactive ? 6 : 4;
	def.memberCount = 3;
	def.spacingCm = 150.f;
	def.patrolSpeedCms = 170.f;
	def.respawnSeconds = 1800;
	for (const MonsterPackDef& d : MonsterTable::Packs())
	{
		if (d.kind == kind)
		{
			def = d;
			break;
		}
	}
	if (MonsterTable::FindType(def.leaderTypeId) == nullptr || MonsterTable::FindType(def.memberTypeId) == nullptr)
	{
		LOG_LINE << "[TEST] pack rejected - no pack monster types in monster_types.csv char=" << playerId << '\n';
		return;
	}

	// 길 = 내 둘레 20 m 네모 (반시계). 머리는 내 앞 오른쪽 모서리에서
	const float px = player->posInfo->x();
	const float py = player->posInfo->y();
	PatrolRoute route;
	route.routeId = 0;
	route.floorId = _floorId;
	route.points = {
		{ px + TEST_ROUTE_HALF_CM, py - TEST_ROUTE_HALF_CM },
		{ px + TEST_ROUTE_HALF_CM, py + TEST_ROUTE_HALF_CM },
		{ px - TEST_ROUTE_HALF_CM, py + TEST_ROUTE_HALF_CM },
		{ px - TEST_ROUTE_HALF_CM, py - TEST_ROUTE_HALF_CM },
	};

	// 다 쓴 테스트 자리를 다시 쓴다 (몬스터가 위치 + 1 로 가리키므로 지우지 않는다). 시체가 아직 그 자리를 가리키면 건너뛴다
	uint32 slotIndex = static_cast<uint32>(_packs.size());
	for (uint32 i = 0; i < _packs.size(); i++)
	{
		if (_packs[i]->isTest == false || _packs[i]->wiped == false)
			continue;

		bool referenced = false;
		for (const auto& [id, m] : _monsters)
		{
			if (m->packSlot == i + 1)
			{
				referenced = true;
				break;
			}
		}
		if (referenced == false)
		{
			slotIndex = i;
			break;
		}
	}
	if (slotIndex == _packs.size())
		_packs.push_back(make_unique<PackSlot>());

	PackSlot& slot = *_packs[slotIndex];
	slot.isTest = true;
	slot.testOwnerId = playerId;
	slot.route = route;
	slot.def = def;
	slot.def.packId = 9000 + slotIndex;
	slot.def.floorId = _floorId;
	slot.def.routeId = 0;
	slot.def.startSeq = 0;
	slot.def.kind = kind;
	slot.def.route = &slot.route;

	SpawnPack(slotIndex, Utils::NowMicroseconds());
	LOG_LINE << "[TEST] pack spawn char=" << playerId << " kind=" << MonsterTable::AggroKindName(kind) << " around=(" << ::lroundf(px) << "," << ::lroundf(py) << ")" << '\n';
}
