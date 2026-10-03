#include "pch.h"
#include "Room.h"
#include "Player.h"
#include "SkillTable.h"
#include "ServerStats.h"
#include "ObjectUtils.h"
#include "Bag.h"

namespace
{
	constexpr double	VALIDATION_MARGIN = 1.15;	// validation_margin
	constexpr double	MAX_BUDGET_SEC = 0.5;
	constexpr uint64	MOVE_FLUSH_MS = 33;
	constexpr uint64	COMBAT_FLUSH_MS = 33;
	constexpr uint64	SPAWN_FLUSH_MS = 33;
	constexpr int32		CC_STATE_TICKS = 30;

	constexpr float		CAST_MOVE_EPSILON = 1.f;
	constexpr float		CAST_MOVE_EPSILON_SQ = CAST_MOVE_EPSILON * CAST_MOVE_EPSILON;

	constexpr uint64	SAVE_INTERVAL_US = 60'000'000;
	constexpr int32		SAVE_PER_TICK = 2;

	constexpr float		SPAWN_SPACING_CM = 150.0f;
	constexpr int32		SPAWN_MAX_RINGS = 6;
	constexpr float		SPAWN_PI = 3.14159265f;
	constexpr float		LOOT_RANGE_CM = 200.f;
}

namespace
{
	const char* PickHardCcReason(const Creature& c, uint64 nowUs, OUT uint64& outRemainMs)
	{
		const char* reason = "none";
		uint64 endUs = 0;

		if (c.stunEndUs > endUs) { endUs = c.stunEndUs; reason = "stun"; }
		if (c.rootEndUs > endUs) { endUs = c.rootEndUs; reason = "root"; }
		if (c.knockbackEndUs > endUs) { endUs = c.knockbackEndUs; reason = "knockback"; }
		if (c.launchEndUs > endUs) { endUs = c.launchEndUs; reason = "launch"; }

		outRemainMs = (endUs > nowUs) ? ((endUs - nowUs) / 1000) : 0;
		return reason;
	}
	// TEST
	void LogSlots(const PlayerRef& player, const char* tag)
	{
		// 한 이벤트를 여러 문장으로 찍으므로 객체를 직접 둔다. 소멸할 때 한 번에 나간다.
		std::osyncstream out(std::cout);

		out << "[ITEM] " << tag << " char=" << player->objectInfo->object_id()
			<< " inv=" << player->inventory.size() << " slots=";

		for (int32 i = 0; i < SKILL_SLOT_COUNT; i++)
		{
			const SkillSlot& s = player->skillSlots[i];
			out << " [" << (i + 1) << "]" << s.skillId;

			if (s.bindState == Protocol::BIND_SHADOWED)
				out << "(S<-" << s.shadowedBy << ")";
		}

		out << " inv_ids=";
		for (const ItemEntry& e : player->inventory)
			out << e.instanceId << "(g" << e.grade << ") ";

		out << '\n';
	}

	bool IsInAttackRange(const CreatureRef& attacker, const CreatureRef& target)
	{
		const float dx = target->posInfo->x() - attacker->posInfo->x();
		const float dy = target->posInfo->y() - attacker->posInfo->y();

		return (dx * dx + dy * dy) <= (attacker->attackRange * attacker->attackRange);
	}

	bool IsHardCcType(Protocol::CcType type)
	{
		return type == Protocol::CC_TYPE_STUN
			|| type == Protocol::CC_TYPE_KNOCKBACK
			|| type == Protocol::CC_TYPE_LAUNCH
			|| type == Protocol::CC_TYPE_ROOT;
	}

	bool IsInSkillRange(const CreatureRef& caster, const CreatureRef& target, float rangeCm)
	{
		const float dx = target->posInfo->x() - caster->posInfo->x();
		const float dy = target->posInfo->y() - caster->posInfo->y();

		return (dx * dx + dy * dy) <= (rangeCm * rangeCm);
	}

	bool IsInLootRange(const PlayerRef& player, const BagRef& bag)
	{
		const float dx = bag->posInfo->x() - player->posInfo->x();
		const float dy = bag->posInfo->y() - player->posInfo->y();

		return (dx * dx + dy * dy) <= (LOOT_RANGE_CM * LOOT_RANGE_CM);
	}

	SendBufferRef MakeBagContents(const BagRef& bag, uint64 nowUs)
	{
		Protocol::S_BAG_CONTENTS pkt;
		pkt.set_bag_id(bag->objectInfo->object_id());
		pkt.set_remaining_ms(bag->expiresAtUs > nowUs ? (bag->expiresAtUs - nowUs) / 1000 : 0);

		for (const ItemEntry& e : bag->items)
			pkt.add_items()->CopyFrom(e.ToProto(Protocol::ITEM_STATE_IN_BAG));

		return ServerPacketHandler::MakeSendBuffer(pkt);
	}
}

static RoomRef MakeFloorRoom(uint32 floorId)
{
	RoomRef room = make_shared<Room>();
	room->_floorId = floorId;
	return room;
}

RoomRef GRooms[FLOOR_COUNT] =
{
	MakeFloorRoom(0),
	MakeFloorRoom(1),
	MakeFloorRoom(2),
	MakeFloorRoom(3)
};

RoomRef GetRoomForFloor(uint32 floorId)
{
	if (floorId >= FLOOR_COUNT)
		return nullptr;

	return GRooms[floorId];
}

Room::Room()
{
}

Room::~Room()
{
}

bool Room::EnterRoom(ObjectRef object, bool randPos)
{
	if (object->posInfo->x() == 0.f && object->posInfo->y() == 0.f)
	{
		const SpawnPoint& spawn = FLOOR_SPAWN[_floorId];

		float spawnX = 0.f;
		float spawnY = 0.f;
		FindSpawnPosition(spawn.x, spawn.y, spawnX, spawnY);

		object->posInfo->set_x(spawnX);
		object->posInfo->set_y(spawnY);
		object->posInfo->set_z(100.f);
	}
	bool success = AddObject(object);

	// 랜덤 위치
	if (randPos)
	{
		object->posInfo->set_x(Utils::GetRandom(0.f, 500.f));
		object->posInfo->set_y(Utils::GetRandom(0.f, 500.f));
		object->posInfo->set_z(100.f);
		object->posInfo->set_yaw(Utils::GetRandom(0.f, 100.f));
	}

	// 입장 사실을 신입 플레이어에게 알린다.
	if (auto player = dynamic_pointer_cast<Player>(object))
	{
		const uint64 enterUs = Utils::NowMicroseconds();
		player->lastMoveUs = enterUs;
		player->lastSaveUs = enterUs;


		if (auto session = player->session.lock())
		{
			if (success)
			{
				session->place.store(SessionPlace::Dungeon);
			}
			else
			{
				SessionPlace expected = SessionPlace::Entering;
				session->place.compare_exchange_strong(expected, SessionPlace::BaseCamp);
			}
		}

		player->moveBudget = player->GetSpeedCeiling(enterUs) * VALIDATION_MARGIN * MAX_BUDGET_SEC;

		Protocol::S_ENTER_GAME enterGamePkt;
		enterGamePkt.set_success(success);

		Protocol::ObjectInfo* playerInfo = new Protocol::ObjectInfo();
		playerInfo->CopyFrom(*player->objectInfo);
		enterGamePkt.set_allocated_player(playerInfo);
		//enterGamePkt.release_player();

		SendBufferRef sendBuffer = ServerPacketHandler::MakeSendBuffer(enterGamePkt);
		if (auto session = player->session.lock())
			session->Send(sendBuffer);

		{
			player->RebuildSkillSlotsFromEquipment();
			LogSlots(player, "enter");

			SendEquipSync(player);
			SendInventorySync(player);
		}
	}

	_pendingSpawns.push_back(object->objectInfo->object_id());

	return success;
}

bool Room::LeaveRoom(ObjectRef object)
{
	if (object == nullptr)
		return false;

	const uint64 objectId = object->objectInfo->object_id();
	bool success = RemoveObject(objectId);

	if (auto player = dynamic_pointer_cast<Player>(object))
	{
		SavePlayer(player);

		if (auto session = player->session.lock())
			session->place.store(SessionPlace::BaseCamp);
	}

	// 퇴장 사실을 퇴장하는 플레이어에게 알린다.
	if (auto player = dynamic_pointer_cast<Player>(object))
	{
		Protocol::S_LEAVE_GAME leaveGamePkt;

		SendBufferRef sendBuffer = ServerPacketHandler::MakeSendBuffer(leaveGamePkt);
		if (auto session = player->session.lock())
			session->Send(sendBuffer);
	}

	// 퇴장 사실을 알린다.
	{
		Protocol::S_DESPAWN despawnPkt;
		despawnPkt.add_object_ids(objectId);

		SendBufferRef sendBuffer = ServerPacketHandler::MakeSendBuffer(despawnPkt);
		Broadcast(sendBuffer, objectId);

		if (auto player = dynamic_pointer_cast<Player>(object))
			if (auto session = player->session.lock())
				session->Send(sendBuffer);
		// "bool success = LeavePlayer(objectId);" 이 부분이 위에서 누락될 경우를 대비하여
	}

	return success;
}

bool Room::HandleEnterPlayer(PlayerRef player)
{
	// 적재하는 사이에 끊겼다 - 런이 시작되기 전이라 몸을 남기지 않는다.
	GameSessionRef session = player->session.lock();
	if (session == nullptr || session->IsConnected() == false)
	{
		LOG_LINE << "[ENTER] skipped - session gone. char=" << player->objectInfo->object_id() << '\n';
		return false;
	}

	const uint64 beginUs = Utils::NowMicroseconds();

	const bool result = EnterRoom(player, false);

	const uint64 elapsedUs = Utils::NowMicroseconds() - beginUs;

	_enterCount++;
	_enterTotalUs += elapsedUs;
	if (elapsedUs > _enterMaxUs)
		_enterMaxUs = elapsedUs;

	if (_enterCount % 30 == 0)
	{
		LOG_LINE << "[ENTER] n=" << _enterCount
			<< " avg=" << (_enterTotalUs / _enterCount) << "us"
			<< " max=" << _enterMaxUs << "us" << '\n';
	}

	return result;
}

bool Room::HandleLeavePlayer(PlayerRef player)
{
	// 던전에서 살아 있는 채로 나가는 길은 탈출뿐이다 - 나가기로 사망을 피하는 허점을 막는다.
	if (_objects.find(player->objectInfo->object_id()) != _objects.end() && player->IsAlive())
	{
		LOG_LINE << "[LEAVE] rejected - alive in dungeon. char=" << player->objectInfo->object_id() << '\n';
		return false;
	}

	return LeaveRoom(player);
}

void Room::HandleMove(Protocol::C_MOVE pkt)
{
	const uint64 objectId = pkt.info().object_id();
	
	auto findIt = _objects.find(objectId);
	if (findIt == _objects.end())
		return;

	// 적용
	PlayerRef player = dynamic_pointer_cast<Player>(findIt->second);
	if (player == nullptr)
		return;

	if (player->IsAlive() == false)
		return;

	const uint64 blockCheckUs = Utils::NowMicroseconds();
	if (player->CanMove(blockCheckUs) == false)
	{
		uint64 remainMs = 0;
		const char* reason = PickHardCcReason(*player, blockCheckUs, OUT remainMs);

		GStats.AddMoveBlocked();

		LOG_LINE << "[MOVE BLOCKED] id=" << objectId
			<< " reason=" << reason
			<< " remain=" << remainMs << "ms" << '\n';

		Protocol::S_MOVE snapbackPkt;
		snapbackPkt.set_correction(true);
		snapbackPkt.add_infos()->CopyFrom(*player->posInfo);

		SEND_PACKET_DECLARATION(snapbackPkt);
		if (auto session = player->session.lock())
			session->Send(sendBuffer);

		return;
	}

	// 서버 이동 검증
	const uint64 nowUs = Utils::NowMicroseconds();
	bool rejected = (player->lastMoveUs == 0);

	if (rejected == false)
	{
		const double deltaSec = static_cast<double>(nowUs - player->lastMoveUs) / 1000000.0;
		player->lastMoveUs = nowUs;

		const double ceiling = player->GetSpeedCeiling(nowUs) * VALIDATION_MARGIN;

		player->moveBudget += ceiling * deltaSec;

		const double maxBudget = ceiling * MAX_BUDGET_SEC;
		if (player->moveBudget > maxBudget)
			player->moveBudget = maxBudget;

		// XY 평면 거리만 본다.
		const double dx = static_cast<double>(pkt.info().x()) - player->posInfo->x();
		const double dy = static_cast<double>(pkt.info().y()) - player->posInfo->y();
		const double distance = std::sqrt(dx * dx + dy * dy);

		if (distance > player->moveBudget)
		{
			rejected = true;

			LOG_LINE << "[MOVE REJECT] id=" << objectId
				<< " dist=" << distance
				<< " budget=" << player->moveBudget
				<< " dt=" << deltaSec << '\n';
		}
		else
		{
			player->moveBudget -= distance;
		}
	}
	
	if (rejected)
	{
		GStats.AddMoveReject();

		// 검증 실패 - posInfo를 갱신하지 않고 마지막 유효 위치를 되돌려 보낸다.
			// 브로캐스트 X
		Protocol::S_MOVE snapbackPkt;
		snapbackPkt.set_correction(true);
		snapbackPkt.add_infos()->CopyFrom(*player->posInfo);

		SEND_PACKET_DECLARATION(snapbackPkt);
		if (auto session = player->session.lock())
			session->Send(sendBuffer);
		
		return;
	}

	if (player->IsCasting())
	{
		const float castDx = pkt.info().x() - player->posInfo->x();
		const float castDy = pkt.info().y() - player->posInfo->y();

		if ((castDx * castDx + castDy * castDy) > CAST_MOVE_EPSILON_SQ)
		{
			const SkillDef* def = SkillTable::Find(player->castSkillId);
			if (def != nullptr && def->canMoveWhileCasting == false)
				CancelCast(player);
		}
	}

	// 검증 통과, 이동
	player->posInfo->CopyFrom(pkt.info());
	_dirtyMovers.insert(objectId);
}

void Room::HandleAttack(uint64 attackerId, uint64 targetId)
{
	auto attackerIt = _objects.find(attackerId);
	if (attackerIt == _objects.end())
		return;

	CreatureRef attacker = dynamic_pointer_cast<Creature>(attackerIt->second);
	if (attacker == nullptr || attacker->IsAlive() == false)
		return;

	if (attacker->CanAttack(Utils::NowMicroseconds()) == false)
		return;

	if (attacker->IsCasting())
		return;

	if (attackerId == targetId)
		return;

	if (attacker->attackHitAtUs != 0)
		return;

	const uint64 nowUs = Utils::NowMicroseconds();

	if (nowUs < attacker->attackReadyAtUs)
		return;

	auto targetIt = _objects.find(targetId);
	if (targetIt == _objects.end())
		return;

	CreatureRef target = dynamic_pointer_cast<Creature>(targetIt->second);
	if (target == nullptr || target->IsAlive() == false)
		return;

	// 대상 락 시점에도 사거리를 본다. 히트 시점에 한 번 더 재검증
	if (IsInAttackRange(attacker, target) == false)
		return;

	const uint32 windupMs = attacker->GetAttackWindupMs();

	attacker->attackTargetId = targetId;
	attacker->attackHitAtUs = nowUs + static_cast<uint64>(windupMs) * 1000;
	_attackers.insert(attackerId);

	Protocol::AttackInfo& info = _pendingAttacks.emplace_back();
	info.set_attacker_id(attackerId);
	info.set_target_id(targetId);
	info.set_windup_ms(windupMs);
}

void Room::HandleSkill(uint64 casterId, Protocol::C_SKILL pkt)
{
	auto casterIt = _objects.find(casterId);
	if (casterIt == _objects.end())
		return;

	CreatureRef caster = dynamic_pointer_cast<Creature>(casterIt->second);
	if (caster == nullptr || caster->IsAlive() == false)
		return;

	SkillSlot* skillSlot = caster->GetSlot(pkt.slot());
	if (skillSlot == nullptr)
		return;

	if (skillSlot->bindState != Protocol::BIND_BOUND)
		return;

	const SkillDef* def = SkillTable::Find(skillSlot->skillId);
	if (def == nullptr)
		return;

	const uint64 nowUs = Utils::NowMicroseconds();

	if (nowUs < skillSlot->cooldownEndUs)
		return;

	if (caster->CanUseSkill(nowUs) == false)
		return;

	if (caster->IsCasting())
		return;

	if (def->shape == SkillShape::Single)
	{
		if (casterId == pkt.target_id())
			return;

		auto targetIt = _objects.find(pkt.target_id());
		if (targetIt == _objects.end())
			return;

		CreatureRef target = dynamic_pointer_cast<Creature>(targetIt->second);
		if (target == nullptr || target->IsAlive() == false)
			return;

		if (IsInSkillRange(caster, target, def->rangeCm) == false)
			return;
	}

	if (caster->attackHitAtUs != 0)
	{
		caster->attackTargetId = 0;
		caster->attackHitAtUs = 0;
		_attackers.erase(casterId);
		_pendingAttackCancels.push_back(casterId);
	}

	caster->castSkillId = def->skillId;
	caster->castSlot = pkt.slot();
	caster->castTargetId = pkt.target_id();
	caster->castAimX = pkt.aim_x();
	caster->castAimY = pkt.aim_y();
	caster->castEndAtUs = nowUs + static_cast<uint64>(def->castMs) * 1000;
	_casters.insert(casterId);

	if (def->castMs > 0)
	{
		Protocol::SkillCastInfo& info = _pendingCasts.emplace_back();
		info.set_caster_id(casterId);
		info.set_is_stationary(def->canMoveWhileCasting == false);
	}
}

void Room::HandleSkillCancel(uint64 casterId)
{
	auto findIt = _objects.find(casterId);
	if (findIt == _objects.end())
		return;

	CancelCast(dynamic_pointer_cast<Creature>(findIt->second));
}

void Room::ApplyCc(uint64 targetId, uint64 instigatorId, Protocol::CcType type, uint32 baseDurationMs, float magnitude)
{
	auto findIt = _objects.find(targetId);
	if (findIt == _objects.end())
		return;

	CreatureRef target = dynamic_pointer_cast<Creature>(findIt->second);
	if (target == nullptr || target->IsAlive() == false)
		return;

	const uint64 nowUs = Utils::NowMicroseconds();

	const uint32 effectiveMs = target->ComputeCcDurationMs(baseDurationMs);
	if (effectiveMs == 0)
		return;

	const bool isHard = IsHardCcType(type);

	if (isHard)
		target->ApplyHardCc(type, nowUs, effectiveMs);
	else
		target->ApplySoftCc(type, nowUs, effectiveMs, magnitude);

	_ccTargets.insert(targetId);

	Protocol::CcEventInfo& info = _pendingCcApplied.emplace_back();
	info.set_target_id(targetId);
	info.set_instigator_id(instigatorId);
	info.set_cc_type(type);
	info.set_duration_ms(effectiveMs);
	info.set_magnitude(magnitude);

	// 하드CC는 진행 중인 평타를 즉시 취소
	if (isHard && target->attackHitAtUs != 0)
	{
		target->attackTargetId = 0;
		target->attackHitAtUs = 0;
		_attackers.erase(targetId);
		_pendingAttackCancels.push_back(targetId);
	}

	if (isHard || type == Protocol::CC_TYPE_SILENCE)
		CancelCast(target);
}

void Room::ResolveAttacks(uint64 nowUs)
{
	if (_attackers.empty())
		return;

	vector<uint64> resolved;

	for (uint64 attackerId : _attackers)
	{
		auto attackerIt = _objects.find(attackerId);
		if (attackerIt == _objects.end())
		{
			resolved.push_back(attackerId);	// 그 사이 방을 나간 경우
			continue;
		}

		CreatureRef attacker = dynamic_pointer_cast<Creature>(attackerIt->second);
		if (attacker == nullptr)
		{
			resolved.push_back(attackerId);
			continue;
		}

		if (attacker->attackHitAtUs == 0 || nowUs < attacker->attackHitAtUs)
			continue;

		resolved.push_back(attackerId);

		const uint64 targetId = attacker->attackTargetId;
		attacker->attackTargetId = 0;
		attacker->attackHitAtUs = 0;

		CreatureRef target = nullptr;
		auto targetIt = _objects.find(targetId);
		if (targetIt != _objects.end())
			target = dynamic_pointer_cast<Creature>(targetIt->second);


		// 공격자 사망, 대상 소명, 대상 사망, 사거리 이탈-> 취소. 쿨다운을 소모 하지 않는다.
		if (attacker->IsAlive() == false
			|| target == nullptr
			|| target->IsAlive() == false
			|| IsInAttackRange(attacker, target) == false)
		{
			_pendingAttackCancels.push_back(attackerId);
			continue;
		}

		bool isCrit = false;
		const int32 damage = attacker->ComputeDamageTo(*target, Protocol::DAMAGE_TYPE_PHYSICAL, OUT isCrit);
		const int32 remainingHp = target->TakeDamage(damage);

		Protocol::DamageInfo& dmg = _pendingDamages.emplace_back();
		dmg.set_attacker_id(attackerId);
		dmg.set_target_id(targetId);
		dmg.set_damage(damage);
		dmg.set_remaining_hp(remainingHp);
		dmg.set_is_crit(isCrit);
		dmg.set_damage_type(Protocol::DAMAGE_TYPE_PHYSICAL);

		// 후딜 소모는 히트에 성공했을 때만
		attacker->attackReadyAtUs = nowUs + static_cast<uint64>(attacker->GetAttackRecoveryMs()) * 1000;

		if (remainingHp <= 0)
		{
			Protocol::DiedInfo& died = _pendingDeaths.emplace_back();
			died.set_victim_id(targetId);
			died.set_instigator_id(attackerId);
			died.set_cause(target->IsPlayer()
				? Protocol::DEATH_CAUSE_PLAYER
				: Protocol::DEATH_CAUSE_MONSTER);

			// 죽은 대상이 공격 중이었다면 그 공격도 없앤다.
			target->attackTargetId = 0;
			target->attackHitAtUs = 0;

			CancelCast(target);

			LOG_LINE << "[DIED] victim=" << targetId << " by=" << attackerId << '\n';
			BeginDeathCommit(target);
		}
	}

	for (uint64 id : resolved)
		_attackers.erase(id);
}

void Room::ResolveCasts(uint64 nowUs)
{
	if (_casters.empty())
		return;

	vector<uint64> resolved;

	for (uint64 casterId : _casters)
	{
		auto casterIt = _objects.find(casterId);
		if (casterIt == _objects.end())
		{
			resolved.push_back(casterId);
			continue;
		}

		CreatureRef caster = dynamic_pointer_cast<Creature>(casterIt->second);
		if (caster == nullptr)
		{
			resolved.push_back(casterId);
			continue;
		}

		if (caster->castEndAtUs == 0 || nowUs < caster->castEndAtUs)
			continue;

		resolved.push_back(casterId);

		const uint32 skillId = caster->castSkillId;
		const Protocol::EquipSlot slot = caster->castSlot;

		// 조준값을 지우기 전에 저장
		const uint64 castTargetId = caster->castTargetId;
		const float castAimX = caster->castAimX;
		const float castAimY = caster->castAimY;

		// 캐스트 상태 해제
		caster->castEndAtUs = 0;
		caster->castSkillId = 0;
		caster->castSlot = Protocol::SLOT_NONE;
		caster->castTargetId = 0;

		const SkillDef* def = SkillTable::Find(skillId);
		if (def == nullptr)
			continue;

		// 효과 적용 시점
		if (SkillSlot* skillSlot = caster->GetSlot(slot))
			skillSlot->cooldownEndUs = nowUs + static_cast<uint64>(def->cooldownMs) * 1000;

		const float originX = caster->posInfo->x();
		const float originY = caster->posInfo->y();

		{
			Protocol::SkillHitInfo& hit = _pendingSkillHits.emplace_back();
			hit.set_caster_id(casterId);
			hit.set_effect_id(def->effectId);
			hit.set_target_id(0);
			hit.set_impact_x(originX);
			hit.set_impact_y(originY);
		}

		if (def->telegraphMs == 0)
		{
			vector<CreatureRef> targets;
			CollectSkillTargets(caster, *def, originX, originY, castTargetId, castAimX, castAimY, OUT targets);
			ApplySkillEffects(caster, *def, targets, nowUs);

			LOG_LINE << "[SKILL FIRE] caster=" << casterId
				<< " skill=" << skillId
				<< " targets=" << targets.size()
				<< " cd=" << def->cooldownMs << "ms" << '\n';
		}
		else
		{
			PendingTelegraph& pending = _pendingTelegraphs.emplace_back();
			pending.casterId = casterId;
			pending.skillId = skillId;
			pending.fireAtUs = nowUs + static_cast<uint64>(def->telegraphMs) * 1000;
			pending.targetId = castTargetId;
			pending.originX = originX;
			pending.originY = originY;

			LOG_LINE << "[SKILL TELEGRAPH] caster=" << casterId
				<< " skill=" << skillId
				<< " in=" << def->telegraphMs << "ms" << '\n';
		}
	}

	for (uint64 id : resolved)
		_casters.erase(id);
}

void Room::ResolveTelegraphs(uint64 nowUs)
{
	if (_pendingTelegraphs.empty())
		return;

	for (int32 i = static_cast<int32>(_pendingTelegraphs.size()) - 1; i >= 0; i--)
	{
		PendingTelegraph& pending = _pendingTelegraphs[i];

		if (nowUs < pending.fireAtUs)
			continue;

		const uint32 skillId = pending.skillId;
		const uint64 casterId = pending.casterId;
		const float originX = pending.originX;
		const float originY = pending.originY;
		const uint64 targetId = pending.targetId;
		const float aimX = pending.aimX;
		const float aimY = pending.aimY;

		_pendingTelegraphs.erase(_pendingTelegraphs.begin() + i);

		const SkillDef* def = SkillTable::Find(skillId);
		if (def == nullptr)
			continue;

		auto casterIt = _objects.find(casterId);
		if (casterIt == _objects.end())
			continue;

		CreatureRef caster = dynamic_pointer_cast<Creature>(casterIt->second);

		if (caster == nullptr || caster->IsAlive() == false)
			continue;

		vector<CreatureRef> targets;
		CollectSkillTargets(caster, *def, originX, originY, targetId, aimX, aimY, OUT targets);
		ApplySkillEffects(caster, *def, targets, nowUs);

		LOG_LINE << "[SKILL FIRE] caster=" << casterId
			<< " skill=" << skillId
			<< " targets=" << targets.size()
			<< " (telegraphed)" << '\n';
	}
}

void Room::UpdateCc(uint64 nowUs)
{
	if (_ccTargets.empty())
		return;

	vector<uint64> cleared;

	for (uint64 targetId : _ccTargets)
	{
		auto findIt = _objects.find(targetId);
		if (findIt == _objects.end())
		{
			cleared.push_back(targetId);
			continue;
		}

		CreatureRef target = dynamic_pointer_cast<Creature>(findIt->second);
		if (target == nullptr)
		{
			cleared.push_back(targetId);
			continue;
		}

		auto expire = [&](uint64& endUs, Protocol::CcType type)
			{
				if (endUs == 0 || nowUs < endUs)
					return;

				endUs = 0;

				Protocol::CcEventInfo& info = _pendingCcExpired.emplace_back();
				info.set_target_id(targetId);
				info.set_cc_type(type);
			};

		expire(target->stunEndUs, Protocol::CC_TYPE_STUN);
		expire(target->rootEndUs, Protocol::CC_TYPE_ROOT);
		expire(target->knockbackEndUs, Protocol::CC_TYPE_KNOCKBACK);
		expire(target->launchEndUs, Protocol::CC_TYPE_LAUNCH);
		expire(target->silenceEndUs, Protocol::CC_TYPE_SILENCE);
		expire(target->healReductionEndUs, Protocol::CC_TYPE_HEAL_REDUCTION);

		if (target->healReductionEndUs == 0)
			target->healReductionMagnitude = 0.f;

		const bool hadSlow = target->slowSources.empty() == false;
		target->RefreshActiveSlow(nowUs);

		if (hadSlow && target->slowSources.empty())
		{
			Protocol::CcEventInfo& info = _pendingCcExpired.emplace_back();
			info.set_target_id(targetId);
			info.set_cc_type(Protocol::CC_TYPE_SLOW);
		}

		const bool stillCced = target->stunEndUs || target->rootEndUs
			|| target->knockbackEndUs || target->launchEndUs
			|| target->silenceEndUs || target->healReductionEndUs
			|| target->slowSources.empty() == false;

		if (stillCced == false)
			cleared.push_back(targetId);
	}
	
	for (uint64 id : cleared)
		_ccTargets.erase(id);
}

void Room::FlushCcState(uint64 nowUs)
{
	if (_ccTargets.empty())
		return;

	Protocol::S_CC_STATE pkt;

	for (uint64 targetId : _ccTargets)
	{
		auto findIt = _objects.find(targetId);
		if (findIt == _objects.end())
			continue;

		CreatureRef target = dynamic_pointer_cast<Creature>(findIt->second);
		if (target == nullptr)
			continue;

		Protocol::CcStateInfo* state = pkt.add_states();
		state->set_target_id(targetId);
		state->set_active_slow(target->activeSlow);

		auto addSlot = [&](uint64 endUs, Protocol::CcType type, float magnitude)
			{
				if (endUs == 0 || nowUs >= endUs)
					return;

				Protocol::CcSlot* slot = state->add_slots();
				slot->set_cc_type(type);
				slot->set_remaining_ms(static_cast<uint32>((endUs - nowUs) / 1000));
				slot->set_magnitude(magnitude);
			};

		addSlot(target->stunEndUs,			Protocol::CC_TYPE_STUN,				0.f);
		addSlot(target->rootEndUs,			Protocol::CC_TYPE_ROOT,				0.f);
		addSlot(target->knockbackEndUs,		Protocol::CC_TYPE_KNOCKBACK,		0.f);
		addSlot(target->launchEndUs,		Protocol::CC_TYPE_LAUNCH,			0.f);
		addSlot(target->silenceEndUs,		Protocol::CC_TYPE_SILENCE,			0.f);
		addSlot(target->healReductionEndUs,	Protocol::CC_TYPE_HEAL_REDUCTION,	target->healReductionMagnitude);
		
		uint64 slowEndUs = 0;
		for (const SlowSource& src : target->slowSources)
		{
			if (src.endUs > slowEndUs)
				slowEndUs = src.endUs;
		}

		addSlot(slowEndUs, Protocol::CC_TYPE_SLOW, target->activeSlow);
	}

	if (pkt.states_size() > 0)
	{
		SEND_PACKET_BROADCAST(pkt);
	}
}

void Room::HandleEquip(GameSessionRef session, string requestId, uint64 instanceId, Protocol::EquipSlot slot)
{
	PlayerRef player = session->player.load();
	if (player == nullptr)
		return;

	if (slot < Protocol::SLOT_WEAPON_PRIMARY || slot > Protocol::SLOT_TRINKET)
	{
		session->SendItemResult(requestId, Protocol::ITEM_BAD_REQUEST, {});
		return;
	}

	if (slot == Protocol::SLOT_WEAPON_SECONDARY)
	{
		session->SendItemResult(requestId, Protocol::ITEM_INVALID_STATE, {});
		return;
	}

	ItemEntry* found = player->FindInInventory(instanceId);
	if (found == nullptr)
	{
		session->SendItemResult(requestId, Protocol::ITEM_NOT_FOUND, {});
		return;
	}

	const int32 idx = slot - 1;

	const size_t invIndex = static_cast<size_t>(found - player->inventory.data());
	ItemEntry incoming = *found;
	ItemEntry outgoing = player->equipped[idx];

	player->inventory.erase(player->inventory.begin() + invIndex);
	player->equipped[idx] = incoming;

	vector<Protocol::ItemInstance> changed;
	changed.push_back(incoming.ToProto(Protocol::ITEM_STATE_EQUIPPED, slot));

	if (outgoing.instanceId != 0)
	{
		player->inventory.push_back(outgoing);
		changed.push_back(outgoing.ToProto(Protocol::ITEM_STATE_CARRIED));
	}

	player->RebuildSkillSlotsFromEquipment();
	LogSlots(player, "equip");
	SendEquipSync(player);

	session->SendItemResult(requestId, Protocol::ITEM_OK, changed);
}

void Room::HandleUnequip(GameSessionRef session, string requestId, uint64 instanceId)
{
	PlayerRef player = session->player.load();
	if (player == nullptr)
		return;

	int32 foundIdx = -1;
	for (int32 i = 0; i < SKILL_SLOT_COUNT; i++)
	{
		if (player->equipped[i].instanceId == instanceId)
		{
			foundIdx = i;
			break;
		}
	}

	if (foundIdx < 0)
	{
		session->SendItemResult(requestId, Protocol::ITEM_NOT_FOUND);
		return;
	}

	if (player->IsInventoryFull())
	{
		session->SendItemResult(requestId, Protocol::ITEM_INVENTORY_FULL);
		return;
	}

	ItemEntry e = player->equipped[foundIdx];
	player->equipped[foundIdx] = ItemEntry{};
	player->inventory.push_back(e);

	player->RebuildSkillSlotsFromEquipment();
	LogSlots(player, "unequip");
	SendEquipSync(player);

	vector<Protocol::ItemInstance> changed;
	changed.push_back(e.ToProto(Protocol::ITEM_STATE_CARRIED));

	session->SendItemResult(requestId, Protocol::ITEM_OK, changed);
}

void Room::HandleBagOpen(GameSessionRef session, uint64 bagId)
{
	PlayerRef player = session->player.load();
	if (player == nullptr)
		return;

	const uint64 playerId = player->objectInfo->object_id();

	if (_objects.find(playerId) == _objects.end() || player->IsAlive() == false)
		return;

	auto findIt = _bags.find(bagId);
	if (findIt == _bags.end())
	{
		LOG_LINE << "[LOOT] open rejected - char=" << playerId << " bag=" << bagId << " reason=not_found\n";
		return;
	}

	BagRef bag = findIt->second;
	if (IsInLootRange(player, bag) == false)
	{
		LOG_LINE << "[LOOT] open rejected - char=" << playerId << " bag=" << bagId << " reason=range\n";
		return;
	}

	bag->viewers.insert(playerId);
	session->Send(MakeBagContents(bag, Utils::NowMicroseconds()));

	LOG_LINE << "[LOOT] open - char=" << playerId << " bag=" << bagId << " items=" << bag->items.size() << '\n';
}

void Room::HandleLoot(GameSessionRef session, string requestId, uint64 bagId, uint64 instanceId)
{
	PlayerRef player = session->player.load();
	if (player == nullptr)
		return;

	const uint64 playerId = player->objectInfo->object_id();

	if (_objects.find(playerId) == _objects.end() || player->IsAlive() == false)
	{
		session->SendItemResult(requestId, Protocol::ITEM_INVALID_STATE);
		return;
	}

	auto bagIt = _bags.find(bagId);
	if (bagIt == _bags.end())
	{
		session->SendItemResult(requestId, Protocol::ITEM_NOT_FOUND);
		return;
	}

	BagRef bag = bagIt->second;
	if (IsInLootRange(player, bag) == false)
	{
		session->SendItemResult(requestId, Protocol::ITEM_INVALID_STATE);
		return;
	}

	auto itemIt = find_if(bag->items.begin(), bag->items.end(),
		[instanceId](const ItemEntry& e) {return e.instanceId == instanceId; });

	if (itemIt == bag->items.end())
	{
		session->SendItemResult(requestId, Protocol::ITEM_NOT_FOUND);
		return;
	}

	// 부분 루팅 - 거부된 아이템은 가방에 남는다.
	if (player->IsInventoryFull())
	{
		session->SendItemResult(requestId, Protocol::ITEM_INVENTORY_FULL);
		return;
	}

	// 메모리만 옮긴다. DB의 이 아이템은 탈출,사망 커밋 때까지 원래 가방 소속이다.
	ItemEntry e = *itemIt;
	bag->items.erase(itemIt);
	player->inventory.push_back(e);

	vector<Protocol::ItemInstance> changed;
	changed.push_back(e.ToProto(Protocol::ITEM_STATE_CARRIED));
	session->SendItemResult(requestId, Protocol::ITEM_OK, changed);

	// 비어도 지우지 않는다.
	PushBagContents(bag);

	LOG_LINE << "[LOOT] take - char=" << playerId << " bag=" << bagId << " item=" << instanceId << " left=" << bag->items.size() << " inv=" << player->inventory.size() << '\n';
}

void Room::HandleDrop(GameSessionRef session, string requestId, uint64 instanceId)
{
	PlayerRef player = session->player.load();
	if (player == nullptr)
		return;

	const uint64 playerId = player->objectInfo->object_id();

	// 죽은 뒤의 소지품은 사망 커밋이 가방으로 옮긴다.
	if (_objects.find(playerId) == _objects.end() || player->IsAlive() == false)
	{
		session->SendItemResult(requestId, Protocol::ITEM_INVALID_STATE);
		return;
	}

	// 착용 중인 아이템은 해제부터 해야한다.
	for (const ItemEntry& e : player->equipped)
	{
		if (e.instanceId != 0 && e.instanceId == instanceId)
		{
			session->SendItemResult(requestId, Protocol::ITEM_INVALID_STATE);
			return;
		}
	}

	ItemEntry* found = player->FindInInventory(instanceId);
	if (found == nullptr)
	{
		session->SendItemResult(requestId, Protocol::ITEM_NOT_FOUND);
		return;
	}

	const size_t invIndex = static_cast<size_t>(found - player->inventory.data());
	const ItemEntry e = *found;

	player->inventory.erase(player->inventory.begin() + invIndex);
	player->droppedIds.push_back(e.instanceId);

	vector<Protocol::ItemInstance> changed;
	changed.push_back(e.ToProto(Protocol::ITEM_STATE_NONE));
	session->SendItemResult(requestId, Protocol::ITEM_OK, changed);

	LOG_LINE << "[ITEM] drop - char=" << playerId << " item=" << e.instanceId
		<< " inv=" << player->inventory.size() << " dropped=" << player->droppedIds.size() << '\n';
}

bool Room::AddBag(BagRef bag)
{
	if (bag == nullptr)
		return false;

	if (AddObject(bag) == false)
		return false;

	_bags[bag->objectInfo->object_id()] = bag;
	_pendingSpawns.push_back(bag->objectInfo->object_id());

	LOG_LINE << "[BAG] add id=" << bag->objectInfo->object_id()
		<< " floor=" << _floorId
		<< " pos=(" << bag->posInfo->x() << "," << bag->posInfo->y() << ")"
		<< " owner=" << bag->ownerCharacterId << '\n';

	return true;
}

bool Room::RemoveBag(uint64 bagId)
{
	auto findIt = _objects.find(bagId);
	if (findIt == _objects.end() || findIt->second->IsBag() == false)
		return false;

	RemoveObject(bagId);
	_bags.erase(bagId);

	// 스폰 전에 지워졌다면(33ms 안) FlushSpawns가 _objects에서 못 찾아 건너뛴다. 이 DESPAWN은 무해하다.
	Protocol::S_DESPAWN despawnPkt;
	despawnPkt.add_object_ids(bagId);

	SEND_PACKET_DECLARATION(despawnPkt);
	Broadcast(sendBuffer);

	LOG_LINE << "[BAG] remove id=" << bagId << '\n';

	return true;
}

void Room::DebugDropBag(uint64 playerId)
{
	// 위치를 Room 스레드에서 읽는다. IOCP 스레드에서 posInfo를 읽으면 HandleMove와 경쟁한다.
	auto findIt = _objects.find(playerId);
	if (findIt == _objects.end())
		return;

	PlayerRef owner = dynamic_pointer_cast<Player>(findIt->second);
	if (owner == nullptr)
		return;

	const char* place = "?";
	if (auto session = owner->session.lock())
		place = (session->place.load() == SessionPlace::Dungeon) ? "Dungeon" : "NOT_Dungeon";

	const uint64 bagId = GNextBagId.fetch_add(1);
	const uint64 expiresAtUs = Utils::NowMicroseconds() + 60'000'000;

	BagRef bag = ObjectUtils::CreateBag(bagId, _floorId, owner->posInfo->x(), owner->posInfo->y(), owner->posInfo->z(), expiresAtUs, playerId);

	LOG_LINE << "[BAG] debug drop by=" << playerId << " place=" << place << '\n';

	AddBag(bag);
}

void Room::DebugKill(uint64 playerId)
{
	auto findIt = _objects.find(playerId);
	if (findIt == _objects.end())
		return;

	PlayerRef victim = dynamic_pointer_cast<Player>(findIt->second);
	if (victim == nullptr || victim->IsAlive() == false)
		return;

	victim->hp = 0;

	KillPlayer(victim, playerId, Protocol::DEATH_CAUSE_PLAYER, "debug");
}

void Room::BeginDeathCommit(const CreatureRef& victim)
{
	PlayerRef player = dynamic_pointer_cast<Player>(victim);
	if (player == nullptr)
		return;

	const uint64 characterId = player->objectInfo->object_id();

	DeathSnapshot snap;
	snap.characterId = characterId;
	snap.floorId = _floorId;
	snap.x = player->posInfo->x();
	snap.y = player->posInfo->y();
	snap.z = player->posInfo->z();

	for (const ItemEntry& e : player->inventory)
		snap.items.push_back(e);

	for (const ItemEntry& e : player->equipped)
		if (e.instanceId != 0)
			snap.items.push_back(e);

	snap.droppedIds = player->droppedIds;

	if (GDeathCommits.TryBegin(characterId) == false)
		return;

	snap.bagId = GNextBagId.fetch_add(1);
	snap.expiresAtUs = Utils::NowMicroseconds() + BAG_LIFETIME_US;

	LOG_LINE << "[DEATH] commit begin - char=" << characterId << " bag=" << snap.bagId << " items=" << snap.items.size() << " dropped=" << snap.droppedIds.size() << '\n';

	RoomRef room = GetRoomRef();
	GDBQueue.Push([room, snap](DBConnection* conn)
		{
			const bool ok = CommitDeath(conn, snap);
			room->DoAsync(&Room::OnBagCreated, snap, ok);
		});
}

void Room::OnBagCreated(DeathSnapshot snap, bool ok)
{
	// 성공이든 실패든 재입장을 허용한다. DB는 이미 확정(커밋 또는 롤백)됐다.
	GDeathCommits.End(snap.characterId);

	// 끊긴 몸이 죽었다 - 커밋이 됐든 안 됐든 몸을 치운다. 다음 접속은 DB에서 새로 적재한다.
	ReleaseDetachedBody(snap.characterId);

	if (ok == false)
	{
		LOG_LINE << "[DEATH] commit failed - no bag. char=" << snap.characterId << '\n';
		return;
	}

	BagRef bag = ObjectUtils::CreateBag(snap.bagId, snap.floorId, snap.x, snap.y, snap.z, snap.expiresAtUs, snap.characterId);
	bag->items = snap.items;

	for (ItemEntry& e : bag->items)
		e.level = 0;

	AddBag(bag);

	// 죽은 플레이어가 아직 방에 있으면 메모리도 비운다. 봇은 이미 끊고 나갔을 수 있다.
	auto findIt = _objects.find(snap.characterId);
	if (findIt == _objects.end())
		return;

	PlayerRef player = dynamic_pointer_cast<Player>(findIt->second);
	if (player == nullptr)
		return;

	player->inventory.clear();
	player->droppedIds.clear();
	for (ItemEntry& e : player->equipped)
		e = ItemEntry{};

	player->RebuildSkillSlotsFromEquipment();
	SendEquipSync(player);

	Protocol::S_INVENTORY_SYNC invPkt;		// 빈 목록, 전부 가방으로 감
	SEND_PACKET_DECLARATION(invPkt);
	if (auto session = player->session.lock())
		session->Send(sendBuffer);
}

void Room::BeginEscapeCommit(uint64 playerId)
{
	auto findIt = _objects.find(playerId);
	if (findIt == _objects.end())
		return;

	PlayerRef player = dynamic_pointer_cast<Player>(findIt->second);
	if (player == nullptr || player->IsAlive() == false)
		return;

	GameSessionRef session = player->session.lock();
	if (session == nullptr)
		return;

	if (GDeathCommits.TryBegin(playerId) == false)
		return;

	EscapeSnapshot snap;
	snap.characterId = playerId;
	snap.carried = player->inventory;
	for (int32 i = 0; i < SKILL_SLOT_COUNT; i++)
		snap.equipped[i] = player->equipped[i];
	snap.droppedIds = player->droppedIds;

	// 방에서 뺀다 - 이 순간부터 공격, CC, 조작의 대상이 아니다(무적대기).
	// 베이스캠프로는 커밋이 끝난 뒤에 보낸다. - 보낸 뒤 되돌리는 것이 가장 나쁘다.
	CancelCast(player);
	SavePlayer(player);
	RemoveObject(playerId);
	session->place.store(SessionPlace::Escaping);

	Protocol::S_DESPAWN despawnPkt;
	despawnPkt.add_object_ids(playerId);
	SEND_PACKET_DECLARATION(despawnPkt);
	Broadcast(sendBuffer);

	LOG_LINE << "[ESCAPE] commit begin - char=" << playerId << " carried=" << snap.carried.size()
		<< " dropped=" << snap.droppedIds.size() << '\n';

	GDBQueue.Push([session, snap](DBConnection* conn)
		{
			// 성공할 때까지 재시도. 돌아왔으면 커밋된 것
			CommitEscape(conn, snap);
			session->place.store(SessionPlace::BaseCamp);
			GDeathCommits.End(snap.characterId);

			Protocol::S_LEAVE_GAME leavePkt;
			SEND_PACKET_DECLARATION(leavePkt);
			session->Send(sendBuffer);

			LOG_LINE << "[ESCAPE] committed - char=" << snap.characterId << '\n';
		});
}

void Room::HandleDisconnect(PlayerRef player)
{
	const uint64 playerId = player->objectInfo->object_id();

	// 방에 없거나 이미 죽었으면 남길 몸이 없다
	if (_objects.find(playerId) == _objects.end() || player->IsAlive() == false)
	{
		LeaveRoom(player);
		return;
	}

	// 몸을 남긴다 - 공격, CC는 계속 받는다. 유예 안에 다시 돌아오면 이 몸으로 돌아온다.
	player->attackTargetId = 0;
	player->attackHitAtUs = 0;
	CancelCast(player);
	player->session.reset();
	player->detachedUntilUs = Utils::NowMicroseconds() + GDisconnectGraceUs.load();

	_detached.insert(playerId);
	GDetachedBodies.Add(playerId, player);

	LOG_LINE << "[DETACH] char=" << playerId << " grace=" << GDisconnectGraceUs.load() / 1000 << "ms\n";
}

void Room::HandleReattach(GameSessionRef session, PlayerRef body)
{
	const uint64 playerId = body->objectInfo->object_id();

	// 표에서 꺼낸 뒤 이 Job이 오기 전에 죽었다 - 사망 커밋이 몸을 지운다. 클라는 잠시 뒤 다시 돌아온다.
	if (_objects.find(playerId) == _objects.end() || body->IsAlive() == false)
	{
		LOG_LINE << "[REATTACH] rejected - char=" << playerId << " (dead or gone)\n";

		SessionPlace expected = SessionPlace::Entering;
		session->place.compare_exchange_strong(expected, SessionPlace::BaseCamp);

		Protocol::S_ENTER_GAME rejectPkt;
		rejectPkt.set_success(false);
		SEND_PACKET_DECLARATION(rejectPkt);
		session->Send(sendBuffer);
		return;
	}

	_detached.erase(playerId);
	body->detachedUntilUs = 0;
	body->session = session;
	session->player.store(body);
	session->place.store(SessionPlace::Dungeon);

	// 끊긴 동안의 시간이 이동 예산으로 쌓이지 않게 지금을 기준으로 다시 잡는다.
	const uint64 nowUs = Utils::NowMicroseconds();
	body->lastMoveUs = nowUs;
	body->moveBudget = body->GetSpeedCeiling(nowUs) * VALIDATION_MARGIN * MAX_BUDGET_SEC;

	Protocol::S_ENTER_GAME enterPkt;
	enterPkt.set_success(true);
	enterPkt.mutable_player()->CopyFrom(*body->objectInfo);
	SEND_PACKET_DECLARATION(enterPkt);
	session->Send(sendBuffer);

	SendEquipSync(body);
	SendInventorySync(body);

	// 방 전체 스냅샷을 다시 받게 한다. 다른 클라에는 이미 있는 몸이라 S_SPAWN 은 무시한다.
	_pendingSpawns.push_back(playerId);

	LOG_LINE << "[REATTACH] char=" << playerId << " inv=" << body->inventory.size() << " dropped=" << body->droppedIds.size() << '\n';
}

void Room::UpdateTick()
{
	const uint64 nowUs = Utils::NowMicroseconds();

	UpdateSaves(nowUs);
	UpdateBagExpiry(nowUs);
	UpdateDetached(nowUs);
	
	// 0.1초 경과했으면, UpdateTick()
	DoTimer(100, &Room::UpdateTick);
}

void Room::FlushMoves()
{
	if (_dirtyMovers.empty() == false)
	{
		Protocol::S_MOVE movePkt;

		for (uint64 objectId : _dirtyMovers)
		{
			auto findIt = _objects.find(objectId);
			if (findIt == _objects.end())
				continue;

			movePkt.add_infos()->CopyFrom(*findIt->second->posInfo);
		}

		_dirtyMovers.clear();

		if (movePkt.infos_size() > 0)
		{
			SendBufferRef sendBuffer = ServerPacketHandler::MakeSendBuffer(movePkt);
			Broadcast(sendBuffer);
		}
	}

	DoTimer(MOVE_FLUSH_MS, &Room::FlushMoves);
}

void Room::FlushCombat()
{
	const uint64 nowUs = Utils::NowMicroseconds();

	UpdateCc(nowUs);
	ResolveAttacks(nowUs);
	ResolveCasts(nowUs);
	ResolveTelegraphs(nowUs);

	if (_pendingAttacks.empty() == false)
	{
		Protocol::S_ATTACK pkt;
		for (const Protocol::AttackInfo& info : _pendingAttacks)
			pkt.add_attacks()->CopyFrom(info);
		_pendingAttacks.clear();

		SEND_PACKET_BROADCAST(pkt);
	}

	if (_pendingAttackCancels.empty() == false)
	{
		Protocol::S_ATTACK_CANCEL pkt;
		for (uint64 id : _pendingAttackCancels)
			pkt.add_attacker_ids(id);
		_pendingAttackCancels.clear();

		SEND_PACKET_BROADCAST(pkt);
	}

	if (_pendingCasts.empty() == false)
	{
		Protocol::S_SKILL_CAST pkt;
		for (const Protocol::SkillCastInfo& info : _pendingCasts)
			pkt.add_casts()->CopyFrom(info);
		_pendingCasts.clear();

		SEND_PACKET_BROADCAST(pkt);
	}

	if (_pendingCastCancels.empty() == false)
	{
		Protocol::S_SKILL_CANCEL pkt;
		for (uint64 id : _pendingCastCancels)
			pkt.add_caster_ids(id);
		_pendingCastCancels.clear();

		SEND_PACKET_BROADCAST(pkt);
	}

	if (_pendingSkillHits.empty() == false)
	{
		Protocol::S_SKILL_HIT pkt;
		for (const Protocol::SkillHitInfo& info : _pendingSkillHits)
			pkt.add_hits()->CopyFrom(info);
		_pendingSkillHits.clear();

		SEND_PACKET_BROADCAST(pkt);
	}

	if (_pendingCcApplied.empty() == false || _pendingCcExpired.empty() == false)
	{
		Protocol::S_CC pkt;

		for (const Protocol::CcEventInfo& info : _pendingCcApplied)
			pkt.add_applied()->CopyFrom(info);
		for (const Protocol::CcEventInfo& info : _pendingCcExpired)
			pkt.add_expired()->CopyFrom(info);

		_pendingCcApplied.clear();
		_pendingCcExpired.clear();

		SEND_PACKET_BROADCAST(pkt);
	}

	if (_pendingDamages.empty() == false)
	{
		Protocol::S_DAMAGE pkt;
		for (const Protocol::DamageInfo& info : _pendingDamages)
			pkt.add_damages()->CopyFrom(info);
		_pendingDamages.clear();

		SEND_PACKET_BROADCAST(pkt);
	}

	if (_pendingDeaths.empty() == false)
	{
		Protocol::S_DIED pkt;
		for (const Protocol::DiedInfo& info : _pendingDeaths)
			pkt.add_deaths()->CopyFrom(info);
		_pendingDeaths.clear();

		SEND_PACKET_BROADCAST(pkt);
	}

	if (++_ccStateTickCounter >= CC_STATE_TICKS)
	{
		_ccStateTickCounter = 0;
		FlushCcState(nowUs);
	}

	DoTimer(COMBAT_FLUSH_MS, &Room::FlushCombat);
}

void Room::FlushSpawns()
{
	if (_pendingSpawns.empty() == false)
	{
		Protocol::S_SPAWN newPkt;
		unordered_set<uint64> newIds;

		for (uint64 id : _pendingSpawns)
		{
			auto findIt = _objects.find(id);
			if (findIt == _objects.end())
				continue;

			newPkt.add_players()->CopyFrom(*findIt->second->objectInfo);
			newIds.insert(id);
		}

		_pendingSpawns.clear();

		if (newIds.empty() == false)
		{
			// 신규 전원을 한 패킷에 담아 기존 전원에게 1회 브로드캐스트
			// 직렬화 1회
			{
				SendBufferRef newBuffer = ServerPacketHandler::MakeSendBuffer(newPkt);
				Broadcast(newBuffer, newIds);
			}

			// 전체 목록을 1회만 직렬화해 신규 전원이 같은 버퍼를 나눠쓴다.
			{
				Protocol::S_SPAWN allPkt;

				for (auto& item : _objects)
				{
					if (item.second->IsPlayer() == false && item.second->IsBag() == false)
						continue;

					allPkt.add_players()->CopyFrom(*item.second->objectInfo);
				}

				if (allPkt.players_size() > 0)
				{
					SendBufferRef allBuffer = ServerPacketHandler::MakeSendBuffer(allPkt);

					for (uint64 id : newIds)
					{
						auto findIt = _objects.find(id);
						if (findIt == _objects.end())
							continue;

						PlayerRef player = dynamic_pointer_cast<Player>(findIt->second);
						if (player == nullptr)
							continue;

						if (auto session = player->session.lock())
							session->Send(allBuffer);
					}
				}
			}
		}
	}
	DoTimer(SPAWN_FLUSH_MS, &Room::FlushSpawns);
}

RoomRef Room::GetRoomRef()
{
	return static_pointer_cast<Room>(shared_from_this());
}

bool Room::AddObject(ObjectRef object)
{
	// 이미 플레이어가 있다면 문제가 있다.
	if (_objects.find(object->objectInfo->object_id()) != _objects.end())
		return false;

	_objects.insert(make_pair(object->objectInfo->object_id(), object));

	object->room.store(GetRoomRef());

	return true;
}

bool Room::RemoveObject(uint64 objectId)
{
	// 없다면 문제가 있다.
	if (_objects.find(objectId) == _objects.end())
		return false;

	ObjectRef object = _objects[objectId];
	object->room.store(weak_ptr<Room>());

	_objects.erase(objectId);
	_dirtyMovers.erase(objectId);
	_attackers.erase(objectId);
	_casters.erase(objectId);
	_pendingTelegraphs.erase(remove_if(_pendingTelegraphs.begin(), _pendingTelegraphs.end(),
		[objectId](const PendingTelegraph& p)
		{return p.casterId == objectId; }), _pendingTelegraphs.end());
	_ccTargets.erase(objectId);

	return true;
}

void Room::Broadcast(SendBufferRef sendBuffer, uint64 exceptId)
{
	for (auto& item : _objects)
	{
		PlayerRef player = dynamic_pointer_cast<Player>(item.second);
		if (player == nullptr)
			continue;

		if (player->objectInfo->object_id() == exceptId)
			continue;

		if (GameSessionRef session = player->session.lock())
			session->Send(sendBuffer);
	}
}

void Room::Broadcast(SendBufferRef sendBuffer, const unordered_set<uint64>& exceptIds)
{
	for (auto& item : _objects)
	{
		PlayerRef player = dynamic_pointer_cast<Player>(item.second);
		if (player == nullptr)
			continue;

		if (exceptIds.find(player->objectInfo->object_id()) != exceptIds.end())
			continue;

		if (GameSessionRef session = player->session.lock())
			session->Send(sendBuffer);
	}
}

bool Room::CancelCast(const CreatureRef& caster)
{
	if (caster == nullptr || caster->IsCasting() == false)
		return false;

	const uint64 casterId = caster->objectInfo->object_id();

	const SkillDef* def = SkillTable::Find(caster->castSkillId);
	const bool wasBroadcast = (def != nullptr && def->castMs > 0);

	caster->castEndAtUs = 0;
	caster->castSkillId = 0;
	caster->castSlot = Protocol::SLOT_NONE;
	caster->castTargetId = 0;

	_casters.erase(casterId);

	if (wasBroadcast)
		_pendingCastCancels.push_back(casterId);

	return true;
}

void Room::SendEquipSync(const PlayerRef& player)
{
	Protocol::S_EQUIP_SYNC equipPkt;
	for (int32 i = 0; i < SKILL_SLOT_COUNT; i++)
	{
		const SkillSlot& s = player->skillSlots[i];

		Protocol::SkillInfo* info = equipPkt.add_skills();
		info->set_slot(static_cast<Protocol::EquipSlot>(i + 1));
		info->set_skill_id(s.skillId);
		info->set_bind_state(s.bindState);
		info->set_shadowed_by(s.shadowedBy);
	}

	SEND_PACKET_DECLARATION(equipPkt);
	if (auto session = player->session.lock())
		session->Send(sendBuffer);
}

void Room::SendInventorySync(const PlayerRef& player)
{
	Protocol::S_INVENTORY_SYNC invPkt;

	for (const ItemEntry& e : player->inventory)
		*invPkt.add_items() = e.ToProto(Protocol::ITEM_STATE_CARRIED, Protocol::SLOT_NONE);

	for (int32 i = 0; i < SKILL_SLOT_COUNT; i++)
	{
		const ItemEntry& e = player->equipped[i];
		if (e.instanceId == 0)
			continue;

		*invPkt.add_items() = e.ToProto(Protocol::ITEM_STATE_EQUIPPED, static_cast<Protocol::EquipSlot>(i + 1));
	}

	SEND_PACKET_DECLARATION(invPkt);
	if (auto session = player->session.lock())
		session->Send(sendBuffer);
}

void Room::KillPlayer(const PlayerRef& victim, uint64 instigatorId, Protocol::DeathCause cause, const char* by)
{
	const uint64 victimId = victim->objectInfo->object_id();
	victim->hp = 0;

	Protocol::DiedInfo& died = _pendingDeaths.emplace_back();
	died.set_victim_id(victimId);
	died.set_instigator_id(instigatorId);
	died.set_cause(cause);

	victim->attackTargetId = 0;
	victim->attackHitAtUs = 0;
	CancelCast(victim);

	LOG_LINE << "[DIED] victim=" << victimId << " by=" << by << '\n';
	BeginDeathCommit(victim);
}

void Room::UpdateDetached(uint64 nowUs)
{
	vector<uint64> expired;

	for (uint64 id : _detached)
	{
		auto findIt = _objects.find(id);
		PlayerRef body = (findIt != _objects.end()) ? dynamic_pointer_cast<Player>(findIt->second) : nullptr;

		if (body != nullptr && body->IsAlive() && nowUs >= body->detachedUntilUs)
			expired.push_back(id);
	}

	for (uint64 id : expired)
	{
		_detached.erase(id);

		// 재접속과 경쟁한다 - 표에서 먼저 꺼낸 쪽만 이긴다.
		PlayerRef body = GDetachedBodies.TryTake(id);
		if (body == nullptr)
			continue;

		LOG_LINE << "[DETACH] grace expired - char=" << id << '\n';
		KillPlayer(body, 0, Protocol::DEATH_CAUSE_NONE, "disconnect");
	}
}

void Room::ReleaseDetachedBody(uint64 characterId)
{
	auto findIt = _objects.find(characterId);
	if (findIt == _objects.end())
		return;

	PlayerRef body = dynamic_pointer_cast<Player>(findIt->second);
	if (body == nullptr || body->detachedUntilUs == 0)
		return;

	_detached.erase(characterId);
	GDetachedBodies.TryTake(characterId);
	LeaveRoom(body);

	LOG_LINE << "[DETACH] body released - char=" << characterId << '\n';
}

void Room::PushBagContents(const BagRef& bag)
{
	if (bag->viewers.empty())
		return;

	SendBufferRef sendBuffer = MakeBagContents(bag, Utils::NowMicroseconds());
	
	for (auto it = bag->viewers.begin(); it != bag->viewers.end(); )
	{
		auto objIt = _objects.find(*it);
		PlayerRef viewer = (objIt != _objects.end()) ? dynamic_pointer_cast<Player>(objIt->second) : nullptr;

		// 닫기 패킷이 없다. 떠났거나 죽었거나 멀어졌으면 열람이 끝난 것으로 보고 여기서 뺀다.
		if (viewer == nullptr || viewer->IsAlive() == false || IsInLootRange(viewer, bag) == false)
		{
			it = bag->viewers.erase(it);
			continue;
		}

		if (auto viewerSession = viewer->session.lock())
			viewerSession->Send(sendBuffer);

		++it;
	}
}

void Room::CollectSkillTargets(const CreatureRef& caster, const SkillDef& def, const float originX, float originY, uint64 castTargetId, float aimX, float aimY, OUT vector<CreatureRef>& outTargets)
{
	outTargets.clear();

	const uint64 casterId = caster->objectInfo->object_id();

	switch (def.shape)
	{
	case SkillShape::Single:
	{
		auto findIt = _objects.find(castTargetId);
		if (findIt == _objects.end())
			return;

		CreatureRef target = dynamic_pointer_cast<Creature>(findIt->second);
		if (target == nullptr || target->IsAlive() == false)
			return;

		const float dx = target->posInfo->x() - originX;
		const float dy = target->posInfo->y() - originY;
		if ((dx * dx + dy * dy) > (def.rangeCm * def.rangeCm))
			return;

		outTargets.push_back(target);
		return;
	}
	case SkillShape::CircleSelf:
	{
		if (def.radiusCm <= 0.f)
			return;

		for (auto& item : _objects)
		{
			if (item.first == casterId)
				continue;

			CreatureRef target = dynamic_pointer_cast<Creature>(item.second);
			if (target == nullptr || target->IsAlive() == false)
				continue;

			const float dx = target->posInfo->x() - originX;
			const float dy = target->posInfo->y() - originY;
			if ((dx * dx + dy * dy) <= (def.radiusCm * def.radiusCm))
				outTargets.push_back(target);
		}
		return;
	}

	default:
		// TODO : LIne, CirclePoint
		return;
	}
}

void Room::ApplySkillEffects(const CreatureRef& caster, const SkillDef& def, const vector<CreatureRef>& targets, uint64 nowUs)
{
	const uint64 casterId = caster->objectInfo->object_id();

	for (const SkillEffect& e : def.effects)
	{
		switch (e.type)
		{
		case SkillEffectType::Movement:
			ApplyMovement(caster, e, nowUs);
			break;
		case SkillEffectType::SelfBuff:
			// TODO : MoveSpeed, WallPierce
			break;
		default:
			break;
		}
	}

	for (const CreatureRef& target : targets)
	{
		const uint64 targetId = target->objectInfo->object_id();

		for (const SkillEffect& e : def.effects)
		{
			switch (e.type)
			{
			case SkillEffectType::Damage:
			{
				if (e.adRatio <= 0.f && e.apRatio <= 0.f)
					break;

				bool isCrit = false;
				const int32 damage = caster->ComputeSkillDamageTo(*target, e.adRatio, e.apRatio, OUT isCrit);
				const int32 remainingHp = target->TakeDamage(damage);

				Protocol::DamageInfo& dmg = _pendingDamages.emplace_back();
				dmg.set_attacker_id(casterId);
				dmg.set_target_id(targetId);
				dmg.set_damage(damage);
				dmg.set_remaining_hp(remainingHp);
				dmg.set_is_crit(isCrit);

				dmg.set_damage_type(e.adRatio > 0.f ? Protocol::DAMAGE_TYPE_PHYSICAL : Protocol::DAMAGE_TYPE_MAGIC);

				if (remainingHp <= 0)
				{
					Protocol::DiedInfo& died = _pendingDeaths.emplace_back();
					died.set_victim_id(targetId);
					died.set_instigator_id(casterId);
					died.set_cause(target->IsPlayer() ? Protocol::DEATH_CAUSE_PLAYER : Protocol::DEATH_CAUSE_MONSTER);

					target->attackTargetId = 0;
					target->attackHitAtUs = 0;
					_attackers.erase(targetId);
					CancelCast(target);

					LOG_LINE << "[DIED] victim=" << targetId << " by=" << casterId << '\n';

					BeginDeathCommit(target);
				}
				break;
			}
			case SkillEffectType::HardCc:
			case SkillEffectType::SoftCc:
				if (e.ccDurationMs > 0)
					ApplyCc(targetId, casterId, e.ccType, e.ccDurationMs, e.ccMagnitude);
				break;

			default:
				// TODO : Knockback, Movement, SelfBuff
				break;
			}
		}
	}
}

void Room::ApplyMovement(const CreatureRef& caster, const SkillEffect& effect, uint64 nowUs)
{
	if (effect.distCm <= 0.f || effect.speedCms <= 0.f)
		return;

	PlayerRef player = dynamic_pointer_cast<Player>(caster);
	if (player == nullptr)
		return;

	constexpr uint64 DASH_GRACE_US = 100'000;

	const double durSec = static_cast<double>(effect.distCm) / effect.speedCms;
	const uint64 durUs = static_cast<uint64>(durSec * 1'000'000.0);

	player->moveExceptionSpeed = effect.speedCms;
	player->moveExceptionEndUs = nowUs + durUs + DASH_GRACE_US;

	LOG_LINE << "[DASH WINDOW] caster=" << player->objectInfo->object_id()
		<< " speed=" << effect.speedCms
		<< " dur=" << (durUs / 1000) << "ms" << '\n';
}

void Room::SavePlayer(PlayerRef player)
{
	if (player == nullptr)
		return;

	const uint64 characterId = player->objectInfo->object_id();
	if (characterId == 0)
		return;

	const int32 hp = player->hp;
	const float x = player->posInfo->x();
	const float y = player->posInfo->y();
	const float z = player->posInfo->z();
	const float yaw = player->posInfo->yaw();

	GDBQueue.Push([=](DBConnection* conn)
		{
			char query[512];
			::snprintf(query, sizeof(query),
				"UPDATE characters SET hp = %d, pos_x = %f, pos_y = %f, "
				"pos_z = %f, yaw = %f WHERE character_id = %llu",
				hp, x, y, z, yaw, characterId);

			conn->Excute(query);
		});
}

void Room::UpdateSaves(uint64 nowUs)
{
	int32 saved = 0;

	for (auto& item : _objects)
	{
		if (saved >= SAVE_PER_TICK)
			break;

		auto player = dynamic_pointer_cast<Player>(item.second);
		if (player == nullptr)
			continue;

		if (nowUs - player->lastSaveUs < SAVE_INTERVAL_US)
			continue;

		player->lastSaveUs = nowUs;
		SavePlayer(player);
		saved++;
	}
}

void Room::UpdateBagExpiry(uint64 nowUs)
{
	vector<uint64> bagIds;
	vector<uint64> instanceIds;

	for (auto& [bagId, bag] : _bags)
	{
		if (nowUs < bag->expiresAtUs)
			continue;

		bagIds.push_back(bagId);

		for (const ItemEntry& e : bag->items)
			instanceIds.push_back(e.instanceId);
	}

	if (bagIds.empty())
		return;

	for (uint64 bagId : bagIds)
		RemoveBag(bagId);

	LOG_LINE << "[BAG] expired floor=" << _floorId << " bags=" << bagIds.size() << " items=" << instanceIds.size() << '\n';

	GDBQueue.Push([bagIds, instanceIds](DBConnection* conn)
		{
			DeleteExpiredBags(conn, bagIds, instanceIds);
		});
}

void Room::FindSpawnPosition(float baseX, float baseY, float& outX, float& outY)
{
	const float spacingSq = SPAWN_SPACING_CM * SPAWN_SPACING_CM;

	for (int32 ring = 1; ring <= SPAWN_MAX_RINGS; ring++)
	{
		const int32 slots = (ring == 0) ? 1 : ring * 6;
		const float radius = ring * SPAWN_SPACING_CM;

		for (int32 i = 0; i < slots; i++)
		{
			const float  angle = (2.f * SPAWN_PI * i) / slots;
			const float cx = baseX + radius * ::cosf(angle);
			const float cy = baseY + radius * ::sinf(angle);

			bool occupied = false;

			for (auto& item : _objects)
			{
				const float dx = item.second->posInfo->x() - cx;
				const float dy = item.second->posInfo->y() - cy;

				if ((dx * dx + dy * dy) < spacingSq)
				{
					occupied = true;
					break;
				}
			}

			if (occupied == false)
			{
				outX = cx;
				outY = cy;
				return;
			}
		}
	}

	const float fallback = SPAWN_MAX_RINGS * SPAWN_SPACING_CM;
	outX = baseX + Utils::GetRandom(-fallback, fallback);
	outY = baseY + Utils::GetRandom(-fallback, fallback);
}
