#include "pch.h"
#include "Room.h"
#include "Player.h"
#include "SkillTable.h"
#include "ItemTable.h"
#include "ItemRoller.h"
#include "ServerStats.h"
#include "ObjectUtils.h"
#include "Bag.h"
#include "GameSessionManager.h"
#include "FloorBounds.h"
#include "SpawnPlacement.h"
#include "WallTable.h"
#include "Monster.h"
#include "MonsterTable.h"
#include "TowerTable.h"
#include "Telemetry.h"

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

	constexpr float		LOOT_RANGE_CM = 200.f;
	constexpr uint64	LEAVE_RETRY_MS = 100;		// 사망 커밋을 기다리는 퇴장 재시도 간격
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
		LogLine out;

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

	// 장비 합산 결과 (EquipStats) - 봇 · 로그로 장비가 전투에 들어갔는지 확인한다
	void LogStats(const PlayerRef& player, const char* tag)
	{
		LOG_LINE << "[STATS] " << tag << " char=" << player->objectInfo->object_id()
			<< " hp=" << player->hp << "/" << player->maxHp
			<< " atk=" << player->attackPower << " mag=" << player->magicPower
			<< " armor=" << player->armor << " mr=" << player->magicResist
			<< " crit=" << player->critChance << " critx=" << player->critMultiplier
			<< " as=" << player->percentAsBonus << " ms=" << player->percentMsBonus
			<< " res=" << player->resistance << " range=" << player->attackRange
			<< " type=" << (player->autoAttackType == Protocol::DAMAGE_TYPE_MAGIC ? "magic" : "physical") << '\n';
	}

	bool IsInAttackRange(const CreatureRef& attacker, const CreatureRef& target)
	{
		const float dx = target->posInfo->x() - attacker->posInfo->x();
		const float dy = target->posInfo->y() - attacker->posInfo->y();

		return (dx * dx + dy * dy) <= (attacker->attackRange * attacker->attackRange);
	}

	// 평타 시선 - 공격자와 대상 중심을 잇는 선분이 벽 · 엄폐물 · 타워에 걸리면 못 때린다 (combat-system.md Rule 1 · 7,
	// 사용자 결정 2026-10-08 "평타는 벽 너머 불가능"). 면에 닿기만 하는 선분은 막지 않는다(WallMap 열린 집합). 벽 없는 층은 늘 통과
	bool IsAttackLineClear(const WallMap& walls, const CreatureRef& attacker, const CreatureRef& target)
	{
		return walls.SweepBlocked(attacker->posInfo->x(), attacker->posInfo->y(), target->posInfo->x(), target->posInfo->y(), 0.f) == false;
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
	else if (auto entering = dynamic_pointer_cast<Player>(object); entering != nullptr && entering->spreadOnEnter)
	{
		// 고른 진입 지점 둘레의 빈자리 - 같은 지점을 고른 사람끼리 겹치지 않게 (entry-point.md AC-EP-04)
		float spawnX = 0.f;
		float spawnY = 0.f;
		const float pointX = object->posInfo->x();
		const float pointY = object->posInfo->y();
		FindSpawnPosition(pointX, pointY, spawnX, spawnY);

		object->posInfo->set_x(spawnX);
		object->posInfo->set_y(spawnY);
		entering->spreadOnEnter = false;

		LOG_LINE << "[ENTER] placed char=" << object->objectInfo->object_id() << " at=(" << spawnX << "," << spawnY
			<< ") point=(" << pointX << "," << pointY << ") hp=" << entering->hp << '\n';
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
		player->moveClock.Reset();
		player->lastSaveUs = enterUs;

		// 장비 스탯 - 입장 체력은 새 최대치로 가득. S_ENTER_GAME · 스폰 스냅샷이 이 체력을 싣는다 (TR-combat-026)
		player->ApplyEquipmentStats(true);
		player->objectInfo->set_hp(player->hp);
		player->objectInfo->set_max_hp(player->maxHp);
		LogStats(player, "enter");

		// 체류 타이머는 방에 놓인 순간부터 - DB 적재 지연이 30 분을 깎지 않는다 (escape-timer.md Core Rule 13)
		if (success)
		{
			player->runStartUs = enterUs;
			player->runDeadlineUs = enterUs + EscapeConfig::Get().runLimitUs;
			player->spawnInvulnUntilUs = EscapeConfig::Get().spawnInvulnerabilityUs > 0 ? enterUs + EscapeConfig::Get().spawnInvulnerabilityUs : 0;
			player->runGold = 0;
			player->zoneTower = -1;
			memset(player->gaugeAudience, 0, sizeof(player->gaugeAudience));
			memset(player->lastGaugeSentUs, 0, sizeof(player->lastGaugeSentUs));
			player->statZoneVisits = player->statZoneLeavesVoluntary = player->statZoneLeavesForced = player->statGaugeReversals = 0;
			player->statZoneDwellUs = player->statAlertDwellUs = 0;
			player->statLastTowerId.clear();

			// 텔레메트리 F8 - 들고 들어온 것의 등급 점수 (1 · 2 · 4 · 8)
			player->statBroughtInSum = 0;
			auto points = [](const ItemEntry& e) { return e.grade > 0 ? (1u << (static_cast<int32>(e.grade) - 1)) : 0u; };
			for (const ItemEntry& e : player->inventory)
				player->statBroughtInSum += points(e);
			for (const ItemEntry& e : player->equipped)
				if (e.instanceId != 0)
					player->statBroughtInSum += points(e);
		}


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
		if (success)
			enterGamePkt.set_run_remaining_ms(static_cast<uint32>(EscapeConfig::Get().runLimitUs / 1000));

		Protocol::ObjectInfo* playerInfo = new Protocol::ObjectInfo();
		playerInfo->CopyFrom(*player->objectInfo);
		enterGamePkt.set_allocated_player(playerInfo);
		//enterGamePkt.release_player();

		SendBufferRef sendBuffer = ServerPacketHandler::MakeSendBuffer(enterGamePkt);
		if (auto session = player->session.lock())
			session->Send(sendBuffer);

		if (success)
			SendChestSnapshot(player);

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
		{
			session->Send(sendBuffer);

			// 베이스캠프로 돌아왔다 - S3 에 DB 상태를 보낸다 (UI 4단계). 죽은 뒤 나가는 경우엔 HandleLeavePlayer 가
			// 사망 커밋이 끝날 때까지 이 호출을 미뤘다.
			GDBQueue.Push([session, objectId](DBConnection* conn)
				{
					SendBaseCampInventory(conn, session, objectId);
				});
		}
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
	JOB_TAG();
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
	JOB_TAG();
	// 던전에서 살아 있는 채로 나가는 길은 탈출뿐이다 - 나가기로 사망을 피하는 허점을 막는다.
	if (_objects.find(player->objectInfo->object_id()) != _objects.end() && player->IsAlive())
	{
		LOG_LINE << "[LEAVE] rejected - alive in dungeon. char=" << player->objectInfo->object_id() << '\n';
		return false;
	}

	// 죽은 뒤 나가는 경우 - 사망 커밋이 끝난 뒤에 내보낸다. 그래야 S_LEAVE_GAME 이 "DB 가 확정됐다"를 뜻하고,
	// 베이스캠프 화면이 사망 결과(가방으로 간 소지품)를 보여 준다. 커밋은 보통 수 ms 다.
	if (GDeathCommits.IsPending(player->objectInfo->object_id()))
	{
		DoTimer(LEAVE_RETRY_MS, &Room::HandleLeavePlayer, player);
		return true;
	}

	return LeaveRoom(player);
}

void Room::HandleMove(Protocol::C_MOVE pkt)
{
	JOB_TAG();
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
	bool wallRejected = false;

	if (rejected == false)
	{
		// 경과 시간은 클라가 보낸 시각 간격으로 - 밀린 이동이 한꺼번에 처리돼도 정상 간격으로 본다 (MoveClock.h)
		const uint64 serverDtUs = nowUs - player->lastMoveUs;

		// 입장 후 첫 이동 - 이 접속이 보낸 시각을 싣는지 한 번 남긴다(구버전 클라는 no = 서버 간격으로 검증)
		if (player->moveClock.announced == false)
		{
			player->moveClock.announced = true;
			LOG_LINE << "[MOVE] clock char=" << objectId << " client_ms=" << (pkt.client_ms() != 0 ? "yes" : "no") << '\n';
		}

		const double deltaSec = player->moveClock.Interval(serverDtUs, pkt.client_ms());
		player->lastMoveUs = nowUs;

		// 속도 상한도 "보낸 때" 기준 - 늦게 도착한 패킷은 뒤처진 만큼 이른 시각으로 본다. 대시 창이 끝난 뒤 처리된
		// 대시 중 이동이 일반 속도로 검사되지 않게 (leadUs < 0 = 클라가 뒤처졌다 = 패킷이 늦게 왔다)
		const int64 leadUs = player->moveClock.leadUs;
		const uint64 sentUs = (leadUs < 0) ? nowUs - min<uint64>(static_cast<uint64>(-leadUs), nowUs) : nowUs;
		const double ceiling = player->GetSpeedCeiling(sentUs) * VALIDATION_MARGIN;

		player->moveBudget += ceiling * deltaSec;

		const double maxBudget = ceiling * MAX_BUDGET_SEC;
		if (player->moveBudget > maxBudget)
			player->moveBudget = maxBudget;

		// 층 사각형 밖 · NaN · 무한대 - 예산을 쓰지 않고 거부한다 (FloorBounds.h, level-map.md AC-LM-02)
		if (FloorBounds::Contains(pkt.info().x(), pkt.info().y()) == false)
		{
			rejected = true;

			LOG_LINE << "[MOVE REJECT] id=" << objectId
				<< " reason=bounds"
				<< " x=" << pkt.info().x()
				<< " y=" << pkt.info().y() << '\n';
		}

		// XY 평면 거리만 본다.
		const double dx = static_cast<double>(pkt.info().x()) - player->posInfo->x();
		const double dy = static_cast<double>(pkt.info().y()) - player->posInfo->y();
		const double distance = std::sqrt(dx * dx + dy * dy);

		if (rejected)
		{
			// 경계 거부 - 이미 남겼다
		}
		else if (distance > player->moveBudget)
		{
			rejected = true;

			LOG_LINE << "[MOVE REJECT] id=" << objectId
				<< " reason=speed"
				<< " dist=" << distance
				<< " budget=" << player->moveBudget
				<< " dt=" << deltaSec
				<< " server_dt=" << (serverDtUs / 1'000'000.0)
				<< " lead_ms=" << (player->moveClock.leadUs / 1000) << '\n';
		}
		else
		{
			// 벽 - 속도 다음이다. 속도 검사가 선분 길이를 묶어 둬야 벽 질의가 보는 격자 칸 수가 묶인다 (ADR-0006 § ⑥).
			// 캐릭터 중심 경로(r = 0)만 본다 - 정상 클라의 캡슐 중심은 벽면에서 42 cm 밖에 있으므로 중심이 벽 안으로
			// 들어가면 조작이다. 벽 데이터가 없는 층(TEMP)은 빈 맵이라 늘 통과한다.
			const float x0 = player->posInfo->x();
			const float y0 = player->posInfo->y();
			WallKind hitKind = WallKind::Wall;

			if (GWallCheck.load(memory_order_relaxed)
				&& WallTable::Get(_floorId).SweepBlocked(x0, y0, pkt.info().x(), pkt.info().y(), 0.f, OUT &hitKind))
			{
				rejected = true;
				wallRejected = true;

				LOG_LINE << "[MOVE REJECT] id=" << objectId
					<< " reason=wall"
					<< " x0=" << x0
					<< " y0=" << y0
					<< " x1=" << pkt.info().x()
					<< " y1=" << pkt.info().y()
					<< " kind=" << WallKindName(hitKind) << '\n';
			}
			else
			{
				player->moveBudget -= distance;
			}
		}
	}

	if (rejected)
	{
		// 벽 거부는 따로 센다 - 봇(DummyClient)은 벽을 몰라 소크에서 SV-3 판정을 흐린다 (ADR-0006 § Risks)
		if (wallRejected)
			GStats.AddMoveWallReject();
		else
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

	// 진입 무적은 움직이면 풀린다 (AC-10) - 제자리 회전 · 같은 좌표 재전송은 이동이 아니다
	if (player->spawnInvulnUntilUs != 0)
	{
		const float mdx = pkt.info().x() - player->posInfo->x();
		const float mdy = pkt.info().y() - player->posInfo->y();
		if (mdx * mdx + mdy * mdy > 1.f)
		{
			const uint64 nowInvUs = Utils::NowMicroseconds();
			LOG_LINE << "[INVULN] cleared char=" << objectId << " by=move left="
				<< (player->spawnInvulnUntilUs > nowInvUs ? (player->spawnInvulnUntilUs - nowInvUs) / 1000 : 0) << "ms\n";
			player->spawnInvulnUntilUs = 0;
		}
	}

	// 검증 통과, 이동
	player->posInfo->CopyFrom(pkt.info());
	_dirtyMovers.insert(objectId);
}

void Room::HandleAttack(uint64 attackerId, uint64 targetId)
{
	JOB_TAG();
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

	// 진입 무적 - 때릴 수 없다 (AC-10). 3 초 동안만 생기는 줄이라 로그로 남긴다
	if (target->IsPlayer() && static_pointer_cast<Player>(target)->IsSpawnInvulnerable(nowUs))
	{
		LOG_LINE << "[INVULN] blocked attack on char=" << targetId << " by=" << attackerId << '\n';
		return;
	}

	// 대상 락 시점에도 사거리 · 시선을 본다. 히트 시점에 한 번 더 재검증
	if (IsInAttackRange(attacker, target) == false)
		return;

	if (IsAttackLineClear(WallTable::Get(_floorId), attacker, target) == false)
	{
		GStats.AddAttackWallBlocked();
		return;
	}

	// 공격하면 내 무적이 풀린다 (AC-10)
	if (attacker->IsPlayer())
		ClearSpawnInvulnerability(static_pointer_cast<Player>(attacker), "attack");

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
	JOB_TAG();
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

	// 스킬도 공격이다 - 무적이 풀린다 (AC-10)
	if (caster->IsPlayer())
		ClearSpawnInvulnerability(static_pointer_cast<Player>(caster), "skill");

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
	JOB_TAG();
	auto findIt = _objects.find(casterId);
	if (findIt == _objects.end())
		return;

	CancelCast(dynamic_pointer_cast<Creature>(findIt->second));
}

void Room::ApplyCc(uint64 targetId, uint64 instigatorId, Protocol::CcType type, uint32 baseDurationMs, float magnitude)
{
	JOB_TAG();
	auto findIt = _objects.find(targetId);
	if (findIt == _objects.end())
		return;

	CreatureRef target = dynamic_pointer_cast<Creature>(findIt->second);
	if (target == nullptr || target->IsAlive() == false)
		return;

	const uint64 nowUs = Utils::NowMicroseconds();

	// 진입 무적 - CC 도 받지 않는다 (AC-10)
	if (target->IsPlayer() && static_pointer_cast<Player>(target)->IsSpawnInvulnerable(nowUs))
		return;

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


		// 공격자 사망, 대상 소명, 대상 사망, 사거리 이탈, 대상 진입 무적 -> 취소. 쿨다운을 소모 하지 않는다.
		// 공격자가 이번 Flush 에 막 죽었으면 이 히트는 동시다 - 그대로 들어간다 (TR-combat-022, KillBatch)
		if (CanStillStrike(attacker) == false
			|| target == nullptr
			|| target->IsAlive() == false
			|| IsInAttackRange(attacker, target) == false
			|| (target->IsPlayer() && static_pointer_cast<Player>(target)->IsSpawnInvulnerable(nowUs)))
		{
			_pendingAttackCancels.push_back(attackerId);
			continue;
		}

		// windup 중 벽 · 엄폐물 뒤로 들어갔다 -> 시선 상실, 같은 취소 (Rule 7 · Edge Cases)
		if (IsAttackLineClear(WallTable::Get(_floorId), attacker, target) == false)
		{
			GStats.AddAttackWallBlocked();
			_pendingAttackCancels.push_back(attackerId);
			continue;
		}

		bool isCrit = false;
		const int32 damage = attacker->ComputeDamageTo(*target, attacker->autoAttackType, OUT isCrit);
		const int32 remainingHp = target->TakeDamage(damage);

		Protocol::DamageInfo& dmg = _pendingDamages.emplace_back();
		dmg.set_attacker_id(attackerId);
		dmg.set_target_id(targetId);
		dmg.set_damage(damage);
		dmg.set_remaining_hp(remainingHp);
		dmg.set_is_crit(isCrit);
		dmg.set_damage_type(attacker->autoAttackType);

		// 후딜 소모는 히트에 성공했을 때만
		attacker->attackReadyAtUs = nowUs + static_cast<uint64>(attacker->GetAttackRecoveryMs()) * 1000;

		if (target->IsMonster())
			OnMonsterDamaged(static_pointer_cast<Monster>(target), attackerId, damage, nowUs);

		if (attacker->IsMonster())
		{
			MonsterBrain::NoteEngaged(static_pointer_cast<Monster>(attacker)->brain, nowUs);
			GMonsterStats.hits.fetch_add(1);
		}

		// 이 루프가 _attackers 를 돌고 있다 - 사망 처리는 Flush 끝 (FlushCombat → KillBatch)
		if (remainingHp <= 0)
			KillCreature(target, attacker, attackerId, nowUs);
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

		// 이번 Flush 에 막 죽은 시전자의 예고는 동시 - 그대로 떨어진다 (TR-combat-022)
		if (caster == nullptr || CanStillStrike(caster) == false)
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
	JOB_TAG();
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

	if (ItemTable::CanEquip(found->itemTypeId, slot) == false)
	{
		session->SendItemResult(requestId, Protocol::ITEM_BAD_REQUEST, {});
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

	OnEquipmentChanged(player, "equip");
	SendEquipSync(player);

	session->SendItemResult(requestId, Protocol::ITEM_OK, changed);
}

void Room::HandleUnequip(GameSessionRef session, string requestId, uint64 instanceId)
{
	JOB_TAG();
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

	OnEquipmentChanged(player, "unequip");
	SendEquipSync(player);

	vector<Protocol::ItemInstance> changed;
	changed.push_back(e.ToProto(Protocol::ITEM_STATE_CARRIED));

	session->SendItemResult(requestId, Protocol::ITEM_OK, changed);
}

void Room::HandleBagOpen(GameSessionRef session, uint64 bagId)
{
	JOB_TAG();
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

	// 상자는 처음 열 때 롤한다 (§ 8 지연 롤). JobQueue 가 직렬이라 둘이 동시에 열어도 한 번만 롤된다.
	// 부팅이 상자마다 그 층 · 등급의 풀이 있는지 확인했다 (ChestSpawnTable).
	if (bag->IsChest() && bag->rolled == false)
	{
		if (const ChestPool* pool = FloorItemPool::Find(_floorId, bag->tier))
		{
			bag->RollIfNeeded(*pool, _rng, []() { return GNextInstanceId.fetch_add(1); });

			LOG_LINE << "[CHEST] rolled chest=" << bag->chestId << " tier=" << FloorItemPool::TierName(bag->tier)
				<< " items=" << bag->items.size() << " by=" << playerId << '\n';
		}
	}

	bag->viewers.insert(playerId);
	session->Send(MakeBagContents(bag, Utils::NowMicroseconds()));

	LOG_LINE << "[LOOT] open - char=" << playerId << " bag=" << bagId << " items=" << bag->items.size() << '\n';
}

void Room::HandleLoot(GameSessionRef session, string requestId, uint64 bagId, uint64 instanceId)
{
	JOB_TAG();
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

	// 상자는 첫 "획득"에서 Looted 가 되고 리스폰 시계가 돈다 (§ 8). 열기만 해서는 바뀌지 않는다.
	if (bag->MarkTaken(Utils::NowMicroseconds()))
	{
		LOG_LINE << "[CHEST] looted chest=" << bag->chestId << " by=" << playerId << " reset_in=" << bag->respawnUs / 1'000'000 << "s\n";

		// 층 전원에게 변경분 - 레이더가 털린 상자를 지운다 (radar.md OQ-1). 리셋은 기존 S_DESPAWN → S_SPAWN 이 곧 Sealed 다
		Protocol::S_CHEST_STATE statePkt;
		statePkt.add_looted_ids(bagId);
		statePkt.set_snapshot(false);
		SEND_PACKET_DECLARATION(statePkt);
		Broadcast(sendBuffer);
	}

	vector<Protocol::ItemInstance> changed;
	changed.push_back(e.ToProto(Protocol::ITEM_STATE_CARRIED));
	session->SendItemResult(requestId, Protocol::ITEM_OK, changed);

	// 비어도 지우지 않는다.
	PushBagContents(bag);

	LOG_LINE << "[LOOT] take - char=" << playerId << " bag=" << bagId << " item=" << instanceId << " left=" << bag->items.size() << " inv=" << player->inventory.size() << '\n';
}

void Room::HandleDrop(GameSessionRef session, string requestId, uint64 instanceId)
{
	JOB_TAG();
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

	// 이번 런에 생긴 아이템은 DB 행이 없다 - 지울 것이 없으니 파기 목록에 넣지 않는다. 메모리에서 사라지면 끝이다.
	if (e.isNew == false)
		player->droppedIds.push_back(e.instanceId);

	vector<Protocol::ItemInstance> changed;
	changed.push_back(e.ToProto(Protocol::ITEM_STATE_NONE));
	session->SendItemResult(requestId, Protocol::ITEM_OK, changed);

	LOG_LINE << "[ITEM] drop - char=" << playerId << " item=" << e.instanceId
		<< " inv=" << player->inventory.size() << " dropped=" << player->droppedIds.size() << '\n';
}

bool Room::AddBag(BagRef bag)
{
	JOB_TAG();
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
	JOB_TAG();
	auto findIt = _objects.find(bagId);
	if (findIt == _objects.end() || findIt->second->IsBag() == false)
		return false;

	RemoveObject(bagId);
	_bags.erase(bagId);

	// 스폰 전에 지워졌다면(33ms 안) FlushSpawns가 _objects에서 못 찾아 건너뛴다. 이 DESPAWN은 무해하다.
	Protocol::S_DESPAWN despawnPkt;
	despawnPkt.add_object_ids(bagId);

	SEND_PACKET_DECLARATION(despawnPkt);
	sendBuffer->SetStatId(STAT_ID_BAG_DESPAWN);
	Broadcast(sendBuffer);

	LOG_LINE << "[BAG] remove id=" << bagId << '\n';

	return true;
}

void Room::DebugDropBag(uint64 playerId)
{
	JOB_TAG();
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

	// 런 중 생성 아이템 검증용 (상자 3단계) - 개활지 상자 한 번 분량을 롤해 넣는다. DB 행이 없다(isNew).
	// 이 층에 풀이 없으면 예전처럼 빈 가방이다.
	if (const ChestPool* pool = FloorItemPool::Find(_floorId, ChestTier::Open))
		bag->items = ItemRoller::RollChest(*pool, _rng, []() { return GNextInstanceId.fetch_add(1); });

	{
		// 여러 문장으로 한 줄을 찍는다 - 객체 하나에 모아 소멸할 때 한 번에 내보낸다 (LogSlots 와 같은 방식)
		LogLine out;
		out << "[BAG] debug drop by=" << playerId << " place=" << place << " items=" << bag->items.size();

		for (const ItemEntry& e : bag->items)
			out << " " << e.instanceId << "(t" << e.itemTypeId << " g" << e.grade << " s" << e.skillIdPrimary << "/" << e.skillIdSecondary << ")";

		out << '\n';
	}

	AddBag(bag);
}

void Room::DebugKill(uint64 playerId)
{
	JOB_TAG();
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

	const int64 newItems = count_if(snap.items.begin(), snap.items.end(), [](const ItemEntry& e) { return e.isNew; });

	LOG_LINE << "[DEATH] commit begin - char=" << characterId << " bag=" << snap.bagId << " items=" << snap.items.size()
		<< " new=" << newItems << " dropped=" << snap.droppedIds.size() << '\n';

	RoomRef room = GetRoomRef();
	GDBQueue.Push([room, snap](DBConnection* conn)
		{
			const bool ok = CommitDeath(conn, snap);
			room->DoAsync(&Room::OnBagCreated, snap, ok);
		});
}

void Room::OnBagCreated(DeathSnapshot snap, bool ok)
{
	JOB_TAG();
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

	// 커밋이 끝났다 - 런 중 생성 아이템도 이제 DB 행이 있다. isNew 를 남기면 이 가방에서 주워 탈출할 때
	// 같은 id 를 또 INSERT 하려다 실패하고, 탈출 커밋은 무한 재시도에 빠진다.
	for (ItemEntry& e : bag->items)
	{
		e.level = 0;
		e.isNew = false;
	}

	AddBag(bag);

	// 사망 커밋 중에 끊겼다가 다시 로그인한 세션은 커밋 전 상태를 받았다(SendBaseCampInventory 가 기다리다 보낸 것).
	// 확정된 결과를 다시 보낸다 - 안 보내면 가방으로 간 아이템이 베이스캠프 화면에 남는다 (UI 4단계).
	if (GameSessionRef baseCamp = GSessionManager.FindInBaseCamp(snap.characterId))
	{
		const uint64 characterId = snap.characterId;
		GDBQueue.Push([baseCamp, characterId](DBConnection* conn)
			{
				SendBaseCampInventory(conn, baseCamp, characterId);
			});

		LOG_LINE << "[DEATH] committed while in base camp - resending inventory. char=" << characterId << '\n';
	}

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

	OnEquipmentChanged(player, "death");
	SendEquipSync(player);

	Protocol::S_INVENTORY_SYNC invPkt;		// 빈 목록, 전부 가방으로 감
	SEND_PACKET_DECLARATION(invPkt);
	if (auto session = player->session.lock())
		session->Send(sendBuffer);
}

void Room::BeginEscapeCommit(uint64 playerId)
{
	JOB_TAG();
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
	snap.gold = player->runGold;

	WriteRunTelemetry(player, "ESCAPED", 0, snap.gold, Utils::NowMicroseconds());
	player->runDeadlineUs = 0;

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

	int64 newItems = count_if(snap.carried.begin(), snap.carried.end(), [](const ItemEntry& e) { return e.isNew; });
	for (const ItemEntry& e : snap.equipped)
	{
		if (e.instanceId != 0 && e.isNew)
			newItems++;
	}

	LOG_LINE << "[ESCAPE] commit begin - char=" << playerId << " carried=" << snap.carried.size()
		<< " new=" << newItems << " dropped=" << snap.droppedIds.size() << '\n';

	// 큐에 넣은 순간부터 커밋 완료까지 - 동시 완충 때 큐가 비는 시간 (escape-timer.md AC-ET-26)
	const uint64 queuedUs = Utils::NowMicroseconds();

	GDBQueue.Push([session, snap, queuedUs](DBConnection* conn)
		{
			// 성공할 때까지 재시도. 돌아왔으면 커밋된 것
			CommitEscape(conn, snap);
			session->place.store(SessionPlace::BaseCamp);
			GDeathCommits.End(snap.characterId);

			Protocol::S_LEAVE_GAME leavePkt;
			leavePkt.set_reason(Protocol::RUN_END_ESCAPED);
			leavePkt.set_gold_kept(snap.gold);
			SEND_PACKET_DECLARATION(leavePkt);
			session->Send(sendBuffer);

			LOG_LINE << "[ESCAPE] committed - char=" << snap.characterId << " gold=" << snap.gold
				<< " queued_to_done=" << (Utils::NowMicroseconds() - queuedUs) / 1000 << "ms\n";

			// 베이스캠프로 돌아왔다 - 방금 커밋한 결과를 S3 에 보낸다 (UI 4단계)
			SendBaseCampInventory(conn, session, snap.characterId);
		});
}

void Room::HandleDisconnect(PlayerRef player)
{
	JOB_TAG();
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
	JOB_TAG();
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
	body->moveClock.Reset();		// 새 접속의 클라 시계는 이전 것과 이어지지 않는다
	body->moveBudget = body->GetSpeedCeiling(nowUs) * VALIDATION_MARGIN * MAX_BUDGET_SEC;

	Protocol::S_ENTER_GAME enterPkt;
	enterPkt.set_success(true);
	enterPkt.mutable_player()->CopyFrom(*body->objectInfo);
	// 마감은 그대로 - 끊긴 동안에도 시간은 흘렀다 (escape-timer.md Core Rule 14)
	enterPkt.set_run_remaining_ms(static_cast<uint32>(EscapeLogic::RemainingUs(body->runDeadlineUs, nowUs) / 1000));
	SEND_PACKET_DECLARATION(enterPkt);
	session->Send(sendBuffer);

	SendEquipSync(body);
	SendInventorySync(body);
	SendChestSnapshot(body);

	// 게이지는 처음부터 다시 받게 한다 - 새 접속의 클라는 아무것도 모른다
	memset(body->gaugeAudience, 0, sizeof(body->gaugeAudience));

	// 방 전체 스냅샷을 다시 받게 한다. 다른 클라에는 이미 있는 몸이라 S_SPAWN 은 무시한다.
	_pendingSpawns.push_back(playerId);

	LOG_LINE << "[REATTACH] char=" << playerId << " inv=" << body->inventory.size() << " dropped=" << body->droppedIds.size() << '\n';
}

void Room::UpdateTick()
{
	JOB_TAG();
	const uint64 nowUs = Utils::NowMicroseconds();

	UpdateSaves(nowUs);
	UpdateBagExpiry(nowUs);
	UpdateChests(nowUs);

	// 같은 틱에 완충과 마감이 오면 탈출이 이긴다 - 게이지가 먼저다 (escape-timer.md Core Rule 16 · AC-ET-12)
	UpdateEscapeTowers(nowUs);
	UpdateRunTimers(nowUs);

	UpdateDetached(nowUs);
	UpdateMonsters(nowUs);

	// 0.1초 경과했으면, UpdateTick()
	DoTimer(100, &Room::UpdateTick);
}

void Room::FlushMoves()
{
	JOB_TAG();

	// 몬스터 이동을 같은 틱에 적분해 같은 S_MOVE 에 싣는다 - 패킷 · Send 수가 늘지 않는다 (monster-ai.md Core Rule 25)
	StepMonsters(Utils::NowMicroseconds());

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
	JOB_TAG();
	const uint64 nowUs = Utils::NowMicroseconds();

	// 이 Flush 에서 해소되는 히트는 동시다 - 사망은 모았다가 아래에서 한꺼번에 (TR-combat-022 "동시 치사는 양쪽 모두 사망")
	_kills.Begin();

	UpdateCc(nowUs);
	ResolveAttacks(nowUs);
	ResolveCasts(nowUs);
	ResolveTelegraphs(nowUs);

	for (const auto& kill : _kills.End())
	{
		if (kill.mutual && kill.victimId < kill.instigatorId)
		{
			GStats.AddMutualKill();
			LOG_LINE << "[DIED] mutual " << kill.victimId << " <-> " << kill.instigatorId << '\n';
		}

		_attackers.erase(kill.victimId);
		OnCreatureKilled(kill.victim, kill.instigator, kill.instigatorId, nowUs);
	}

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
	JOB_TAG();
	if (_pendingSpawns.empty() == false)
	{
		// 플레이어와 가방 · 상자를 다른 패킷에 담는다 - 클라 처리는 같고(S_SPAWN 하나), 송신 통계에서
		// 가방 몫을 따로 셀 수 있다 (G2 · SV-5, STAT_ID_BAG_SPAWN). 둘 다 있으면 패킷이 하나 늘 뿐이다.
		Protocol::S_SPAWN newPkt;
		Protocol::S_SPAWN newBagPkt;
		unordered_set<uint64> newIds;
		bool newPlayer = false;

		for (uint64 id : _pendingSpawns)
		{
			auto findIt = _objects.find(id);
			if (findIt == _objects.end())
				continue;

			if (CreatureRef creature = dynamic_pointer_cast<Creature>(findIt->second))
			{
				creature->objectInfo->set_hp(creature->hp);
				creature->objectInfo->set_max_hp(creature->maxHp);
			}

			if (findIt->second->IsBag())
			{
				newBagPkt.add_players()->CopyFrom(*findIt->second->objectInfo);
			}
			else
			{
				newPkt.add_players()->CopyFrom(*findIt->second->objectInfo);
				newPlayer = newPlayer || findIt->second->IsPlayer();
			}

			newIds.insert(id);
		}

		_pendingSpawns.clear();

		if (newIds.empty() == false)
		{
			// 신규 전원을 한 패킷에 담아 기존 전원에게 1회 브로드캐스트
			// 직렬화 1회
			if (newPkt.players_size() > 0)
			{
				SendBufferRef newBuffer = ServerPacketHandler::MakeSendBuffer(newPkt);
				Broadcast(newBuffer, newIds);
			}

			if (newBagPkt.players_size() > 0)
			{
				SendBufferRef newBagBuffer = ServerPacketHandler::MakeSendBuffer(newBagPkt);
				newBagBuffer->SetStatId(STAT_ID_BAG_SPAWN);
				Broadcast(newBagBuffer, newIds);
			}

			// 전체 목록을 1회만 직렬화해 신규 전원이 같은 버퍼를 나눠쓴다. 받을 신규 플레이어가 없으면 만들지 않는다.
			if (newPlayer)
			{
				Protocol::S_SPAWN allPkt;
				Protocol::S_SPAWN allBagPkt;

				for (auto& item : _objects)
				{
					if (item.second->IsPlayer() || item.second->IsMonster())
					{
						// 체력은 스냅샷 순간 값 - 이후는 S_DAMAGE. 시체 유예 중인 몬스터는 보내지 않는다(곧 사라진다)
						CreatureRef creature = static_pointer_cast<Creature>(item.second);
						if (creature->IsMonster() && creature->IsAlive() == false)
							continue;

						creature->objectInfo->set_hp(creature->hp);
						creature->objectInfo->set_max_hp(creature->maxHp);
						allPkt.add_players()->CopyFrom(*creature->objectInfo);
					}
					else if (item.second->IsBag())
						allBagPkt.add_players()->CopyFrom(*item.second->objectInfo);
				}

				vector<SendBufferRef> allBuffers;
				if (allPkt.players_size() > 0)
					allBuffers.push_back(ServerPacketHandler::MakeSendBuffer(allPkt));

				if (allBagPkt.players_size() > 0)
				{
					allBuffers.push_back(ServerPacketHandler::MakeSendBuffer(allBagPkt));
					allBuffers.back()->SetStatId(STAT_ID_BAG_SPAWN);
				}

				for (uint64 id : newIds)
				{
					auto findIt = _objects.find(id);
					if (findIt == _objects.end())
						continue;

					PlayerRef player = dynamic_pointer_cast<Player>(findIt->second);
					if (player == nullptr)
						continue;

					if (auto session = player->session.lock())
					{
						for (const SendBufferRef& buffer : allBuffers)
							session->Send(buffer);
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

void Room::SendChestSnapshot(const PlayerRef& player)
{
	Protocol::S_CHEST_STATE pkt;
	pkt.set_snapshot(true);
	for (auto& [bagId, bag] : _bags)
	{
		if (bag->IsChest() && bag->lootedAtUs != 0)
			pkt.add_looted_ids(bagId);
	}

	SEND_PACKET_DECLARATION(pkt);
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

	if (victim->runDeadlineUs != 0)
	{
		WriteRunTelemetry(victim, cause == Protocol::DEATH_CAUSE_NONE ? "DISCONNECT_DEATH" : "DIED", instigatorId, 0, Utils::NowMicroseconds());
		victim->runDeadlineUs = 0;
	}

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

		if (target->IsPlayer() && static_pointer_cast<Player>(target)->IsSpawnInvulnerable(Utils::NowMicroseconds()))
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

			// 몬스터 광역은 몬스터를 맞히지 않는다 - 보스 내려치기가 병사를 죽이지 않게 (monster-ai.md Core Rule 18 ②)
			if (caster->IsMonster() && target->IsMonster())
				continue;

			// 진입 무적 (AC-10)
			if (target->IsPlayer() && static_pointer_cast<Player>(target)->IsSpawnInvulnerable(Utils::NowMicroseconds()))
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

				if (target->IsMonster())
					OnMonsterDamaged(static_pointer_cast<Monster>(target), casterId, damage, nowUs);

				if (caster->IsMonster())
				{
					MonsterBrain::NoteEngaged(static_pointer_cast<Monster>(caster)->brain, nowUs);
					GMonsterStats.hits.fetch_add(1);
				}

				if (remainingHp <= 0)
					KillCreature(target, caster, casterId, nowUs);
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
		// 상자는 만료가 없다 - 리셋은 UpdateChests 가 한다
		if (bag->IsChest() || nowUs < bag->expiresAtUs)
			continue;

		bagIds.push_back(bagId);

		// DB 행이 없는 것(디버그 가방의 롤 아이템)은 지울 것이 없다
		for (const ItemEntry& e : bag->items)
		{
			if (e.isNew == false)
				instanceIds.push_back(e.instanceId);
		}
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

void Room::UpdateChests(uint64 nowUs)
{
	for (auto& [bagId, bag] : _bags)
	{
		if (bag->IsDueForReset(nowUs) == false)
			continue;

		// 남은 내용물은 롤 아이템이라 DB 행이 없다 - 메모리에서 버리면 끝이다.
		const size_t discarded = bag->ResetChest();

		// 사라졌다가 다시 나타나게 한다. 닫기 전용 패킷이 없어서, 열어 둔 창은 "사라짐"으로 받아 문구 없이
		// 닫힌다(클라 2026-10-04 결정). 다시 나타나는 것은 다음 FlushSpawns(33ms 안)다.
		Protocol::S_DESPAWN despawnPkt;
		despawnPkt.add_object_ids(bagId);
		SEND_PACKET_DECLARATION(despawnPkt);
		sendBuffer->SetStatId(STAT_ID_BAG_DESPAWN);
		Broadcast(sendBuffer);

		_pendingSpawns.push_back(bagId);

		LOG_LINE << "[CHEST] reset chest=" << bag->chestId << " discarded=" << discarded << '\n';
	}
}

void Room::FindSpawnPosition(float baseX, float baseY, float& outX, float& outY)
{
	// 고리 6겹을 돌고, 다 차면 반경 900 cm 원 안 (SpawnPlacement.h - 예전 폴백은 ±900 cm 정사각형이었다)
	const float spacingSq = SpawnPlacement::SPACING_CM * SpawnPlacement::SPACING_CM;

	auto occupied = [&](float cx, float cy)
	{
		for (auto& item : _objects)
		{
			const float dx = item.second->posInfo->x() - cx;
			const float dy = item.second->posInfo->y() - cy;

			if ((dx * dx + dy * dy) < spacingSq)
				return true;
		}
		return false;
	};

	SpawnPlacement::Find(baseX, baseY, occupied, [] { return Utils::GetRandom(0.f, 1.f); }, outX, outY);
}

void Room::ClearSpawnInvulnerability(const PlayerRef& player, const char* by)
{
	if (player->spawnInvulnUntilUs == 0)
		return;

	const uint64 nowUs = Utils::NowMicroseconds();
	LOG_LINE << "[INVULN] cleared char=" << player->objectInfo->object_id() << " by=" << by << " left="
		<< (player->spawnInvulnUntilUs > nowUs ? (player->spawnInvulnUntilUs - nowUs) / 1000 : 0) << "ms\n";
	player->spawnInvulnUntilUs = 0;
}

void Room::OnEquipmentChanged(const PlayerRef& player, const char* tag)
{
	player->RebuildSkillSlotsFromEquipment();
	LogSlots(player, tag);

	// 치유 패킷이 없다 - 몬스터 귀환과 같은 방식으로 피해 0 · 남은 체력을 보낸다 (클라 OnDamaged 가 재동기화로 읽는다)
	if (player->ApplyEquipmentStats(false) && player->IsAlive())
	{
		Protocol::DamageInfo& resync = _pendingDamages.emplace_back();
		resync.set_attacker_id(0);
		resync.set_target_id(player->objectInfo->object_id());
		resync.set_damage(0);
		resync.set_remaining_hp(player->hp);
	}

	LogStats(player, tag);
}

void Room::KillCreature(const CreatureRef& victim, const CreatureRef& instigator, uint64 instigatorId, uint64 nowUs)
{
	if (_kills.IsOpen())
	{
		_kills.Note(victim->objectInfo->object_id(), instigatorId, victim, instigator);
		return;
	}

	_attackers.erase(victim->objectInfo->object_id());
	OnCreatureKilled(victim, instigator, instigatorId, nowUs);
}

bool Room::CanStillStrike(const CreatureRef& creature) const
{
	return creature->IsAlive() || _kills.DiedThisFlush(creature->objectInfo->object_id());
}

void Room::OnCreatureKilled(const CreatureRef& victim, const CreatureRef& instigator, uint64 instigatorId, uint64 nowUs)
{
	const uint64 victimId = victim->objectInfo->object_id();

	// 원인은 가해자 기준이다 - 예전에는 피해자 종류를 넣어 몬스터에게 죽은 플레이어가 PLAYER 로 나갔다 (monster-ai.md Core Rule 19)
	Protocol::DiedInfo& died = _pendingDeaths.emplace_back();
	died.set_victim_id(victimId);
	died.set_instigator_id(instigatorId);
	died.set_cause((instigator != nullptr && instigator->IsMonster()) ? Protocol::DEATH_CAUSE_MONSTER : Protocol::DEATH_CAUSE_PLAYER);

	// 죽은 대상이 공격 중이었다면 그 공격도 없앤다.
	victim->attackTargetId = 0;
	victim->attackHitAtUs = 0;
	CancelCast(victim);

	LOG_LINE << "[DIED] victim=" << victimId << " by=" << instigatorId << '\n';

	if (victim->IsMonster())
	{
		OnMonsterDied(static_pointer_cast<Monster>(victim), instigatorId, nowUs);
		GMonsterStats.deaths.fetch_add(1);
	}
	else
	{
		if (instigator != nullptr && instigator->IsMonster())
			GMonsterStats.playerKills.fetch_add(1);

		if (PlayerRef player = dynamic_pointer_cast<Player>(victim); player != nullptr && player->runDeadlineUs != 0)
		{
			WriteRunTelemetry(player, "DIED", instigatorId, 0, nowUs);
			player->runDeadlineUs = 0;
		}

		BeginDeathCommit(victim);
	}
}

void Room::InitMonsters()
{
	JOB_TAG();
	const uint64 nowUs = Utils::NowMicroseconds();

	_monsterSlots.clear();
	for (const MonsterSpawnDef& spawn : MonsterTable::Spawns())
	{
		if (spawn.floorId != _floorId)
			continue;

		MonsterSlot& slot = _monsterSlots.emplace_back();
		slot.spawn = &spawn;
	}

	// 재시작 = 모든 런 무효 - 전 자리 만피 Idle 로 바로 태어난다 (Core Rule 22)
	for (MonsterSlot& slot : _monsterSlots)
		SpawnMonster(slot, nowUs);

	LOG_LINE << "[MON] floor=" << _floorId << " spawns=" << _monsterSlots.size() << '\n';
}

void Room::SpawnMonster(MonsterSlot& slot, uint64 nowUs)
{
	const MonsterTypeDef* type = MonsterTable::FindType(slot.spawn->typeId);
	if (type == nullptr)
		return;

	const uint64 id = GNextMonsterId.fetch_add(1);
	MonsterRef monster = make_shared<Monster>(id, *type, *slot.spawn);
	MonsterBrain::Reset(monster->brain, nowUs);
	monster->lastStepUs = nowUs;

	if (AddObject(monster) == false)
		return;

	_monsters[id] = monster;
	slot.monsterId = id;
	slot.deferStartUs = 0;
	_pendingSpawns.push_back(id);
	GMonsterStats.alive.fetch_add(1);

	LOG_LINE << "[MON] spawn id=" << id << " spawn=" << slot.spawn->spawnId << " type=" << type->nameKey
		<< " pos=(" << slot.spawn->x << "," << slot.spawn->y << ")\n";
}

void Room::UpdateMonsters(uint64 nowUs)
{
	if (_monsterSlots.empty())
		return;

	// ── 시체 유예가 끝난 몬스터를 방에서 뺀다 ──
	{
		Protocol::S_DESPAWN despawnPkt;
		for (auto it = _monsters.begin(); it != _monsters.end(); )
		{
			const MonsterRef& m = it->second;
			if (m->diedAtUs != 0 && nowUs >= m->diedAtUs + MonsterConfig::CORPSE_LINGER_US)
			{
				despawnPkt.add_object_ids(it->first);
				RemoveObject(it->first);
				it = _monsters.erase(it);
				continue;
			}
			++it;
		}

		if (despawnPkt.object_ids_size() > 0)
		{
			SEND_PACKET_BROADCAST(despawnPkt);
		}
	}

	// ── 유효 목표 - 방 안 · 살아 있는 플레이어 (끊긴 몸 포함, § 12). 틱마다 한 번 ──
	_brainPlayers.clear();
	for (auto& [id, object] : _objects)
	{
		if (object->IsPlayer() == false)
			continue;

		// 진입 무적 중인 플레이어는 감지 · 목표가 아니다 (monster-ai.md Core Rule 12)
		const Player* player = static_cast<const Player*>(object.get());
		if (player->IsAlive() && player->IsSpawnInvulnerable(nowUs) == false)
			_brainPlayers.push_back({ id, player->posInfo->x(), player->posInfo->y() });
	}

	// ── 리스폰 - 자리마다. 곁(12 m)에 플레이어가 있으면 최대 60 초 미룬다 (F9) ──
	for (MonsterSlot& slot : _monsterSlots)
	{
		if (slot.monsterId != 0 || nowUs < slot.respawnAtUs)
			continue;

		bool playerClose = false;		// "near" 는 windows.h 매크로다
		const float r2 = MonsterConfig::RESPAWN_DEFER_RADIUS_CM * MonsterConfig::RESPAWN_DEFER_RADIUS_CM;
		for (const BrainPlayer& p : _brainPlayers)
		{
			const float dx = p.x - slot.spawn->x;
			const float dy = p.y - slot.spawn->y;
			if (dx * dx + dy * dy <= r2)
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
				LOG_LINE << "[MON] respawn deferred spawn=" << slot.spawn->spawnId << " - player within 12 m\n";
			}
			continue;
		}

		SpawnMonster(slot, nowUs);
	}

	// ── 판단 ──
	const uint64 startUs = Utils::NowMicroseconds();
	const WallMap& walls = WallTable::Get(_floorId);
	auto lineClear = [&walls](float ax, float ay, float bx, float by) { return walls.SweepBlocked(ax, ay, bx, by, 0.f) == false; };

	for (auto& [id, m] : _monsters)
	{
		if (m->diedAtUs != 0)
			continue;

		BrainInput in;
		in.nowUs = nowUs;
		in.x = m->posInfo->x();
		in.y = m->posInfo->y();
		in.homeX = m->spawn->x;
		in.homeY = m->spawn->y;
		in.windingUp = m->attackHitAtUs != 0;
		in.casting = m->IsCasting();
		in.players = &_brainPlayers;
		in.lineClear = lineClear;

		const MonsterState before = m->brain.state;
		m->decision = MonsterBrain::Think(m->brain, *m->type, in);
		const BrainOutput& out = m->decision;

		if (before != m->brain.state)
		{
			LOG_LINE << "[MON] state id=" << id << " " << MonsterStateName(before) << "->" << MonsterStateName(m->brain.state)
				<< " target=" << m->brain.targetId << '\n';
		}

		if (out.resetOnArrive)
		{
			// T11 · T12 - 만피 · CC 해제. 클라는 피해 0 · 남은 체력 = 최대 로 체력바를 되돌린다 (치유 패킷이 없다)
			m->RestoreFull();

			Protocol::DamageInfo& heal = _pendingDamages.emplace_back();
			heal.set_attacker_id(0);
			heal.set_target_id(id);
			heal.set_damage(0);
			heal.set_remaining_hp(m->hp);

			if (out.snapHome)
			{
				m->posInfo->set_x(m->spawn->x);
				m->posInfo->set_y(m->spawn->y);
				m->posInfo->set_state(Protocol::MOVE_STATE_IDLE);
				_dirtyMovers.insert(id);
				GMonsterStats.resetSnaps.fetch_add(1);
				LOG_LINE << "[MON] reset_snap id=" << id << " spawn=" << m->spawn->spawnId << '\n';
			}
		}

		// T10 - 같은 무리의 쉬는 동료를 부른다
		if (out.aggroed && m->spawn->groupId != 0 && m->type->assistRadiusCm > 0.f)
		{
			const float r2 = m->type->assistRadiusCm * m->type->assistRadiusCm;
			for (auto& [otherId, other] : _monsters)
			{
				if (otherId == id || other->diedAtUs != 0 || other->spawn->groupId != m->spawn->groupId)
					continue;

				const float dx = other->posInfo->x() - in.x;
				const float dy = other->posInfo->y() - in.y;
				if (dx * dx + dy * dy <= r2 && MonsterBrain::Alert(other->brain, m->brain.targetId, nowUs))
					LOG_LINE << "[MON] assist id=" << otherId << " by=" << id << " target=" << m->brain.targetId << '\n';
			}
		}

		if (out.castSkill)
		{
			if (const SkillDef* def = SkillTable::Find(m->type->skillId))
				BeginMonsterCast(m, *def, nowUs);
		}
		else if (out.attackTargetId != 0)
		{
			HandleAttack(id, out.attackTargetId);
		}
	}

	GMonsterStats.RecordAi(Utils::NowMicroseconds() - startUs);
}

void Room::StepMonsters(uint64 nowUs)
{
	if (_monsters.empty())
		return;

	const WallMap& walls = WallTable::Get(_floorId);
	auto blocked = [&walls](float ax, float ay, float bx, float by, float r) { return walls.SweepBlocked(ax, ay, bx, by, r); };

	uint32 entries = 0;

	for (auto& [id, m] : _monsters)
	{
		if (m->diedAtUs != 0)
			continue;

		const uint64 dtUs = (m->lastStepUs != 0 && nowUs > m->lastStepUs) ? nowUs - m->lastStepUs : 0;
		m->lastStepUs = nowUs;

		const BrainOutput& d = m->decision;
		Protocol::PosInfo* pos = m->posInfo;

		if (d.hasGoal == false)
		{
			if (pos->state() == Protocol::MOVE_STATE_RUN)
			{
				pos->set_state(Protocol::MOVE_STATE_IDLE);
				_dirtyMovers.insert(id);
				entries++;
			}
			continue;
		}

		// F1 - CC · windup · 시전이면 0. 몬스터는 걸으면서 때리지 않는다 (Core Rule 17)
		float speed = m->type->moveSpeedCms * (1.f - m->activeSlow) * d.speedMult;
		if (m->CanMove(nowUs) == false || m->attackHitAtUs != 0 || m->IsCasting())
			speed = 0.f;

		StepInput in;
		in.nowUs = nowUs;
		in.dtUs = dtUs;
		in.x = pos->x();
		in.y = pos->y();
		in.goalX = d.goalX;
		in.goalY = d.goalY;
		in.stopDistCm = d.stopDistCm;
		in.speedCms = speed;
		in.radiusCm = m->type->radiusCm;
		in.blocked = blocked;

		const StepOutput o = MonsterMover::Step(m->brain, in);

		if (o.moved)
		{
			pos->set_x(o.x);
			pos->set_y(o.y);
			pos->set_yaw(o.yawDeg);
			pos->set_state(Protocol::MOVE_STATE_RUN);
			_dirtyMovers.insert(id);
			entries++;
			continue;
		}

		// 멈춘 첫 걸음은 한 번 더 보낸다 - 클라가 정지 애니메이션으로 바꾼다. 서 있으면 목표 쪽을 본다
		const float faceYaw = ::atan2f(d.goalY - pos->y(), d.goalX - pos->x()) * 180.f / 3.14159265f;
		float diff = ::fmodf(faceYaw - pos->yaw() + 540.f, 360.f) - 180.f;
		const bool turn = ::fabsf(diff) > 10.f && (d.goalX != pos->x() || d.goalY != pos->y());

		if (pos->state() == Protocol::MOVE_STATE_RUN || turn)
		{
			if (turn)
				pos->set_yaw(faceYaw);
			pos->set_state(Protocol::MOVE_STATE_IDLE);
			_dirtyMovers.insert(id);
			entries++;
		}
	}

	GMonsterStats.moveEntries.fetch_add(entries);
}

void Room::OnMonsterDamaged(const MonsterRef& monster, uint64 attackerId, int32 damage, uint64 nowUs)
{
	float ax = monster->posInfo->x();
	float ay = monster->posInfo->y();

	auto findIt = _objects.find(attackerId);
	if (findIt != _objects.end())
	{
		ax = findIt->second->posInfo->x();
		ay = findIt->second->posInfo->y();
	}

	MonsterBrain::OnDamaged(monster->brain, attackerId, damage, ax, ay, nowUs);
}

void Room::OnMonsterDied(const MonsterRef& monster, uint64 instigatorId, uint64 nowUs)
{
	const uint64 id = monster->objectInfo->object_id();
	const MonsterTypeDef& type = *monster->type;

	MonsterBrain::Kill(monster->brain, nowUs);
	monster->decision = BrainOutput();
	monster->diedAtUs = nowUs;
	monster->posInfo->set_state(Protocol::MOVE_STATE_IDLE);
	GMonsterStats.alive.fetch_sub(1);

	for (MonsterSlot& slot : _monsterSlots)
	{
		if (slot.monsterId == id)
		{
			slot.monsterId = 0;
			slot.respawnAtUs = nowUs + static_cast<uint64>(slot.spawn->respawnSeconds) * 1'000'000;
			slot.deferStartUs = 0;
			break;
		}
	}

	// ── 드랍 - 주인 없는 메모리 전용 가방, 먼저 여는 사람이 임자 (Core Rule 20) ──
	size_t dropped = 0;
	{
		vector<ItemEntry> items = MonsterDrop::Roll(type, type.drops ? FloorItemPool::Find(_floorId, type.dropTier) : nullptr,
			_rng, []() { return GNextInstanceId.fetch_add(1); });

		if (items.empty() == false)
		{
			// 시체가 벽에 붙어 있으면 가방이 벽 안에 생긴다 - 가까운 빈 자리로. 못 찾으면 집(자리 검증이 벽 밖을 보장)
			float bx = monster->posInfo->x();
			float by = monster->posInfo->y();
			if (WallTable::Get(_floorId).FindNearestFree(bx, by, MonsterConfig::DROP_FREE_RADIUS_CM, MonsterConfig::DROP_FREE_SEARCH_CM, OUT bx, OUT by) == false)
			{
				bx = monster->spawn->x;
				by = monster->spawn->y;
			}

			BagRef bag = ObjectUtils::CreateBag(GNextBagId.fetch_add(1), _floorId, bx, by, monster->posInfo->z(),
				nowUs + MonsterConfig::DROP_BAG_LIFETIME_US, /*owner*/ 0);
			bag->items = move(items);
			dropped = bag->items.size();
			AddBag(bag);
		}
	}

	LOG_LINE << "[MON] died id=" << id << " spawn=" << monster->spawn->spawnId << " type=" << type.nameKey
		<< " by=" << instigatorId << " drop=" << dropped << " respawn_in=" << monster->spawn->respawnSeconds << "s\n";
}

bool Room::BeginMonsterCast(const MonsterRef& monster, const SkillDef& def, uint64 nowUs)
{
	// HandleSkill 의 꼬리를 슬롯 검사 없이 - 몬스터는 6 슬롯이 없다 (skill-system Q5 · monster-ai.md Core Rule 18 ①)
	if (monster->CanUseSkill(nowUs) == false || monster->IsCasting())
		return false;

	const uint64 id = monster->objectInfo->object_id();

	if (monster->attackHitAtUs != 0)
	{
		monster->attackTargetId = 0;
		monster->attackHitAtUs = 0;
		_attackers.erase(id);
		_pendingAttackCancels.push_back(id);
	}

	monster->castSkillId = def.skillId;
	monster->castSlot = Protocol::SLOT_NONE;
	monster->castTargetId = 0;
	monster->castAimX = monster->posInfo->x();
	monster->castAimY = monster->posInfo->y();
	monster->castEndAtUs = nowUs + static_cast<uint64>(def.castMs) * 1000;
	_casters.insert(id);

	if (def.castMs > 0)
	{
		Protocol::SkillCastInfo& info = _pendingCasts.emplace_back();
		info.set_caster_id(id);
		info.set_is_stationary(def.canMoveWhileCasting == false);
	}

	monster->brain.skillReadyAtUs = nowUs + static_cast<uint64>(def.castMs + def.cooldownMs) * 1000;

	LOG_LINE << "[MON] cast id=" << id << " skill=" << def.skillId << '\n';
	return true;
}

/*-------- 탈출 & 체류 타이머 (design/gdd/escape-timer.md) --------*/

void Room::InitEscape()
{
	JOB_TAG();
	_towers.clear();

	for (const TowerDef& def : TowerTable::All())
	{
		if (def.floorId != _floorId)
			continue;

		EscapeTowerState& t = _towers.emplace_back();
		t.towerId = def.towerId;
		t.x = def.x;
		t.y = def.y;
	}

	// 와이어 id = CSV 문자열 id 오름차순 순번 (Ta=1 · Tb=2 · Tc=3) - Core Rule 1
	sort(_towers.begin(), _towers.end(), [](const EscapeTowerState& a, const EscapeTowerState& b) { return a.towerId < b.towerId; });
	if (static_cast<int32>(_towers.size()) > Player::MAX_TOWERS)
		_towers.resize(Player::MAX_TOWERS);
	for (size_t i = 0; i < _towers.size(); i++)
		_towers[i].wireId = static_cast<uint32>(i + 1);

	if (_towers.empty())
		LOG_LINE << "[ESCAPE] floor=" << _floorId << " towers=0 (TEMP)\n";
	else
		LOG_LINE << "[ESCAPE] floor=" << _floorId << " towers=" << _towers.size() << '\n';
}

void Room::DebugAddRunGold(uint64 playerId, uint64 amount)
{
	JOB_TAG();
	auto findIt = _objects.find(playerId);
	if (findIt == _objects.end() || findIt->second->IsPlayer() == false)
		return;

	PlayerRef player = static_pointer_cast<Player>(findIt->second);
	player->runGold += amount;
	LOG_LINE << "[GOLD] debug run gold char=" << playerId << " +" << amount << " = " << player->runGold << '\n';
}

void Room::SendGauge(const PlayerRef& player, uint32 towerWireId, Protocol::EscapeGaugeState state, uint32 permille, float ratePerS, bool selfInZone)
{
	GameSessionRef session = player->session.lock();
	if (session == nullptr)
		return;

	Protocol::S_ESCAPE_GAUGE pkt;
	pkt.set_tower_id(towerWireId);
	pkt.set_state(state);
	pkt.set_progress_permille(permille);
	pkt.set_rate_per_s(ratePerS);
	pkt.set_self_in_zone(selfInZone);

	SEND_PACKET_DECLARATION(pkt);
	session->Send(sendBuffer);
}

void Room::UpdateEscapeTowers(uint64 nowUs)
{
	if (_towers.empty())
		return;

	const EscapeConfig& cfg = EscapeConfig::Get();
	const uint64 dtUs = (_lastEscapeTickUs != 0 && nowUs > _lastEscapeTickUs) ? nowUs - _lastEscapeTickUs : 0;
	_lastEscapeTickUs = nowUs;

	// 점유자 · 받는 사람 후보 = 살아 있고 세션이 붙은 플레이어 (끊긴 몸 · 시체 · 몬스터 제외, Core Rule 4)
	vector<PlayerRef> players;
	for (auto& [id, object] : _objects)
	{
		if (object->IsPlayer() == false)
			continue;

		PlayerRef p = static_pointer_cast<Player>(object);
		if (p->IsAlive() && p->detachedUntilUs == 0 && p->session.lock() != nullptr)
			players.push_back(p);
		else
			p->zoneTower = -1;
	}

	for (int32 i = 0; i < static_cast<int32>(_towers.size()); i++)
	{
		EscapeTowerState& t = _towers[i];

		struct Seen { PlayerRef p; float d; bool wasIn; bool in; };
		vector<Seen> seen;
		seen.reserve(players.size());
		vector<uint64> occupants;

		for (const PlayerRef& p : players)
		{
			const float dx = p->posInfo->x() - t.x;
			const float dy = p->posInfo->y() - t.y;
			const float d = ::sqrtf(dx * dx + dy * dy);
			const bool wasIn = (p->zoneTower == i);
			const bool in = EscapeLogic::InZone(wasIn, d, cfg);

			if (in && wasIn == false)
			{
				p->zoneTower = i;
				p->statZoneVisits++;
				p->statLastTowerId = t.towerId;
			}
			else if (in == false && wasIn)
			{
				p->zoneTower = -1;
				// 나가기 직전 1 초 안에 넉백을 받았으면 밀려난 것 (텔레메트리 망설임 지표, § G)
				if (p->knockbackEndUs + 1'000'000 > nowUs)
					p->statZoneLeavesForced++;
				else
					p->statZoneLeavesVoluntary++;
			}

			if (in)
			{
				occupants.push_back(p->objectInfo->object_id());
				p->statZoneDwellUs += dtUs;
			}
			else if (d <= cfg.alertCm)
			{
				p->statAlertDwellUs += dtUs;
			}

			seen.push_back({ p, d, wasIn, in });
		}

		sort(occupants.begin(), occupants.end());

		const EscapeLogic::StepResult r = EscapeLogic::Step(t.gauge, static_cast<int32>(occupants.size()), dtUs, cfg);
		const bool stateChanged = (r.before != r.after);
		const uint32 permille = r.completed ? 1000 : EscapeLogic::Permille(t.gauge, cfg);

		if (stateChanged)
		{
			const char* event = nullptr;
			if (r.completed)
				event = "COMPLETE";
			else if (r.before == Protocol::ESCAPE_GAUGE_IDLE && r.after == Protocol::ESCAPE_GAUGE_FILLING)
				event = "FILL_START";
			else if (r.before == Protocol::ESCAPE_GAUGE_FILLING && r.after == Protocol::ESCAPE_GAUGE_DRAINING)
				event = "REVERSAL";
			else if (r.before == Protocol::ESCAPE_GAUGE_DRAINING && r.after == Protocol::ESCAPE_GAUGE_FILLING)
				event = "RESUME";
			else if (r.after == Protocol::ESCAPE_GAUGE_IDLE)
				event = "DRAIN_END";

			if (event != nullptr)
			{
				GTelemetry.AppendGauge(_floorId, t.towerId, event, permille, static_cast<uint32>(occupants.size()));
				LOG_LINE << "[ESCAPE] gauge tower=" << t.towerId << " " << event << " permille=" << permille << " occupants=" << occupants.size() << '\n';
			}

			// 역행 - 직전 틱 점유자(마지막으로 떠난 사람)가 "게이지를 버렸다"
			if (r.before == Protocol::ESCAPE_GAUGE_FILLING && r.after == Protocol::ESCAPE_GAUGE_DRAINING)
			{
				for (uint64 id : t.lastOccupants)
				{
					auto it = _objects.find(id);
					if (it != _objects.end() && it->second->IsPlayer())
						static_pointer_cast<Player>(it->second)->statGaugeReversals++;
				}
			}
		}

		const Protocol::EscapeGaugeState alertNow =
			(t.gauge.state == Protocol::ESCAPE_GAUGE_FILLING && r.completed == false) ? Protocol::ESCAPE_GAUGE_FILLING : Protocol::ESCAPE_GAUGE_IDLE;
		const bool alertChanged = (alertNow != t.alertState);
		t.alertState = alertNow;

		const Protocol::EscapeGaugeState shownState = r.completed ? Protocol::ESCAPE_GAUGE_COMPLETED : t.gauge.state;
		const float rate = EscapeLogic::RatePerSecond(shownState, cfg);
		const bool moving = (t.gauge.state == Protocol::ESCAPE_GAUGE_FILLING || t.gauge.state == Protocol::ESCAPE_GAUGE_DRAINING);

		// 받는 사람마다 - 변화가 있을 때만 (Core Rule 11)
		for (const Seen& s : seen)
		{
			const EscapeLogic::Audience audience = EscapeLogic::AudienceFor(s.in, s.d, cfg);
			const EscapeLogic::Audience prev = static_cast<EscapeLogic::Audience>(s.p->gaugeAudience[i]);
			uint64& lastSent = s.p->lastGaugeSentUs[i];

			if (audience == EscapeLogic::Audience::Full)
			{
				const bool heartbeat = moving && nowUs - lastSent >= cfg.heartbeatUs;
				if (prev != EscapeLogic::Audience::Full || stateChanged || r.completed || s.in != s.wasIn || heartbeat)
				{
					SendGauge(s.p, t.wireId, shownState, permille, rate, s.in);
					lastSent = nowUs;
				}
			}
			else
			{
				// 전체 집합에서 나갔다 - 보이던 링을 지우게 한다 (새 정보는 없다)
				if (prev == EscapeLogic::Audience::Full)
					SendGauge(s.p, t.wireId, Protocol::ESCAPE_GAUGE_IDLE, 0, 0.f, false);

				if (audience == EscapeLogic::Audience::Alert)
				{
					// 경보 - 타워 · 진행도 · 방향 없이 "누가 채우기 시작했다 / 끝났다"만 (Core Rule 10)
					if ((prev != EscapeLogic::Audience::Alert && alertNow == Protocol::ESCAPE_GAUGE_FILLING)
						|| (prev == EscapeLogic::Audience::Alert && alertChanged))
					{
						SendGauge(s.p, 0, alertNow, 0, 0.f, false);
					}
				}
				else if (prev == EscapeLogic::Audience::Alert)
				{
					SendGauge(s.p, 0, Protocol::ESCAPE_GAUGE_IDLE, 0, 0.f, false);
				}
			}

			s.p->gaugeAudience[i] = static_cast<uint8>(audience);
		}

		t.lastOccupants = occupants;

		// 완충 - 그 틱의 점유자 전원을 id 오름차순으로 탈출 커밋 (Core Rule 9, AC-ET-08). 이미 죽는 중이면 건너뛴다
		if (r.completed)
		{
			for (uint64 id : occupants)
				BeginEscapeCommit(id);

			t.lastOccupants.clear();
		}
	}
}

void Room::UpdateRunTimers(uint64 nowUs)
{
	vector<PlayerRef> expired;
	for (auto& [id, object] : _objects)
	{
		if (object->IsPlayer() == false)
			continue;

		PlayerRef p = static_pointer_cast<Player>(object);
		if (p->IsAlive() && EscapeLogic::Expired(p->runDeadlineUs, nowUs))
			expired.push_back(p);
	}

	for (const PlayerRef& p : expired)
		BeginExpiryCommit(p, nowUs);
}

void Room::BeginExpiryCommit(const PlayerRef& player, uint64 nowUs)
{
	const uint64 playerId = player->objectInfo->object_id();

	// 끊긴 몸 - 재접속과 경쟁한다. 먼저 꺼낸 쪽만 이긴다 (Core Rule 16-1, AC-ET-11)
	if (player->detachedUntilUs != 0)
	{
		if (GDetachedBodies.TryTake(playerId) == nullptr)
			return;
		_detached.erase(playerId);
	}

	// 이미 죽는 중 · 탈출 중이면 그 결과가 이긴다 (AC-ET-21 · 22)
	if (GDeathCommits.TryBegin(playerId) == false)
		return;

	ExpirySnapshot snap;
	snap.characterId = playerId;
	for (const ItemEntry& e : player->inventory)
	{
		snap.itemsDestroyed++;
		if (e.isNew == false)
			snap.rowIds.push_back(e.instanceId);
	}
	for (const ItemEntry& e : player->equipped)
	{
		if (e.instanceId == 0)
			continue;
		snap.itemsDestroyed++;
		if (e.isNew == false)
			snap.rowIds.push_back(e.instanceId);
	}
	snap.rowIds.insert(snap.rowIds.end(), player->droppedIds.begin(), player->droppedIds.end());
	snap.goldKept = EscapeLogic::GoldKept(player->runGold, EscapeConfig::Get().expiryGoldKeptPct);

	WriteRunTelemetry(player, "EXPIRED", 0, snap.goldKept, nowUs);
	player->runDeadlineUs = 0;

	// 탈출 대기와 같다 - 방에서 빼고, 커밋이 끝난 뒤 베이스캠프로. 전투 사망 경로(S_DIED · 가방)를 거치지 않는다
	GameSessionRef session = player->session.lock();
	CancelCast(player);
	SavePlayer(player);
	RemoveObject(playerId);
	if (session != nullptr)
		session->place.store(SessionPlace::Escaping);

	Protocol::S_DESPAWN despawnPkt;
	despawnPkt.add_object_ids(playerId);
	SEND_PACKET_DECLARATION(despawnPkt);
	Broadcast(sendBuffer);

	LOG_LINE << "[EXPIRE] commit begin - char=" << playerId << " destroyed=" << snap.itemsDestroyed << " rows=" << snap.rowIds.size()
		<< " gold_kept=" << snap.goldKept << " detached=" << (session == nullptr ? 1 : 0) << '\n';

	GDBQueue.Push([session, snap](DBConnection* conn)
		{
			CommitExpiry(conn, snap);
			GDeathCommits.End(snap.characterId);

			LOG_LINE << "[EXPIRE] committed - char=" << snap.characterId << '\n';

			// 끊긴 몸이면 보낼 곳이 없다 - 다음 접속이 DB 에서 새로 적재한다
			if (session == nullptr)
				return;

			session->place.store(SessionPlace::BaseCamp);

			Protocol::S_LEAVE_GAME leavePkt;
			leavePkt.set_reason(Protocol::RUN_END_EXPIRED);
			leavePkt.set_gold_kept(snap.goldKept);
			leavePkt.set_items_destroyed(snap.itemsDestroyed);
			SEND_PACKET_DECLARATION(leavePkt);
			session->Send(sendBuffer);

			SendBaseCampInventory(conn, session, snap.characterId);
		});
}

void Room::WriteRunTelemetry(const PlayerRef& player, const char* reason, uint64 killerId, uint64 goldKept, uint64 nowUs)
{
	RunRow row;
	row.charId = player->objectInfo->object_id();
	row.floorId = _floorId;
	row.entryPoint = player->entryPointId;
	row.reason = reason;
	row.runMs = (player->runStartUs != 0 && nowUs > player->runStartUs) ? (nowUs - player->runStartUs) / 1000 : 0;
	row.broughtInSum = player->statBroughtInSum;

	auto account = [&row](const ItemEntry& e)
	{
		const uint32 grade = static_cast<uint32>(e.grade);
		row.endCarriedCount++;
		row.endCarriedSum += grade > 0 ? (1u << (grade - 1)) : 0u;
		row.endCarriedMaxGrade = max(row.endCarriedMaxGrade, grade);
	};
	for (const ItemEntry& e : player->inventory)
		account(e);
	for (const ItemEntry& e : player->equipped)
		if (e.instanceId != 0)
			account(e);

	row.runGold = player->runGold;
	row.goldKept = goldKept;
	row.zoneVisits = player->statZoneVisits;
	row.zoneLeavesVoluntary = player->statZoneLeavesVoluntary;
	row.zoneLeavesForced = player->statZoneLeavesForced;
	row.zoneDwellMs = player->statZoneDwellUs / 1000;
	row.gaugeReversalsPresent = player->statGaugeReversals;
	row.alertDwellMs = player->statAlertDwellUs / 1000;
	row.lastTowerId = player->statLastTowerId;
	row.killerId = killerId;

	GTelemetry.AppendRun(row);
}
