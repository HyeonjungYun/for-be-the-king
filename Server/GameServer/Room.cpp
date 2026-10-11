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
#include "SkillKinematics.h"
#include "Decoy.h"

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
		if (c.tauntEndUs > endUs) { endUs = c.tauntEndUs; reason = "taunt"; }

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
			|| type == Protocol::CC_TYPE_ROOT
			|| type == Protocol::CC_TYPE_TAUNT;
	}

	bool IsInSkillRange(const CreatureRef& caster, const CreatureRef& target, float rangeCm)
	{
		const float dx = target->posInfo->x() - caster->posInfo->x();
		const float dy = target->posInfo->y() - caster->posInfo->y();

		return (dx * dx + dy * dy) <= (rangeCm * rangeCm);
	}

	// requires=mark:X 가 보는 표식 종류 (skill_effects.csv param 과 같은 이름). 표식 조건이 아니면 nullptr
	const char* RequiredMark(SkillRequire condition)
	{
		switch (condition)
		{
		case SkillRequire::MarkSpear:		return "spear";
		case SkillRequire::MarkArrow:		return "arrow";
		case SkillRequire::MarkBlood:		return "blood";
		case SkillRequire::MarkHeavyBleed:	return "heavy_bleed";
		case SkillRequire::MarkRope:		return "rope";
		default:							return nullptr;
		}
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

// FoW 필터 방송 (Room.h) - 항목 목록 하나짜리 패킷. 이 파일에서만 쓰므로 정의도 여기
template <typename TPkt, typename TItem, typename AddFn, typename KeepFn>
void Room::SendListVisible(const vector<TItem>& items, AddFn&& add, KeepFn&& keep, uint16 statId)
{
	if (items.empty())
		return;

	auto finish = [statId](TPkt& pkt)
		{
			SendBufferRef buffer = ServerPacketHandler::MakeSendBuffer(pkt);
			if (statId != 0)
				buffer->SetStatId(statId);
			return buffer;
		};

	SendVisible(static_cast<int32>(items.size()),
		[&](const PlayerRef& v) { int32 n = 0; for (const TItem& item : items) n += keep(v, item) ? 1 : 0; return n; },
		[&]() { TPkt pkt; for (const TItem& item : items) add(pkt, item); return finish(pkt); },
		[&](const PlayerRef& v) { TPkt pkt; for (const TItem& item : items) if (keep(v, item)) add(pkt, item); return finish(pkt); });
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

// 투사체 id - 층을 가리지 않고 유일 (클라는 한 층만 보지만 로그에서 섞이지 않게). 50 억 대역 (몬스터 40 억 위)
atomic<uint64> GNextProjectileId = PROJECTILE_ID_BASE;

// 미끼 id - 60 억 대역. 오브젝트라 _objects 에 들어가므로 플레이어 · 몬스터 · 가방과 겹치면 안 된다
atomic<uint64> GNextDecoyId = DECOY_ID_BASE;

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

		// 지난 런의 CC · 상태 효과 · 재입력이 따라오지 않는다. 시야도 처음부터 (FoW - 이 클라는 아무것도 모른다)
		player->ClearCombatState();
		player->ResetVision();

		// 테스트 모드 무적 · FoW 끔도 런마다 꺼진다 - 클라 패널(새 폰)도 꺼진 상태로 시작한다
		player->testGod = false;
		player->testSeeAll = false;

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
		{
			SendChestSnapshot(player);
			SendDynamicWalls(player, enterUs);		// 지각변동 벽이 서 있으면 - 지형이라 보이든 말든 안다
		}

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

	// 넉백 · 끌기 중 - 서버가 위치를 끝점으로 옮겼고 클라는 KnockbackInfo 로 같은 끝점까지 밀려가는 중이다.
	// 되돌리지 않고 조용히 무시한다 (서버가 위치를 소유하는 구간). CC 가 끝난 뒤의 첫 이동이 끝점에서 검증된다 (스킬 로스터 1 단계)
	// 도발도 같다 - 서버가 매 틱 시전자 쪽으로 옮기고 보정 S_MOVE 로 알린다 (StepTaunts)
	if (blockCheckUs < player->knockbackEndUs || blockCheckUs < player->tauntEndUs)
		return;

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

	// 이동 스킬의 자기 변위 - 클라는 캐스트(60 ms)가 끝나는 순간 대시를 보내는데, 서버는 다음 FlushCombat(33 ms 간격)에야 창을 연다.
	// 그 사이에 온 이동은 속도 거부 + 캐스트 취소였다 (2026-10-10 UE 클라 Z 질주 - 봇은 100 ms 를 기다려 안 걸렸다).
	// 캐스트가 한 Flush 안에 끝나는 이동 스킬이면 여기서 먼저 해소해 창을 연다
	if (player->IsCasting())
	{
		const uint64 dueUs = Utils::NowMicroseconds();
		const SkillDef* castDef = SkillTable::Find(player->castSkillId);

		if (castDef != nullptr && castDef->HasMovement() && player->castEndAtUs <= dueUs + COMBAT_FLUSH_MS * 1000)
		{
			player->castEndAtUs = min(player->castEndAtUs, dueUs);
			ResolveCasts(dueUs);
		}
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

			// 벽을 지나는 이동 스킬(유령 신랑의 저주) 창 동안은 건너뛴다 - 속도 창과 같은 "보낸 때" 기준
			const bool passesWalls = (player->moveIgnoresWallsUntilUs != 0 && sentUs < player->moveIgnoresWallsUntilUs);

			if (passesWalls == false
				&& GWallCheck.load(memory_order_relaxed)
				&& Walls().SweepBlocked(x0, y0, pkt.info().x(), pkt.info().y(), 0.f, OUT &hitKind))
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

	// 건곤일척 - 움직이면 망치 스킬 피해 증가가 사라진다 (사용자 결정 2026-10-11 "움직이지 않은 상태에서만")
	if (player->stillSkillId != 0)
	{
		const float sdx = player->posInfo->x() - player->stillAnchorX;
		const float sdy = player->posInfo->y() - player->stillAnchorY;
		if (sdx * sdx + sdy * sdy > 30.f * 30.f)
			BreakStillBuff(player, "move", nowUs);
	}
}

void Room::HandleAttack(uint64 attackerId, uint64 targetId, float aimX, float aimY)
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

	// 대상 없는 평타 (일보일검 · 난무, 사용자 결정 2026-10-11) - 대상이 있든 없든 조준 쪽으로. 보통 평타 경로를 타지 않는다
	if (attacker->IsPlayer() && StartSpecialBasic(attacker, aimX, aimY, nowUs))
		return;

	auto targetIt = _objects.find(targetId);
	if (targetIt == _objects.end())
		return;

	CreatureRef target = dynamic_pointer_cast<Creature>(targetIt->second);
	if (target == nullptr || target->IsAlive() == false)
		return;

	// 몬스터끼리는 적이 아니다 (monster-ai.md Core Rule 18 ② · 2026-10-11 "모든 몬스터는 서로 적대시하지 않아")
	if (attacker->IsMonster() && target->IsMonster())
		return;

	// 진입 무적 - 때릴 수 없다 (AC-10). 3 초 동안만 생기는 줄이라 로그로 남긴다
	if (target->IsPlayer() && static_pointer_cast<Player>(target)->IsSpawnInvulnerable(nowUs))
	{
		LOG_LINE << "[INVULN] blocked attack on char=" << targetId << " by=" << attackerId << '\n';
		return;
	}

	// 대상 락 시점에도 사거리 · 시선을 본다. 히트 시점에 한 번 더 재검증 (유령 신부의 축복은 벽을 무시한다)
	if (IsInAttackRange(attacker, target) == false)
		return;

	if (IgnoresWalls(attacker) == false && IsAttackLineClear(Walls(), attacker, target) == false)
	{
		GStats.AddAttackWallBlocked();
		return;
	}

	// FoW - 은신 · 이 클라가 모르는 대상은 지정할 수 없다 (fog-of-war.md AC-FOW-06, 안티치트)
	if (CanTarget(attacker, target, nowUs) == false)
	{
		LOG_LINE << "[FOW] attack rejected - target not visible. attacker=" << attackerId << " target=" << targetId << '\n';
		return;
	}

	// 공격하면 내 무적이 풀린다 (AC-10). 은신도 (그림자 감추기 · 니힐)
	if (attacker->IsPlayer())
		ClearSpawnInvulnerability(static_pointer_cast<Player>(attacker), "attack");
	BreakStealth(attacker, "attack", nowUs);

	const uint32 windupMs = attacker->GetAttackWindupMs();

	attacker->attackMode = 0;
	attacker->attackTargetId = targetId;
	attacker->attackHitAtUs = nowUs + static_cast<uint64>(windupMs) * 1000;
	_attackers.insert(attackerId);

	Protocol::AttackInfo& info = _pendingAttacks.emplace_back();
	info.set_attacker_id(attackerId);
	info.set_target_id(targetId);
	info.set_windup_ms(windupMs);
	info.set_interval_ms(attacker->GetAttackIntervalMs());		// 우클릭 지속 공격의 박자 (2026-10-11)
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

	// 패시브는 키를 차지할 뿐 - 눌러도 아무 일 없다 (사용자 결정 2026-10-10). 효과는 평타 적중(on_hit) · 치명상(lethal_save)에서 나간다
	if (def->kind == SkillKind::Passive)
		return;

	// 통아 · 육량전 · 은장도 - "그동안에는 기본 공격만 사용할 수 있다" (basic_only 버프)
	if (caster->status.BuffMax(SkillBuffStat::BasicOnly) > 0.f)
	{
		LOG_LINE << "[SKILL REJECT] caster=" << casterId << " skill=" << def->skillId << " reason=basic_only" << '\n';
		return;
	}

	// 갑옷 스킬 #106 ~ #114 - "망치를 들고 있을 때만" (skills.csv requires_weapon). 무기가 다르면 아무 일 없다
	if (MeetsWeaponRequirement(caster, *def) == false)
	{
		LOG_LINE << "[SKILL REJECT] caster=" << casterId << " skill=" << def->skillId << " reason=weapon requires=" << def->requiresWeapon
			<< (def->requiresWeaponTag.empty() ? "" : ":" + def->requiresWeaponTag) << '\n';
		return;
	}

	const uint64 nowUs = Utils::NowMicroseconds();

	// 유지형 - 같은 키를 다시 누르면 끝 (사용자 결정). 유지 중엔 다른 스킬을 쓸 수 없다
	if (caster->IsHolding())
	{
		if (caster->holdSkillId == def->skillId && caster->holdSlot == pkt.slot())
			StopHold(caster, nowUs, "key");
		return;
	}

	// 재입력 - 첫 발동 뒤 창(skill_rules recast_window_ms) 안의 같은 키. 쿨다운 · 캐스트 없이 바로, 첫 조준 대상을 그대로 쓴다
	if (caster->recastSkillId == def->skillId && caster->recastSlot == pkt.slot() && nowUs < caster->recastUntilUs)
	{
		if (caster->CanUseSkill(nowUs) == false || caster->IsCasting())
			return;

		const uint64 targetId = caster->recastTargetId;
		const float aimX = caster->recastAimX;
		const float aimY = caster->recastAimY;

		caster->recastSkillId = 0;
		caster->recastUntilUs = 0;
		caster->recastTargetId = 0;
		caster->castItemLevel = caster->IsPlayer() ? static_pointer_cast<Player>(caster)->ItemLevelOfSlot(pkt.slot()) : 0;

		LOG_LINE << "[SKILL RECAST] caster=" << casterId << " skill=" << def->skillId << " target=" << targetId << '\n';
		FireSkill(caster, *def, pkt.slot(), targetId, aimX, aimY, /*recast*/ true, nowUs);
		return;
	}

	if (nowUs < skillSlot->cooldownEndUs)
		return;

	if (caster->CanUseSkill(nowUs) == false)
		return;

	if (caster->IsCasting())
		return;

	// 외로운 외침 - 반경 안에 아무도 없을 때만 쓸 수 있다 (requires alone)
	for (const SkillEffect& e : def->effects)
	{
		if (e.condition == SkillRequire::Alone && IsAlone(caster, def->radiusCm) == false)
			return;
	}

	// 대상 지정 단일기만 대상을 검사한다 - 조준 없는 단일기(맹수의 포효)는 서버가 전방에서 고른다 (CollectSkillTargets)
	if (def->shape == SkillShape::Single && def->aimType == SkillAimType::Target)
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

		// 벽 너머는 지정할 수 없다 - 유령 신부의 축복만 예외 (2026-10-11). 쿨다운 없이 거부한다
		if (HasLineOfAttack(caster, caster->posInfo->x(), caster->posInfo->y(), target) == false)
		{
			LOG_LINE << "[SKILL REJECT] caster=" << casterId << " skill=" << def->skillId << " reason=wall target=" << pkt.target_id() << '\n';
			return;
		}

		// FoW - 은신 · 이 클라가 모르는 대상은 지정할 수 없다 (범위 · 투사체 스킬엔 맞는다)
		if (CanTarget(caster, target, nowUs) == false)
		{
			LOG_LINE << "[FOW] skill rejected - target not visible. caster=" << casterId << " skill=" << def->skillId << " target=" << pkt.target_id() << '\n';
			return;
		}
	}

	// 스킬을 쓰면 은신이 풀린다 (그림자 감추기 · 니힐) - 은신 스킬 자신은 발동 때 새로 건다
	BreakStealth(caster, "skill", nowUs);

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
	caster->castItemLevel = caster->IsPlayer() ? static_pointer_cast<Player>(caster)->ItemLevelOfSlot(pkt.slot()) : 0;
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

	// 진입 무적 - CC 도 받지 않는다 (AC-10). 니힐도
	if (target->IsPlayer() && static_pointer_cast<Player>(target)->IsSpawnInvulnerable(nowUs))
		return;
	if (target->IsInvulnerable(nowUs))
		return;

	const uint32 effectiveMs = target->ComputeCcDurationMs(baseDurationMs);
	if (effectiveMs == 0)
		return;

	ApplyCcRaw(target, instigatorId, type, effectiveMs, magnitude, nowUs);
}

// 저항 계산 없이 그 길이 그대로 - 넉백 CC(날아가는 시간)는 거리가 이미 저항으로 줄었다 (공식 C6)
void Room::ApplyCcRaw(const CreatureRef& target, uint64 instigatorId, Protocol::CcType type, uint32 effectiveMs, float magnitude, uint64 nowUs)
{
	const uint64 targetId = target->objectInfo->object_id();
	const bool isHard = IsHardCcType(type);

	if (isHard)
		target->ApplyHardCc(type, nowUs, effectiveMs);
	else
		target->ApplySoftCc(type, nowUs, effectiveMs, magnitude);

	// 도발 - 누구에게 끌려가는가 (시전자). 겹치면 나중 것
	if (type == Protocol::CC_TYPE_TAUNT)
	{
		target->tauntSourceId = instigatorId;
		LOG_LINE << "[TAUNT] target=" << targetId << " by=" << instigatorId << " for=" << effectiveMs << "ms" << '\n';
	}

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

	// 행동 불능이면 유지형도 끊긴다 (속박은 아니다 - 빈쿨룸은 자기를 속박하면서 유지한다)
	if (type == Protocol::CC_TYPE_STUN || type == Protocol::CC_TYPE_KNOCKBACK || type == Protocol::CC_TYPE_LAUNCH || type == Protocol::CC_TYPE_TAUNT)
	{
		if (target->IsHolding())
			StopHold(target, nowUs, "cc");
	}
}

void Room::ResolveAttacks(uint64 nowUs)
{
	if (_attackers.empty())
		return;

	vector<uint64> resolved;

	// 스냅샷 - 피해 경로(치명상 방어 · 밀기)가 대상의 평타를 지우며 _attackers 를 고친다
	const vector<uint64> attackers(_attackers.begin(), _attackers.end());

	for (uint64 attackerId : attackers)
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

		// 대상 없는 평타 (일보일검 횡베기 · 난무 사방 베기, 2026-10-11) - 대상 대신 모양으로 판정한다
		if (attacker->attackMode != 0)
		{
			if (CanStillStrike(attacker))
				ResolveSpecialBasic(attacker, nowUs);
			attacker->attackMode = 0;
			continue;
		}

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

		// windup 중 벽 · 엄폐물 뒤로 들어갔다 -> 시선 상실, 같은 취소 (Rule 7 · Edge Cases). 유령 신부의 축복은 예외
		if (IgnoresWalls(attacker) == false && IsAttackLineClear(Walls(), attacker, target) == false)
		{
			GStats.AddAttackWallBlocked();
			_pendingAttackCancels.push_back(attackerId);
			continue;
		}

		// 활 · 스태프 - 평타가 투사체로 날아간다. 피해는 도착 때 (OnProjectileHit, 그때 스탯으로). 날아가는 중 벽에 막힌다 (2 단계)
		if (attacker->rangedAutoAttack)
		{
			Projectile p;
			p.casterId = attackerId;
			p.skillId = 0;
			p.effectId = 0;
			p.x = attacker->posInfo->x();
			p.y = attacker->posInfo->y();
			p.dirX = target->posInfo->x() - p.x;
			p.dirY = target->posInfo->y() - p.y;
			p.speedCms = SkillTable::Rules().basicProjectileSpeedCms;
			p.radiusCm = SkillTable::Rules().projectileRadiusCm;
			p.homingTargetId = targetId;
			p.lastStepUs = nowUs;
			p.ctx.casterId = attackerId;
			LaunchProjectile(move(p));

			attacker->attackReadyAtUs = nowUs + static_cast<uint64>(attacker->GetAttackRecoveryMs()) * 1000;
			continue;
		}

		// 반격(정좌) · 받아넘기기(유수) - 평타가 무효. 후딜은 소모된다 (3 단계)
		if (TryCounter(target, attacker, attackerId, nowUs) || TryParry(target, attacker->posInfo->x(), attacker->posInfo->y(), nowUs))
		{
			attacker->attackReadyAtUs = nowUs + static_cast<uint64>(attacker->GetAttackRecoveryMs()) * 1000;
			_pendingAttackCancels.push_back(attackerId);
			continue;
		}

		bool isCrit = false;
		int32 damage = attacker->ComputeDamageTo(*target, attacker->autoAttackType, OUT isCrit);

		// 점지 - 내 창 표식이 남은 적에게 평타 추가 피해 (skill_rules mark_spear_basic_bonus). 표식은 남는다
		if (target->status.HasMark("spear", attackerId))
			damage = static_cast<int32>(damage * (1.f + SkillTable::Rules().markSpearBasicBonus));

		// 은장도 - 체력이 낮을수록 칼의 피해가 크다 (low_hp_bonus × 잃은 체력 비율)
		if (const float lowHp = attacker->status.BuffMax(SkillBuffStat::LowHpBonus); lowHp > 0.f && attacker->maxHp > 0)
			damage = static_cast<int32>(::lroundf(damage * (1.f + lowHp * (1.f - static_cast<float>(attacker->hp) / attacker->maxHp))));

		// 사망 처리는 Flush 끝 (FlushCombat → KillBatch)
		DamageSource basic;
		basic.basic = true;
		const int32 hpBefore = target->hp;
		const int32 remainingHp = DealDamage(attacker, attackerId, target, damage, attacker->autoAttackType, isCrit, nowUs, basic);

		// 후딜 소모는 히트에 성공했을 때만
		attacker->attackReadyAtUs = nowUs + static_cast<uint64>(attacker->GetAttackRecoveryMs()) * 1000;

		// 고슴도치 - 나를 근접 평타로 때린 적에게 받은 피해의 일부 (여기는 근접 경로 - 투사체 평타는 OnProjectileHit)
		TryThorns(attacker, target, hpBefore - max(0, remainingHp), nowUs);

		// 난무 - 사방 베기: 내 둘레 적 전부가 같은 평타를 맞는다
		if (const float spin = attacker->status.BuffMax(SkillBuffStat::BasicSpin); spin > 0.f)
			ApplyBasicSpin(attacker, target, spin, nowUs);

		// 패시브 - 평타 적중마다 추가 피해 (뇌신 · 화신) + 평타 강화 버프(혼원일체 · 여진). 죽은 대상에겐 들어가지 않는다
		if (remainingHp > 0 && attacker->IsPlayer())
			ApplyOnHitPassives(attacker, target, nowUs);

		// 경독 창 - 창이 닫힐 때 기절시킬 대상을 모은다
		if (remainingHp > 0 && nowUs < attacker->petrifyEndUs && find(attacker->petrifyTargets.begin(), attacker->petrifyTargets.end(), targetId) == attacker->petrifyTargets.end())
			attacker->petrifyTargets.push_back(targetId);
	}

	for (uint64 id : resolved)
		_attackers.erase(id);
}

void Room::ResolveCasts(uint64 nowUs)
{
	if (_casters.empty())
		return;

	vector<uint64> resolved;

	// 스냅샷 - 효과(CC · 밀기)가 대상의 시전을 지우며 _casters 를 고친다
	const vector<uint64> casters(_casters.begin(), _casters.end());

	for (uint64 casterId : casters)
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

		// 효과 적용 시점 - 쿨다운 · S_SKILL_HIT · 판정 · 효과 (예고가 있으면 PendingTelegraph)
		FireSkill(caster, *def, slot, castTargetId, castAimX, castAimY, /*recast*/ false, nowUs);
	}

	for (uint64 id : resolved)
		_casters.erase(id);
}

void Room::FireSkill(const CreatureRef& caster, const SkillDef& def, Protocol::EquipSlot slot, uint64 castTargetId, float aimX, float aimY, bool recast, uint64 nowUs)
{
	const uint64 casterId = caster->objectInfo->object_id();

	// 쿨다운은 효과가 나가는 순간 (R3). 재입력의 두 번째 입력은 첫 발동의 쿨다운을 그대로 둔다. 유지형은 끝날 때 (StopHold)
	if (recast == false && def.kind != SkillKind::Hold)
	{
		if (SkillSlot* skillSlot = caster->GetSlot(slot))
			skillSlot->cooldownEndUs = nowUs + static_cast<uint64>(def.cooldownMs) * 1000;
	}

	const float originX = caster->posInfo->x();
	const float originY = caster->posInfo->y();

	// 재입력은 첫 대상의 지금 위치를 조준한다 (피의 추적 - 대상 쪽으로 돌진)
	if (recast && castTargetId != 0)
	{
		auto targetIt = _objects.find(castTargetId);
		if (targetIt != _objects.end())
		{
			aimX = targetIt->second->posInfo->x();
			aimY = targetIt->second->posInfo->y();
		}
	}

	{
		Protocol::SkillHitInfo& hit = _pendingSkillHits.emplace_back();
		hit.set_caster_id(casterId);
		hit.set_effect_id(def.effectId);
		hit.set_target_id(castTargetId);
		hit.set_impact_x(originX);
		hit.set_impact_y(originY);
		hit.set_aim_x(aimX);
		hit.set_aim_y(aimY);
	}

	// 건곤일척 - 제자리에서 쓴 망치 스킬 한 번의 피해 배율. 발동 때 버프를 소모해 문맥에 싣는다 (예고 · 다단 · 투사체도 같은 값)
	const float stillBonus = TakeStillBonus(caster, def, nowUs);

	// 벽력일창 (fall_on_hit) - 도약은 지금, 내리찍기(대상 효과)는 도약이 끝나는 때. 그사이 맞으면 추락 = 예고 취소 (4 단계)
	const SkillEffect* fall = (recast == false) ? def.FindEffect(SkillEffectType::FallOnHit) : nullptr;
	const SkillEffect* leap = (fall != nullptr) ? def.FindEffect(SkillEffectType::Movement) : nullptr;
	const uint32 leapMs = (leap != nullptr) ? SkillKinematics::TravelMs(leap->distCm, leap->speedCms) : 0;

	if (fall != nullptr && leapMs > 0)
	{
		SkillContext ctx;
		ctx.casterId = casterId;
		ctx.originX = originX;
		ctx.originY = originY;
		ctx.aimX = aimX;
		ctx.aimY = aimY;
		ctx.itemLevel = caster->castItemLevel;
		ctx.slot = slot;
		ctx.targetId = castTargetId;
		ctx.bonusDamageMod = stillBonus;

		vector<CreatureRef> none;
		ApplySkillEffects(caster, def, none, ctx, nowUs);		// 자기 효과(도약)만

		caster->leapUntilUs = nowUs + static_cast<uint64>(leapMs) * 1000;
		caster->leapSkillId = def.skillId;

		PendingTelegraph& pending = _pendingTelegraphs.emplace_back();
		pending.casterId = casterId;
		pending.skillId = def.skillId;
		pending.fireAtUs = caster->leapUntilUs;
		pending.targetId = castTargetId;
		pending.originX = originX;
		pending.originY = originY;
		pending.aimX = aimX;
		pending.aimY = aimY;
		pending.itemLevel = caster->castItemLevel;
		pending.targetsOnly = true;
		pending.bonusDamageMod = stillBonus;

		LOG_LINE << "[LEAP] char=" << casterId << " skill=" << def.skillId << " lands in=" << leapMs << "ms" << '\n';
	}
	else if (def.telegraphMs == 0 || recast)
	{
		SkillContext ctx;
		ctx.casterId = casterId;
		ctx.originX = originX;
		ctx.originY = originY;
		ctx.aimX = aimX;
		ctx.aimY = aimY;
		ctx.itemLevel = caster->castItemLevel;
		ctx.recast = recast;
		ctx.slot = slot;
		ctx.targetId = castTargetId;
		ctx.bonusDamageMod = stillBonus;

		vector<CreatureRef> targets;

		if (def.shape == SkillShape::Projectile && recast == false)
		{
			// 2 단계 - 날아가는 투사체. 대상 효과는 맞는 순간(OnProjectileHit), 자기 효과는 지금 (대상 없이).
			// 연사(volley) 스킬은 한 발을 여기서 쏘지 않는다 - 자기 효과 volley 가 여러 발을 쏜다 (2026-10-11)
			if (def.FindEffect(SkillEffectType::Volley) == nullptr)
				LaunchSkillProjectile(caster, def, ctx, nowUs);
		}
		else if (def.shape == SkillShape::Projectile)
		{
			// 투사체 스킬의 재입력(올가미 당기기) - 투사체가 맞힌 대상에게 바로. 사거리 안이어야 한다
			auto targetIt = _objects.find(castTargetId);
			CreatureRef target = (targetIt != _objects.end()) ? dynamic_pointer_cast<Creature>(targetIt->second) : nullptr;
			if (target != nullptr && target->IsAlive() && IsInSkillRange(caster, target, def.rangeCm))
				targets.push_back(target);
		}
		else
		{
			// 재입력의 판정 모양이 다를 수 있다 (찌르고 베기 1118 - 찌르기 다음 휩쓸어 베기). requires=recast 효과의
			// param "shape:cone:<각>:<반경>" · "shape:circle:<반경>" 이 그 모양이다
			SkillDef recastShape;
			const SkillDef* shapeDef = &def;
			if (recast)
			{
				for (const SkillEffect& e : def.effects)
				{
					if (e.condition != SkillRequire::Recast || e.param.rfind("shape:", 0) != 0)
						continue;

					vector<string> parts;
					size_t from = 0;
					for (size_t at = e.param.find(':'); ; at = e.param.find(':', from))
					{
						parts.push_back(e.param.substr(from, at == string::npos ? string::npos : at - from));
						if (at == string::npos)
							break;
						from = at + 1;
					}

					recastShape = def;
					if (parts.size() >= 4 && parts[1] == "cone")
					{
						recastShape.shape = SkillShape::Cone;
						recastShape.angleDeg = static_cast<float>(::atof(parts[2].c_str()));
						recastShape.radiusCm = static_cast<float>(::atof(parts[3].c_str()));
						shapeDef = &recastShape;
					}
					else if (parts.size() >= 3 && parts[1] == "circle")
					{
						recastShape.shape = SkillShape::CircleSelf;
						recastShape.radiusCm = static_cast<float>(::atof(parts[2].c_str()));
						shapeDef = &recastShape;
					}
					break;
				}
			}

			CollectSkillTargets(caster, *shapeDef, originX, originY, castTargetId, aimX, aimY, OUT targets);
		}

		ApplySkillEffects(caster, def, targets, ctx, nowUs);

		LOG_LINE << "[SKILL FIRE] caster=" << casterId
			<< " skill=" << def.skillId
			<< " targets=" << targets.size()
			<< (recast ? " recast" : "")
			<< (def.shape == SkillShape::Projectile && recast == false ? " projectile" : "")
			<< " cd=" << def.cooldownMs << "ms" << '\n';
	}
	else
	{
		PendingTelegraph& pending = _pendingTelegraphs.emplace_back();
		pending.casterId = casterId;
		pending.skillId = def.skillId;
		pending.fireAtUs = nowUs + static_cast<uint64>(def.telegraphMs) * 1000;
		pending.targetId = castTargetId;
		pending.originX = originX;
		pending.originY = originY;
		pending.aimX = aimX;
		pending.aimY = aimY;
		pending.itemLevel = caster->castItemLevel;
		pending.bonusDamageMod = stillBonus;

		LOG_LINE << "[SKILL TELEGRAPH] caster=" << casterId
			<< " skill=" << def.skillId
			<< " in=" << def.telegraphMs << "ms" << '\n';
	}

	// 재입력 창 - 첫 발동 뒤 recast_window_ms 안의 같은 키 (사용자 결정 2026-10-10: 2 초)
	if (recast == false && def.kind == SkillKind::Recast)
	{
		caster->recastSkillId = def.skillId;
		caster->recastSlot = slot;
		caster->recastUntilUs = nowUs + static_cast<uint64>(SkillTable::Rules().recastWindowMs) * 1000;
		caster->recastTargetId = castTargetId;
		caster->recastAimX = aimX;
		caster->recastAimY = aimY;
	}
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
		const uint32 itemLevel = pending.itemLevel;
		const bool targetsOnly = pending.targetsOnly;
		const float bonusDamageMod = pending.bonusDamageMod;

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

		SkillContext ctx;
		ctx.casterId = casterId;
		ctx.originX = originX;
		ctx.originY = originY;
		ctx.aimX = aimX;
		ctx.aimY = aimY;
		ctx.itemLevel = itemLevel;
		ctx.slot = caster->castSlot;
		ctx.targetsOnly = targetsOnly;
		ctx.targetId = targetId;
		ctx.bonusDamageMod = bonusDamageMod;
		if (targetsOnly)
		{
			caster->leapUntilUs = 0;		// 내리찍었다 - 더는 도약 중이 아니다
			caster->leapSkillId = 0;
		}
		ApplySkillEffects(caster, *def, targets, ctx, nowUs);

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
		expire(target->blindEndUs, Protocol::CC_TYPE_BLIND);
		expire(target->tauntEndUs, Protocol::CC_TYPE_TAUNT);
		if (target->tauntEndUs == 0)
			target->tauntSourceId = 0;

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
			|| target->blindEndUs || target->tauntEndUs
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

		AppendCcState(target, nowUs, pkt);
	}

	if (pkt.states_size() == 0)
		return;

	// FoW - 아는 생물의 CC 만 (보이게 되는 순간의 CC 는 SendSnapshots 가 따로 준다)
	const int32 total = pkt.states_size();
	auto keep = [&](const PlayerRef& v, const Protocol::CcStateInfo& s) { return CanSee(v, s.target_id()); };

	SendVisible(total,
		[&](const PlayerRef& v) { int32 n = 0; for (const Protocol::CcStateInfo& s : pkt.states()) n += keep(v, s) ? 1 : 0; return n; },
		[&]() { return ServerPacketHandler::MakeSendBuffer(pkt); },
		[&](const PlayerRef& v)
		{
			Protocol::S_CC_STATE mine;
			for (const Protocol::CcStateInfo& s : pkt.states())
				if (keep(v, s))
					mine.add_states()->CopyFrom(s);
			return ServerPacketHandler::MakeSendBuffer(mine);
		});
}

void Room::HandleEquip(GameSessionRef session, string requestId, uint64 instanceId, Protocol::EquipSlot slot)
{
	JOB_TAG();
	PlayerRef player = session->player.load();
	if (player == nullptr)
		return;

	// 죽은 몸은 장비를 못 바꾼다 - 루팅 · 버리기와 같은 거절 (예전엔 검사가 없어 사망 저장 중에 메모리에서 바꿔 낄 수 있었다, 2026-10-08)
	if (_objects.find(player->objectInfo->object_id()) == _objects.end() || player->IsAlive() == false)
	{
		LOG_LINE << "[ITEM] equip rejected - not alive in room. char=" << player->objectInfo->object_id() << " item=" << instanceId << '\n';
		session->SendItemResult(requestId, Protocol::ITEM_INVALID_STATE, {});
		return;
	}

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

	// 죽은 몸은 장비를 못 바꾼다 (HandleEquip 과 같다)
	if (_objects.find(player->objectInfo->object_id()) == _objects.end() || player->IsAlive() == false)
	{
		LOG_LINE << "[ITEM] unequip rejected - not alive in room. char=" << player->objectInfo->object_id() << " item=" << instanceId << '\n';
		session->SendItemResult(requestId, Protocol::ITEM_INVALID_STATE);
		return;
	}

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

	// 테스트 모드가 만든 아이템(isTest)은 가방에도 DB 에도 가지 않는다 - 그냥 사라진다
	for (const ItemEntry& e : player->inventory)
		if (e.isTest == false)
			snap.items.push_back(e);

	for (const ItemEntry& e : player->equipped)
		if (e.instanceId != 0 && e.isTest == false)
			snap.items.push_back(e);

	snap.droppedIds = player->droppedIds;

	if (GDeathCommits.TryBegin(characterId) == false)
		return;

	snap.bagId = GNextBagId.fetch_add(1);
	snap.expiresAtUs = Utils::NowMicroseconds() + BAG_LIFETIME_US;

	const int64 newItems = count_if(snap.items.begin(), snap.items.end(), [](const ItemEntry& e) { return e.isNew; });

	LOG_LINE << "[DEATH] commit begin - char=" << characterId << " bag=" << snap.bagId << " items=" << snap.items.size()
		<< " new=" << newItems << " dropped=" << snap.droppedIds.size() << '\n';

	// 죽는 순간 소지 · 착용을 비운다 (사용자 결정 2026-10-08) - DB 저장은 위 스냅샷으로 한다.
	// 예전엔 저장이 끝난 뒤(OnBagCreated) 비웠다. DB 장애로 저장이 길어지면 그동안 죽은 사람 화면에 아이템이 남고, 메모리에서 바꿔 낄 수도 있었다.
	// 가방이 월드에 나타나는 것은 그대로 저장 뒤다 - 먼저 띄우면 누가 주운 뒤 저장이 실패할 때 복제가 된다 (DB 우선, ADR-0005).
	// 저장을 포기해도(데이터가 깨진 경우) DB 는 그대로라 다음 접속이 DB 에서 다시 적재한다 - SavePlayer 는 체력 · 위치만 쓴다
	player->inventory.clear();
	player->droppedIds.clear();
	for (ItemEntry& e : player->equipped)
		e = ItemEntry{};

	OnEquipmentChanged(player, "death");
	SendEquipSync(player);

	if (GameSessionRef session = player->session.lock())		// 끊긴 몸이면 보낼 곳이 없다
	{
		Protocol::S_INVENTORY_SYNC invPkt;		// 빈 목록 - 전부 곧 가방으로 간다
		SEND_PACKET_DECLARATION(invPkt);
		session->Send(sendBuffer);
	}

	// 끝날 때까지 다시 한다 (ADR-0010 R-1, 2026-10-08) - 그동안 이 캐릭터는 재입장 · 베이스캠프 장비 조작 · 퇴장이 막혀 있다
	// (GDeathCommits). 예전엔 3 번 실패하면 포기해서 죽은 사람이 장비를 그대로 지켰다
	RoomRef room = GetRoomRef();
	auto shared = make_shared<DeathSnapshot>(move(snap));

	// 첫 시도가 실패하면 이 스냅샷이 파일로 남는다 - 그 사이 서버가 꺼져도 다음 부팅이 끝낸다 (CommitJournal.h)
	JournalEntry journal;
	journal.kind = JournalKind::Death;
	journal.characterId = characterId;
	journal.death = *shared;

	QueueCommit(move(journal),
		[shared](DBConnection* conn, CommitMode mode, const string& journalId) { return TryCommitDeath(conn, *shared, mode, journalId); },
		[room, shared](DBConnection*, bool committed, CommitMode) { room->DoAsync(&Room::OnBagCreated, *shared, committed); });
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
		LOG_LINE << "[DEATH] commit abandoned - no bag, items stay with the owner. char=" << snap.characterId << '\n';
		return;
	}

	// 10 분은 가방이 실제로 생긴 지금부터 - DB 장애로 저장이 오래 밀렸으면 처음 잡아 둔 만료 시각은 이미 지났을 수 있다.
	// DB 쪽도 커밋 때 NOW() + 10 분이라 둘이 맞는다
	snap.expiresAtUs = Utils::NowMicroseconds() + BAG_LIFETIME_US;

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

	// 죽은 사람의 메모리 인벤토리는 이미 비었다 - 죽는 순간 BeginDeathCommit 이 비운다 (2026-10-08)
}

void Room::BeginEscapeCommit(uint64 playerId)
{
	BeginEscapeCommitInto(playerId, nullptr);
}

void Room::BeginEscapeCommitInto(uint64 playerId, vector<uint64>* despawnBatch)
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

	// 테스트 모드가 만든 아이템(isTest)은 커밋에서 뺀다 - DB 에 남지 않는다
	EscapeSnapshot snap;
	snap.characterId = playerId;
	for (const ItemEntry& e : player->inventory)
		if (e.isTest == false)
			snap.carried.push_back(e);
	for (int32 i = 0; i < SKILL_SLOT_COUNT; i++)
		snap.equipped[i] = player->equipped[i].isTest ? ItemEntry{} : player->equipped[i];
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

	if (despawnBatch != nullptr)
	{
		despawnBatch->push_back(playerId);
	}
	else
	{
		Protocol::S_DESPAWN despawnPkt;
		despawnPkt.add_object_ids(playerId);
		SEND_PACKET_DECLARATION(despawnPkt);
		Broadcast(sendBuffer);
	}

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

	// 끝날 때까지 다시 한다 - 엄격 → 관대 → 포기 (ObjectUtils.h QueueCommit). 그동안 Escaping 이라 베이스캠프 조작 · 재입장이 막힌다
	JournalEntry journal;
	journal.kind = JournalKind::Escape;
	journal.characterId = playerId;
	journal.escape = snap;

	QueueCommit(move(journal),
		[snap](DBConnection* conn, CommitMode mode, const string& journalId) { return TryCommitEscape(conn, snap, mode, journalId); },
		[session, snap, queuedUs](DBConnection* conn, bool committed, CommitMode)
		{
			session->place.store(SessionPlace::BaseCamp);
			GDeathCommits.End(snap.characterId);

			// 포기(데이터가 깨진 경우뿐)면 DB 는 런 전 그대로다 - "탈출 성공" 배너 없이(RUN_END_NONE) 베이스캠프로, 아래 동기화가 실제 상태를 보여 준다
			Protocol::S_LEAVE_GAME leavePkt;
			leavePkt.set_reason(committed ? Protocol::RUN_END_ESCAPED : Protocol::RUN_END_NONE);
			leavePkt.set_gold_kept(committed ? snap.gold : 0);
			SEND_PACKET_DECLARATION(leavePkt);
			session->Send(sendBuffer);

			LOG_LINE << "[ESCAPE] " << (committed ? "committed" : "abandoned") << " - char=" << snap.characterId << " gold=" << (committed ? snap.gold : 0)
				<< " queued_to_done=" << (Utils::NowMicroseconds() - queuedUs) / 1000 << "ms\n";

			// 베이스캠프로 돌아왔다 - 방금 커밋한 결과를 S3 에 보낸다 (UI 4단계)
			SendBaseCampInventory(conn, session, snap.characterId);

			// 기다리는 사이 끊고 다시 로그인했다 - 그 세션은 로그인 때 저장 전 상태를 받았을 수 있다. 결과만 다시 보낸다
			// (S_LEAVE_GAME 은 보내지 않는다 - 이미 베이스캠프에 있고, 그사이 입장을 누르면 화면이 엇갈린다. 2026-10-08 리뷰 ③)
			ResendBaseCampIfRelogged(conn, session, snap.characterId);
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
	// FoW - 새 접속은 아무것도 모른다. 아는 집합을 비우면 다음 시야 갱신이 보이는 생물을 전부 다시 보낸다 (S_VISION 도)
	body->ResetVision();
	_pendingSpawns.push_back(playerId);
	SendDynamicWalls(body, Utils::NowMicroseconds());

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
	UpdateDecoys(nowUs);

	// 0.1초 경과했으면, UpdateTick()
	DoTimer(100, &Room::UpdateTick);
}

void Room::FlushMoves()
{
	JOB_TAG();
	_retiredWalls.clear();		// 잡 맨 앞 - 아무도 옛 벽 지도를 쥐고 있지 않다

	// 몬스터 이동을 같은 틱에 적분해 같은 S_MOVE 에 싣는다 - 패킷 · Send 수가 늘지 않는다 (monster-ai.md Core Rule 25)
	StepMonsters(Utils::NowMicroseconds());

	// 도발당한 쪽은 서버가 시전자 쪽으로 옮긴다 - 같은 S_MOVE 에 실린다
	StepTaunts(Utils::NowMicroseconds());

	// 그림자 분신이 걷는다 - 같은 S_MOVE 에 실린다 (2026-10-11)
	StepClones(Utils::NowMicroseconds());

	// FoW - 이번 위치로 시야를 갱신한다. 보이게 된 생물의 S_SPAWN 이 아래 S_MOVE 보다 먼저 나가야 한다
	UpdateVisibility(Utils::NowMicroseconds());

	if (_dirtyMovers.empty() == false)
	{
		vector<const Protocol::PosInfo*> infos;
		infos.reserve(_dirtyMovers.size());

		for (uint64 objectId : _dirtyMovers)
		{
			auto findIt = _objects.find(objectId);
			if (findIt == _objects.end())
				continue;

			infos.push_back(findIt->second->posInfo);
		}

		_dirtyMovers.clear();

		// 받는 사람마다 아는 생물의 이동만 (FoW). 내 것도 실린다 - 클라는 보정이 아닌 내 이동을 무시한다
		SendListVisible<Protocol::S_MOVE>(infos,
			[](Protocol::S_MOVE& pkt, const Protocol::PosInfo* info) { pkt.add_infos()->CopyFrom(*info); },
			[this](const PlayerRef& viewer, const Protocol::PosInfo* info) { return CanSee(viewer, info->object_id()); });
	}

	DoTimer(MOVE_FLUSH_MS, &Room::FlushMoves);
}

void Room::FlushCombat()
{
	JOB_TAG();
	const uint64 nowUs = Utils::NowMicroseconds();
	_retiredWalls.clear();

	// 이 Flush 에서 해소되는 히트는 동시다 - 사망은 모았다가 아래에서 한꺼번에 (TR-combat-022 "동시 치사는 양쪽 모두 사망")
	_kills.Begin();

	UpdateCc(nowUs);
	UpdatePendingSelfCcs(nowUs);	// 오버로드 · 니힐 - 효과가 끝날 때의 자기 기절 (2026-10-11)
	UpdateStatuses(nowUs);
	UpdateTrails(nowUs);
	UpdatePathPainters(nowUs);		// 열화 - 지나간 길에 장판 (2026-10-11)
	UpdateZones(nowUs);
	UpdateHolds(nowUs);
	UpdateWindows(nowUs);
	ResolveAttacks(nowUs);
	ResolveCasts(nowUs);
	ResolveTelegraphs(nowUs);
	UpdateVolleys(nowUs);			// 연사 - 이번 Flush 에 나갈 발을 쏜다 (2026-10-11)
	UpdateDynamicWalls(nowUs);		// 지각변동 벽이 무너진다
	StepProjectiles(nowUs);
	UpdateSlams(nowUs);				// 저 멀리 - 날아간 적이 벽에 부딪힌다 (2026-10-11)
	ResolvePendingHits(nowUs);
	ResolveBursts(nowUs);

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

	// ── 방송 - 받는 사람마다 아는 생물의 사건만 (FoW, fog-of-war.md § Detailed Rules 13). 전부 보이는 사람은 버퍼 하나를 함께 쓴다 ──
	auto selfId = [](const PlayerRef& v) { return v->objectInfo->object_id(); };

	if (_pendingAttacks.empty() == false)
	{
		SendListVisible<Protocol::S_ATTACK>(_pendingAttacks,
			[](Protocol::S_ATTACK& pkt, const Protocol::AttackInfo& info) { pkt.add_attacks()->CopyFrom(info); },
			[&](const PlayerRef& v, const Protocol::AttackInfo& info) { return CanSee(v, info.attacker_id()) || info.target_id() == selfId(v); });
		_pendingAttacks.clear();
	}

	if (_pendingAttackCancels.empty() == false)
	{
		SendListVisible<Protocol::S_ATTACK_CANCEL>(_pendingAttackCancels,
			[](Protocol::S_ATTACK_CANCEL& pkt, uint64 id) { pkt.add_attacker_ids(id); },
			[&](const PlayerRef& v, uint64 id) { return CanSee(v, id); });
		_pendingAttackCancels.clear();
	}

	if (_pendingCasts.empty() == false)
	{
		SendListVisible<Protocol::S_SKILL_CAST>(_pendingCasts,
			[](Protocol::S_SKILL_CAST& pkt, const Protocol::SkillCastInfo& info) { pkt.add_casts()->CopyFrom(info); },
			[&](const PlayerRef& v, const Protocol::SkillCastInfo& info) { return CanSee(v, info.caster_id()); });
		_pendingCasts.clear();
	}

	if (_pendingCastCancels.empty() == false)
	{
		SendListVisible<Protocol::S_SKILL_CANCEL>(_pendingCastCancels,
			[](Protocol::S_SKILL_CANCEL& pkt, uint64 id) { pkt.add_caster_ids(id); },
			[&](const PlayerRef& v, uint64 id) { return CanSee(v, id); });
		_pendingCastCancels.clear();
	}

	if (_pendingSkillHits.empty() == false)
	{
		// 시전자를 모르더라도 맞은 대상을 알거나 연출 지점이 내 시야 안이면 받는다 - 안 보이는 적의 범위기에 맞았을 때 무엇이 왔는지는 보인다
		SendListVisible<Protocol::S_SKILL_HIT>(_pendingSkillHits,
			[](Protocol::S_SKILL_HIT& pkt, const Protocol::SkillHitInfo& info) { pkt.add_hits()->CopyFrom(info); },
			[&](const PlayerRef& v, const Protocol::SkillHitInfo& info)
			{
				return CanSee(v, info.caster_id()) || CanSee(v, info.target_id()) || NearViewer(v, info.impact_x(), info.impact_y(), 300.f);
			});
		_pendingSkillHits.clear();
	}

	if (_pendingCcApplied.empty() == false || _pendingCcExpired.empty() == false || _pendingKnockbacks.empty() == false)
	{
		auto build = [&](const PlayerRef* v, Protocol::S_CC* out)
			{
				int32 n = 0;
				for (const Protocol::CcEventInfo& info : _pendingCcApplied)
					if (v == nullptr || CanSee(*v, info.target_id())) { n++; if (out) out->add_applied()->CopyFrom(info); }
				for (const Protocol::CcEventInfo& info : _pendingCcExpired)
					if (v == nullptr || CanSee(*v, info.target_id())) { n++; if (out) out->add_expired()->CopyFrom(info); }
				for (const Protocol::KnockbackInfo& info : _pendingKnockbacks)
					if (v == nullptr || CanSee(*v, info.target_id())) { n++; if (out) out->add_knockbacks()->CopyFrom(info); }
				return n;
			};

		SendVisible(static_cast<int32>(_pendingCcApplied.size() + _pendingCcExpired.size() + _pendingKnockbacks.size()),
			[&](const PlayerRef& v) { return build(&v, nullptr); },
			[&]() { Protocol::S_CC pkt; build(nullptr, &pkt); return ServerPacketHandler::MakeSendBuffer(pkt); },
			[&](const PlayerRef& v) { Protocol::S_CC pkt; build(&v, &pkt); return ServerPacketHandler::MakeSendBuffer(pkt); });

		_pendingCcApplied.clear();
		_pendingCcExpired.clear();
		_pendingKnockbacks.clear();
	}

	if (_pendingStatusApplied.empty() == false || _pendingStatusExpired.empty() == false)
	{
		auto build = [&](const PlayerRef* v, Protocol::S_STATUS* out)
			{
				int32 n = 0;
				for (const Protocol::StatusInfo& info : _pendingStatusApplied)
					if (v == nullptr || CanSee(*v, info.target_id())) { n++; if (out) out->add_applied()->CopyFrom(info); }
				for (const Protocol::StatusInfo& info : _pendingStatusExpired)
					if (v == nullptr || CanSee(*v, info.target_id())) { n++; if (out) out->add_expired()->CopyFrom(info); }
				return n;
			};

		SendVisible(static_cast<int32>(_pendingStatusApplied.size() + _pendingStatusExpired.size()),
			[&](const PlayerRef& v) { return build(&v, nullptr); },
			[&]() { Protocol::S_STATUS pkt; build(nullptr, &pkt); return ServerPacketHandler::MakeSendBuffer(pkt); },
			[&](const PlayerRef& v) { Protocol::S_STATUS pkt; build(&v, &pkt); return ServerPacketHandler::MakeSendBuffer(pkt); });

		_pendingStatusApplied.clear();
		_pendingStatusExpired.clear();
	}

	if (_pendingProjectileSpawns.empty() == false || _pendingProjectileEnds.empty() == false)
	{
		// 발사 - 시전자를 알거나, 나를 쫓거나, 날아갈 길이 내 시야 안을 지나면. 끝 - 전원 (클라는 모르는 id 를 버린다)
		auto keepSpawn = [&](const PlayerRef& v, const Protocol::ProjectileInfo& info)
			{
				if (CanSee(v, info.caster_id()) || CanSee(v, info.target_id()))
					return true;
				const float r = (v->visionRadiusCm > 0.f ? v->visionRadiusCm : Vision::Rules().radiusCm) + info.radius_cm();
				return Vision::SegmentNearCircle(info.x(), info.y(), info.x() + info.dir_x() * info.range_cm(), info.y() + info.dir_y() * info.range_cm(),
					v->posInfo->x(), v->posInfo->y(), r);
			};

		auto build = [&](const PlayerRef* v, Protocol::S_PROJECTILE* out)
			{
				int32 n = 0;
				for (const Protocol::ProjectileInfo& info : _pendingProjectileSpawns)
					if (v == nullptr || keepSpawn(*v, info)) { n++; if (out) out->add_spawned()->CopyFrom(info); }
				for (const Protocol::ProjectileEndInfo& info : _pendingProjectileEnds)
				{
					n++;
					if (out) out->add_ended()->CopyFrom(info);
				}
				return n;
			};

		SendVisible(static_cast<int32>(_pendingProjectileSpawns.size() + _pendingProjectileEnds.size()),
			[&](const PlayerRef& v) { return build(&v, nullptr); },
			[&]() { Protocol::S_PROJECTILE pkt; build(nullptr, &pkt); return ServerPacketHandler::MakeSendBuffer(pkt); },
			[&](const PlayerRef& v) { Protocol::S_PROJECTILE pkt; build(&v, &pkt); return ServerPacketHandler::MakeSendBuffer(pkt); });

		_pendingProjectileSpawns.clear();
		_pendingProjectileEnds.clear();
	}

	if (_pendingZoneSpawns.empty() == false || _pendingZoneEnds.empty() == false)
	{
		// 장판 - 시전자를 알거나 장판이 내 시야와 겹치면. 끝 - 전원
		auto build = [&](const PlayerRef* v, Protocol::S_ZONE* out)
			{
				int32 n = 0;
				// 지각변동 벽은 지형 - 서버 WallMap 이 모두를 막으니 모두가 알아야 한다 (모르면 보이지 않는 벽에 걸려 되돌려진다)
				for (const Protocol::ZoneInfo& info : _pendingZoneSpawns)
					if (v == nullptr || info.param() == "wall" || CanSee(*v, info.caster_id()) || NearViewer(*v, info.x(), info.y(), info.radius_cm())) { n++; if (out) out->add_spawned()->CopyFrom(info); }
				for (uint64 id : _pendingZoneEnds)
				{
					n++;
					if (out) out->add_ended(id);
				}
				return n;
			};

		SendVisible(static_cast<int32>(_pendingZoneSpawns.size() + _pendingZoneEnds.size()),
			[&](const PlayerRef& v) { return build(&v, nullptr); },
			[&]() { Protocol::S_ZONE pkt; build(nullptr, &pkt); return ServerPacketHandler::MakeSendBuffer(pkt); },
			[&](const PlayerRef& v) { Protocol::S_ZONE pkt; build(&v, &pkt); return ServerPacketHandler::MakeSendBuffer(pkt); });

		_pendingZoneSpawns.clear();
		_pendingZoneEnds.clear();
	}

	if (_pendingDamages.empty() == false)
	{
		// 맞은 쪽을 알 때만 - 안 보이는 적을 범위기로 맞혀도 숫자가 뜨지 않는다 (위치가 새지 않게)
		SendListVisible<Protocol::S_DAMAGE>(_pendingDamages,
			[](Protocol::S_DAMAGE& pkt, const Protocol::DamageInfo& info) { pkt.add_damages()->CopyFrom(info); },
			[&](const PlayerRef& v, const Protocol::DamageInfo& info) { return CanSee(v, info.target_id()); });
		_pendingDamages.clear();
	}

	if (_pendingDeaths.empty() == false)
	{
		// 죽은 쪽을 알거나 내가 죽였을 때
		SendListVisible<Protocol::S_DIED>(_pendingDeaths,
			[](Protocol::S_DIED& pkt, const Protocol::DiedInfo& info) { pkt.add_deaths()->CopyFrom(info); },
			[&](const PlayerRef& v, const Protocol::DiedInfo& info) { return CanSee(v, info.victim_id()) || info.instigator_id() == selfId(v); });
		_pendingDeaths.clear();
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

		// FoW 가 켜져 있으면 생물(플레이어 · 몬스터 · 미끼)은 여기서 보내지 않는다 - 시야 갱신(UpdateVisibility)이 보이는 사람에게만 보낸다.
		// 가방 · 상자는 숨기지 않으므로 그대로 (fog-of-war.md § Detailed Rules 9)
		const bool fow = Vision::Enabled();

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
				if (fow == false)
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
					if (fow == false && (item.second->IsPlayer() || item.second->IsMonster()))
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
	ForgetObject(objectId);
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
	const uint64 nowUs = Utils::NowMicroseconds();

	// 맞을 수 있는 상대인가 - 살아 있고, 진입 무적이 아니고, 몬스터끼리는 아니다 (monster-ai.md Core Rule 18 ②)
	auto canBeHit = [&](const CreatureRef& target)
		{
			if (target == nullptr || target->IsAlive() == false)
				return false;
			if (caster->IsMonster() && target->IsMonster())
				return false;
			if (target->IsPlayer() && static_pointer_cast<Player>(target)->IsSpawnInvulnerable(nowUs))
				return false;
			return true;
		};

	// 조준 방향 - 시전자에서 조준점으로. 조준점이 발밑이면 바라보는 방향
	float dirX = aimX - originX;
	float dirY = aimY - originY;
	const float dirLen = ::sqrtf(dirX * dirX + dirY * dirY);
	if (dirLen > 1.f)
	{
		dirX /= dirLen;
		dirY /= dirLen;
	}
	else
	{
		const float yawRad = caster->posInfo->yaw() * 3.14159265f / 180.f;
		dirX = ::cosf(yawRad);
		dirY = ::sinf(yawRad);
	}

	switch (def.shape)
	{
	case SkillShape::None:
		return;

	case SkillShape::Single:
	{
		// 조준 없는 단일기(맹수의 포효) - 대치 중인 적 = 전방 반원 안에서 가장 가까운 적 (skill_rules confront_range_cm)
		if (def.aimType == SkillAimType::None)
		{
			const float range = SkillTable::Rules().confrontRangeCm;
			CreatureRef best = nullptr;
			float bestSq = range * range;

			for (auto& item : _objects)
			{
				if (item.first == casterId)
					continue;

				CreatureRef target = dynamic_pointer_cast<Creature>(item.second);
				if (canBeHit(target) == false)
					continue;

				const float dx = target->posInfo->x() - originX;
				const float dy = target->posInfo->y() - originY;
				const float distSq = dx * dx + dy * dy;
				if (distSq > bestSq || (dx * dirX + dy * dirY) < 0.f)
					continue;
				if (HasLineOfAttack(caster, originX, originY, target) == false)
					continue;

				best = target;
				bestSq = distSq;
			}

			if (best != nullptr)
				outTargets.push_back(best);
			return;
		}

		auto findIt = _objects.find(castTargetId);
		if (findIt == _objects.end())
			return;

		CreatureRef target = dynamic_pointer_cast<Creature>(findIt->second);
		if (canBeHit(target) == false)
			return;

		const float dx = target->posInfo->x() - originX;
		const float dy = target->posInfo->y() - originY;
		if ((dx * dx + dy * dy) > (def.rangeCm * def.rangeCm))
			return;

		// 벽 너머는 맞지 않는다 - 시전 중 대상이 벽 뒤로 갔다 (유령 신부의 축복 예외, 2026-10-11)
		if (HasLineOfAttack(caster, originX, originY, target) == false)
			return;

		outTargets.push_back(target);
		return;
	}

	case SkillShape::CircleSelf:
	case SkillShape::CirclePoint:
	{
		if (def.radiusCm <= 0.f)
			return;

		// 지점 원은 조준점에 - 사거리 밖을 찍었으면 사거리 끝 (클라 조준 표시와 같은 규칙)
		float cx = originX;
		float cy = originY;
		if (def.shape == SkillShape::CirclePoint)
		{
			const float reach = min(dirLen, def.rangeCm);
			cx = originX + dirX * reach;
			cy = originY + dirY * reach;
		}

		for (auto& item : _objects)
		{
			if (item.first == casterId)
				continue;

			CreatureRef target = dynamic_pointer_cast<Creature>(item.second);
			if (canBeHit(target) == false)
				continue;

			const float dx = target->posInfo->x() - cx;
			const float dy = target->posInfo->y() - cy;
			if ((dx * dx + dy * dy) > (def.radiusCm * def.radiusCm))
				continue;

			// 원의 중심(내 자리 · 찍은 지점)에서 벽에 가린 대상은 맞지 않는다 (2026-10-11 "벽 너머 적에게 공격이 가능해")
			if (HasLineOfAttack(caster, cx, cy, target) == false)
				continue;

			outTargets.push_back(target);
		}
		return;
	}

	case SkillShape::Cone:
	{
		// 반경 안이고 조준 방향과의 각이 angle/2 이내. 각도 · 반경은 skills.csv (일률값, 나중에 조정)
		const float halfCos = ::cosf(def.angleDeg * 0.5f * 3.14159265f / 180.f);

		for (auto& item : _objects)
		{
			if (item.first == casterId)
				continue;

			CreatureRef target = dynamic_pointer_cast<Creature>(item.second);
			if (canBeHit(target) == false)
				continue;

			const float dx = target->posInfo->x() - originX;
			const float dy = target->posInfo->y() - originY;
			const float distSq = dx * dx + dy * dy;
			if (distSq > def.radiusCm * def.radiusCm)
				continue;

			// 발밑에 겹친 상대는 방향을 셀 수 없다 - 맞는다
			const float dist = ::sqrtf(distSq);
			if (dist > 1.f && (dx * dirX + dy * dirY) / dist < halfCos)
				continue;

			if (HasLineOfAttack(caster, originX, originY, target) == false)
				continue;

			outTargets.push_back(target);
		}
		return;
	}

	case SkillShape::Projectile:
		// 2 단계 - 즉시 판정하지 않는다. FireSkill 이 투사체를 날리고 StepProjectiles 가 맞는 순간 판정한다
		return;

	case SkillShape::Line:
	{
		// 시전자에서 range 만큼 뻗는 선분 - 선분까지의 거리가 반폭 + 캡슐 반경 이내면 맞는다
		constexpr float CAPSULE_RADIUS = 42.f;
		const float halfWidth = def.widthCm * 0.5f + CAPSULE_RADIUS;
		const WallMap& walls = Walls();

		for (auto& item : _objects)
		{
			if (item.first == casterId)
				continue;

			CreatureRef target = dynamic_pointer_cast<Creature>(item.second);
			if (canBeHit(target) == false)
				continue;

			const float dx = target->posInfo->x() - originX;
			const float dy = target->posInfo->y() - originY;
			const float t = dx * dirX + dy * dirY;		// 선분 위 투영 거리
			if (t < 0.f || t > def.rangeCm)
				continue;

			const float perp = ::fabsf(dx * dirY - dy * dirX);
			if (perp > halfWidth)
				continue;

			// 벽에 막힌다 - 선형 스킬 (ADR-0006 · skill-system.md line). 유령 신부의 축복은 예외
			if (IgnoresWalls(caster) == false && walls.SweepBlocked(originX, originY, target->posInfo->x(), target->posInfo->y(), 0.f))
				continue;

			outTargets.push_back(target);
		}
		return;
	}
	}
}

void Room::ApplySkillEffects(const CreatureRef& caster, const SkillDef& def, const vector<CreatureRef>& targets, const SkillContext& ctx, uint64 nowUs)
{
	// 🔴 1 단계 - 피해(다단 간격) · CC · 넉백 · 끌기 · 이동(블링크 포함) · 버프 · 지속 피해 · 표식 · 처형 · 조건(requires).
	//    Zone · Chain · Delayed · Throw · Counter · Parry · Hold · Stance · Decoy · Invuln · Reveal · Trail · SelfCc · PetrifyWindow · FallOnHit 는 2 ~ 4 단계
	const uint64 casterId = ctx.casterId;

	// 적중 = 판정에 걸린 대상이 하나라도 있다 (hit 조건). 처치 = 이 발동이 대상을 죽였다 (no_kill 조건)
	const bool anyHit = targets.empty() == false;
	bool anyKill = false;

	// 1) 자기 효과 - 결과와 무관한 것 먼저 (이동 · 버프). 적중 · 미처치 조건은 대상 효과 뒤에.
	//    투사체 적중(targetsOnly)은 발사 때 이미 나갔으니 건너뛴다.
	//    조건은 먼저 전부 본다 - 앞 효과(속성 해제)가 뒤 효과의 조건(no_element)을 바꾸지 않게 (오버로드 · 니힐, 2026-10-11)
	vector<const SkillEffect*> selfEffects;
	for (const SkillEffect& e : def.effects)
	{
		if (ctx.targetsOnly || e.self == false || e.condition == SkillRequire::Hit || e.condition == SkillRequire::NoKill)
			continue;
		if (MeetsCondition(e, ctx, nullptr, anyHit, anyKill, nowUs) == false)
			continue;

		selfEffects.push_back(&e);
	}
	for (const SkillEffect* e : selfEffects)
		ApplySelfEffect(caster, def, *e, ctx, nowUs);

	// 1.5) 장판 - 대상이 아니라 자리에 놓인다. 한 발동에 한 번 (모양에 따라 지점 하나 · 경로 여러 개 · 부채꼴).
	//      이동 스킬의 장판에 dist_cm 이 있으면 실제로 지나간 길에만 그 간격으로 (열화, 2026-10-11)
	if (ctx.targetsOnly == false)
	{
		for (uint32 i = 0; i < def.effects.size(); i++)
		{
			const SkillEffect& e = def.effects[i];
			if (e.type != SkillEffectType::Zone || MeetsCondition(e, ctx, nullptr, anyHit, anyKill, nowUs) == false)
				continue;

			if (e.distCm > 0.f && def.HasMovement())
				StartPathPainter(caster, def, i, ctx, nowUs);
			else
				PlaceZonesFor(caster, def, i, ctx, nowUs);
		}
	}

	// 2) 대상 효과 - seq 순서대로. 표식 조건은 그 대상의 효과를 전부 본 뒤 한 번만 지운다 (피해 + 처형이 같은 표식을 본다)
	for (size_t ti = 0; ti < targets.size(); ti++)
	{
		const CreatureRef& target = targets[ti];
		vector<string> consumedMarks;

		// 반격(정좌) - 대상 지정 · 직선 스킬은 그 대상에게 아무것도 못 하고 반격을 받는다 (3 단계)
		if ((def.shape == SkillShape::Single || def.shape == SkillShape::Line) && TryCounter(target, caster, casterId, nowUs))
			continue;

		for (uint32 i = 0; i < def.effects.size(); i++)
		{
			const SkillEffect& e = def.effects[i];
			if (e.self || e.type == SkillEffectType::Zone)
				continue;

			// param first - 처음 맞은 하나에게만 (방패 돌진의 피해 · 기절)
			if (e.param == "first" && ti > 0)
				continue;

			if (MeetsCondition(e, ctx, target, anyHit, anyKill, nowUs) == false)
				continue;

			if (const char* markKind = RequiredMark(e.condition))
				consumedMarks.push_back(markKind);

			ApplyTargetEffect(caster, def, i, target, ctx, nowUs);

			if (target->IsAlive() == false)
			{
				anyKill = true;
				break;
			}
		}

		for (const string& kind : consumedMarks)
		{
			if (target->status.ConsumeMark(kind, casterId))
				PushStatus(false, target->objectInfo->object_id(), Protocol::STATUS_MARK, kind, 0, 0, 0.f, casterId);
		}

		// 표식을 쓰는 범위기 (낙뢰 1022 - 표식이 남은 적마다 벼락) - 맞은 대상마다 연출 (2026-10-11)
		if (consumedMarks.empty() == false && (def.shape == SkillShape::CircleSelf || def.shape == SkillShape::CirclePoint))
			PushTargetVisual(casterId, def.effectId, target);
	}

	// 3) 결과 조건의 자기 효과 - 적중했으면(hit) · 처치하지 못했으면(no_kill). 투사체 적중에서는 보지 않는다 (해당 스킬 없음)
	for (const SkillEffect& e : def.effects)
	{
		if (ctx.targetsOnly || e.self == false || (e.condition != SkillRequire::Hit && e.condition != SkillRequire::NoKill))
			continue;
		if (MeetsCondition(e, ctx, nullptr, anyHit, anyKill, nowUs) == false)
			continue;

		ApplySelfEffect(caster, def, e, ctx, nowUs);
	}
}

bool Room::MeetsCondition(const SkillEffect& e, const SkillContext& ctx, const CreatureRef& target, bool anyHit, bool anyKill, uint64 nowUs) const
{
	// 재입력의 두 번째 입력은 recast 효과만, 첫 입력은 recast 효과를 빼고
	if (ctx.recast != (e.condition == SkillRequire::Recast))
		return false;

	switch (e.condition)
	{
	case SkillRequire::None:
	case SkillRequire::Recast:
		return true;

	case SkillRequire::MarkSpear:
	case SkillRequire::MarkArrow:
	case SkillRequire::MarkBlood:
	case SkillRequire::MarkHeavyBleed:
	case SkillRequire::MarkRope:
		// 본인 표식만 (사용자 결정) - 남이 남긴 표식은 쓸 수 없다
		return target != nullptr && target->status.HasMark(RequiredMark(e.condition), ctx.casterId);

	case SkillRequire::Stunned:
		return target != nullptr && nowUs < target->stunEndUs;

	case SkillRequire::Hit:
		return anyHit;

	case SkillRequire::NoKill:
		return anyHit && anyKill == false;

	case SkillRequire::Element:
	case SkillRequire::NoElement:
	{
		// 스태프 속성 - 시전자에게 깃든 것이 있는가
		auto casterIt = _objects.find(ctx.casterId);
		CreatureRef caster = (casterIt != _objects.end()) ? dynamic_pointer_cast<Creature>(casterIt->second) : nullptr;
		const bool hasElement = (caster != nullptr && caster->stance.empty() == false);
		return (e.condition == SkillRequire::Element) ? hasElement : hasElement == false;
	}

	case SkillRequire::Wall:
		return false;		// 회오리 벽 충돌은 OnSpinTouchedWall 이 직접 본다

	case SkillRequire::Alone:
	{
		auto casterIt = _objects.find(ctx.casterId);
		CreatureRef caster = (casterIt != _objects.end()) ? dynamic_pointer_cast<Creature>(casterIt->second) : nullptr;
		const SkillDef* def = (caster != nullptr) ? SkillTable::Find(caster->castSkillId) : nullptr;
		return caster != nullptr && IsAlone(caster, def != nullptr ? def->radiusCm : 0.f);
	}

	case SkillRequire::OnDot:
	case SkillRequire::OnDashAtMe:
	case SkillRequire::OnSkill:
	case SkillRequire::OnBasic:
		return false;		// 트리거 - 발동 때가 아니라 OnDotApplied · TriggerDashMarks · OnSkillDamageLanded · ApplyBasicProcs 가 부른다
	}

	return false;
}

void Room::ApplySelfEffect(const CreatureRef& caster, const SkillDef& def, const SkillEffect& e, const SkillContext& ctx, uint64 nowUs)
{
	switch (e.type)
	{
	case SkillEffectType::Movement:
		// dash · leap · forward · back 은 클라가 옮기고 서버는 속도 창만 연다 (방향은 클라 몫 - 서버는 거리 · 속도만 검증).
		// blink 는 서버가 옮긴다 (벽 무시 · 도착 보정). to_mark:<표식> 는 그 표식이 남은 가장 가까운 적 옆으로 (집념)
		if (e.param == "blink")
			BlinkCaster(caster, e, ctx, nowUs);
		else if (e.param.rfind("to_mark:", 0) == 0)
			MoveToMark(caster, def, e, e.param.substr(8), nowUs);
		else if (e.param == "behind")
			MoveBehindTarget(caster, def, e, ctx, nowUs);		// 그림자 밟기 - 대상의 등 뒤로
		else
		{
			ApplyMovement(caster, e, nowUs);

			// 간파 - 나를 향해 질주해 오는 적에게 확신의 표식 (상대의 패시브)
			if (e.param == "dash" || e.param == "leap" || e.param == "forward")
				TriggerDashMarks(caster, ctx.aimX - ctx.originX, ctx.aimY - ctx.originY, nowUs);

			// 질풍노도 (#108) - 날아가는 경로가 벽에 닿으면 requires=wall 효과 (자기 기절 · 주위 감속). 클라는 벽 앞에서 멈춘다
			bool hasWallEffect = false;
			for (const SkillEffect& w : def.effects)
				hasWallEffect |= (w.condition == SkillRequire::Wall);
			if (hasWallEffect && e.distCm > 0.f)
			{
				float dirX = ctx.aimX - ctx.originX;
				float dirY = ctx.aimY - ctx.originY;
				const float len = ::sqrtf(dirX * dirX + dirY * dirY);
				if (len > 0.001f)
				{
					dirX /= len;
					dirY /= len;
					const WallMap& walls = Walls();
					if (walls.SweepBlocked(ctx.originX, ctx.originY, ctx.originX + dirX * e.distCm, ctx.originY + dirY * e.distCm, SkillKinematics::CAPSULE_RADIUS_CM))
					{
						LOG_LINE << "[DASH] wall char=" << ctx.casterId << " skill=" << def.skillId << '\n';
						ApplyWallEffects(caster, def, nowUs);
					}
				}
			}
		}
		break;

	case SkillEffectType::Damage:
	case SkillEffectType::Dot:
	case SkillEffectType::SoftCc:
	case SkillEffectType::HardCc:
	{
		// 자기 자신을 대상으로 (동귀어진의 자해 · 이열치열의 화상 · 냉혈의 빙결 · 감속)
		uint32 index = 0;
		for (; index < def.effects.size(); index++)
			if (&def.effects[index] == &e)
				break;
		ApplyTargetEffect(caster, def, index, caster, ctx, nowUs);
		break;
	}

	case SkillEffectType::Immune:
	case SkillEffectType::DamageMod:
	{
		// duration 0 = 패시브 (장착 슬롯에서 읽는다 - 여기서 할 일 없음). 아니면 param 을 든 버프
		if (e.durationMs == 0)
			break;

		BuffInstance buff;
		buff.skillId = def.skillId;
		buff.stat = (e.type == SkillEffectType::Immune) ? SkillBuffStat::Immune : SkillBuffStat::DamageMod;
		buff.magnitude = (e.type == SkillEffectType::Immune) ? 1.f : e.magnitude + e.perLevel * static_cast<float>(ctx.itemLevel);
		buff.param = e.param;
		buff.endUs = nowUs + static_cast<uint64>(e.durationMs) * 1000;

		caster->status.AddBuff(buff);
		_statusTargets.insert(ctx.casterId);

		// 건곤일척 (param "tag:hammer;still") - 지금 자리를 기억한다. 움직이면 사라지고 망치 스킬 한 번에 쓰인다 (TakeStillBonus)
		if (e.type == SkillEffectType::DamageMod && e.param.find(";still") != string::npos)
			OnStillBuffApplied(caster, def.skillId);

		const string label = string(SkillTable::BuffStatName(buff.stat)) + ":" + e.param;
		PushStatus(true, ctx.casterId, Protocol::STATUS_BUFF, label, 1, e.durationMs, buff.magnitude, ctx.casterId);
		LOG_LINE << "[BUFF] caster=" << ctx.casterId << " skill=" << def.skillId << " " << label << " +" << buff.magnitude << " for=" << e.durationMs << "ms" << '\n';
		break;
	}

	case SkillEffectType::Cleanse:
		Cleanse(caster, e.param, nowUs);
		break;

	case SkillEffectType::Heal:
		Heal(caster, ctx.casterId, static_cast<int32>(::lroundf(caster->maxHp * e.magnitude)), nowUs);
		break;

	case SkillEffectType::Doom:
		StartDoom(caster, def, e, nowUs);
		break;

	case SkillEffectType::Reveal:
		if (e.param == "nearest")
			RevealNearest(caster, def, nowUs);
		break;

	case SkillEffectType::DamageReduceOnce:
		break;		// 패시브 - ApplyDamageMods 가 슬롯에서 읽는다

	case SkillEffectType::SelfBuff:
	{
		if (e.stat == SkillBuffStat::None || e.durationMs == 0)
			break;

		// 크기 = magnitude + per_level × 아이템 강화 레벨 (사용자 결정: 스킬 레벨 = 아이템 강화). 같은 버프는 갱신
		const float magnitude = e.magnitude + e.perLevel * static_cast<float>(ctx.itemLevel);

		BuffInstance buff;
		buff.skillId = def.skillId;
		buff.stat = e.stat;
		buff.magnitude = magnitude;
		buff.endUs = nowUs + static_cast<uint64>(e.durationMs) * 1000;

		caster->status.AddBuff(buff);
		caster->OnBuffsChanged();
		_statusTargets.insert(ctx.casterId);

		PushStatus(true, ctx.casterId, Protocol::STATUS_BUFF, SkillTable::BuffStatName(e.stat), 1, e.durationMs, magnitude, ctx.casterId);

		LOG_LINE << "[BUFF] caster=" << ctx.casterId << " skill=" << def.skillId
			<< " stat=" << SkillTable::BuffStatName(e.stat) << " +" << magnitude << " lv=" << ctx.itemLevel
			<< " for=" << e.durationMs << "ms" << '\n';
		break;
	}

	case SkillEffectType::Hold:
	case SkillEffectType::Counter:
	{
		// 유지형(회오리 · 빈쿨룸 · 정좌) - 다시 누를 때까지 (상한은 duration, 0 = 없음). 정좌는 유지 동안 반격 창
		uint32 index = 0;
		for (; index < def.effects.size(); index++)
			if (&def.effects[index] == &e)
				break;

		if (e.type == SkillEffectType::Counter)
		{
			caster->counterUntilUs = nowUs + static_cast<uint64>(e.durationMs) * 1000;
			caster->counterRatio = e.adRatio;
			caster->counterSkillId = def.skillId;
			PushStatus(true, ctx.casterId, Protocol::STATUS_COUNTER, "counter", 1, e.durationMs, e.adRatio, ctx.casterId);
		}

		if (def.kind == SkillKind::Hold)
			StartHold(caster, def, index, ctx.slot, ctx, nowUs);
		break;
	}

	case SkillEffectType::Parry:
		caster->parryUntilUs = nowUs + static_cast<uint64>(e.durationMs) * 1000;
		PushStatus(true, ctx.casterId, Protocol::STATUS_PARRY, "parry", 1, e.durationMs, 0.f, ctx.casterId);
		LOG_LINE << "[PARRY] start char=" << ctx.casterId << " for=" << e.durationMs << "ms" << '\n';
		break;

	case SkillEffectType::Reflect:
		// 자업자득 (#107) - 창 동안 날아오는 스킬 투사체를 쏜 쪽으로 되돌린다 (TryReflect). 클라 표시는 받아넘기기와 같은 종류에 param reflect
		caster->reflectUntilUs = nowUs + static_cast<uint64>(e.durationMs) * 1000;
		PushStatus(true, ctx.casterId, Protocol::STATUS_PARRY, "reflect", 1, e.durationMs, 0.f, ctx.casterId);
		LOG_LINE << "[REFLECT] start char=" << ctx.casterId << " for=" << e.durationMs << "ms" << '\n';
		break;

	case SkillEffectType::Stance:
		SetStance(caster, e.param, e.apRatio, nowUs);
		break;

	case SkillEffectType::SelfCc:
		// 속성 없이 썼다(no_element) 등 - 자기에게 CC. param at_end 면 효과가 끝나는 때 (오버로드 · 니힐, 2026-10-11 고침 -
		// 예전엔 바로 걸려 오버로드는 버프 중에 기절하고, 니힐은 자기 무적에 막혀 기절하지 않았다)
		if (e.durationMs == 0)
			break;
		if (e.param == "at_end")
			ScheduleSelfCcAtEnd(caster, def, e, nowUs);
		else
			ApplyCc(ctx.casterId, ctx.casterId, e.ccType, e.durationMs, e.magnitude);
		break;

	case SkillEffectType::PetrifyWindow:
		caster->petrifyEndUs = nowUs + static_cast<uint64>(e.durationMs) * 1000;
		caster->petrifyStunMs = static_cast<uint32>(e.magnitude);
		caster->petrifyTargets.clear();
		_windowOwners.insert(ctx.casterId);
		PushStatus(true, ctx.casterId, Protocol::STATUS_STANCE, "petrify", 1, e.durationMs, e.magnitude, ctx.casterId);
		LOG_LINE << "[PETRIFY] window char=" << ctx.casterId << " for=" << e.durationMs << "ms stun=" << e.magnitude << '\n';
		break;

	case SkillEffectType::Invuln:
		// 니힐 - 피해 · CC · 밀기 전부 무효 (4 단계). "기척을 지운다" 는 같은 스킬의 stealth 효과 (FoW, 2026-10-11)
		caster->invulnUntilUs = nowUs + static_cast<uint64>(e.durationMs) * 1000;
		PushStatus(true, ctx.casterId, Protocol::STATUS_INVULN, "invuln", 1, e.durationMs, 0.f, ctx.casterId);
		LOG_LINE << "[INVULN] skill char=" << ctx.casterId << " for=" << e.durationMs << "ms" << '\n';
		break;

	case SkillEffectType::Decoy:
		SpawnDecoy(caster, e, ctx, nowUs);
		break;

	// ── 2차 로스터 (2026-10-11) - RoomSkills2.cpp · RoomVision.cpp ──
	case SkillEffectType::Stealth:
		StartStealth(caster, def.skillId, e.durationMs, e.param.find("break_on_damage") != string::npos, nowUs);
		break;

	case SkillEffectType::VisionArea:
		StartVisionArea(caster, def, e, nowUs);
		break;

	case SkillEffectType::Hawk:
		CastHawk(caster, def, e, nowUs);
		break;

	case SkillEffectType::Scent:
		caster->scentUntilUs = nowUs + static_cast<uint64>(e.durationMs) * 1000;
		PushStatus(true, ctx.casterId, Protocol::STATUS_BUFF, "scent", 1, e.durationMs, 0.f, ctx.casterId);
		LOG_LINE << "[SCENT] char=" << ctx.casterId << " for=" << e.durationMs << "ms" << '\n';
		break;

	case SkillEffectType::Shield:
		ApplyShield(caster, def, e, nowUs);
		break;

	case SkillEffectType::Volley:
		StartVolley(caster, def, e, ctx, nowUs);
		break;

	case SkillEffectType::Clone:
		SpawnClone(caster, def, e, ctx, nowUs);
		break;

	case SkillEffectType::RaiseWall:
		RaiseWall(caster, def, e, ctx, nowUs);
		break;

	default:
		// OnHit · LethalSave 는 패시브 - 평타 · 피해 경로에서 발동한다. FallOnHit 는 FireSkill 이 예고로 돌린다
		break;
	}
}

void Room::ApplyTargetEffect(const CreatureRef& caster, const SkillDef& def, uint32 effectIndex, const CreatureRef& target, const SkillContext& ctx, uint64 nowUs)
{
	const SkillEffect& e = def.effects[effectIndex];
	const uint64 casterId = ctx.casterId;
	const uint64 targetId = target->objectInfo->object_id();
	const SkillRules& rules = SkillTable::Rules();

	switch (e.type)
	{
	case SkillEffectType::Damage:
	{
		if (e.adRatio <= 0.f && e.apRatio <= 0.f)
			break;

		DamageSource src;
		src.skillId = def.skillId;
		src.elem = ElementOf(e.param);
		src.extraMult = (target != caster) ? ctx.bonusDamageMod : 0.f;

		bool isCrit = false;
		const int32 damage = caster->ComputeSkillDamageTo(*target, e.adRatio, e.apRatio, OUT isCrit);
		const int32 remaining = DealDamage(caster, casterId, target, damage, e.DamageType(), isCrit, nowUs, src);

		// 월척 등 - 내 스킬이 맞혔을 때의 패시브 (자해는 제외)
		if (target != caster && damage > 0)
			OnSkillDamageLanded(caster, target, def.skillId, nowUs);

		// 다단 - 간격이 있으면 남은 타를 예약(ResolvePendingHits), 없으면 지금 전부 (0 단계와 같은 총량)
		if (e.hits > 1 && remaining > 0)
		{
			if (e.intervalMs > 0)
			{
				PendingHit& pending = _pendingHits.emplace_back();
				pending.casterId = casterId;
				pending.targetId = targetId;
				pending.skillId = def.skillId;
				pending.effectIndex = effectIndex;
				pending.remaining = e.hits - 1;
				pending.nextAtUs = nowUs + static_cast<uint64>(e.intervalMs) * 1000;
				pending.bonusDamageMod = src.extraMult;
			}
			else
			{
				for (uint32 hit = 1; hit < e.hits && target->IsAlive(); hit++)
				{
					const int32 more = caster->ComputeSkillDamageTo(*target, e.adRatio, e.apRatio, OUT isCrit);
					DealDamage(caster, casterId, target, more, e.DamageType(), isCrit, nowUs, src);
				}
			}
		}
		break;
	}

	case SkillEffectType::ExtendDot:
	{
		// 째깍째깍 - 마법 지속 피해의 남은 시간 연장
		const Protocol::DamageType filter = (e.param == "physical") ? Protocol::DAMAGE_TYPE_PHYSICAL : Protocol::DAMAGE_TYPE_MAGIC;
		const uint32 touched = target->status.ExtendDots(filter, static_cast<uint32>(e.magnitude));
		if (touched > 0)
		{
			for (const DotInstance& d : target->status.Dots())
			{
				if (d.type == filter)
					PushStatus(true, targetId, Protocol::STATUS_DOT, d.kind, target->status.DotStacks(d.kind), static_cast<uint32>((d.endUs > nowUs ? d.endUs - nowUs : 0) / 1000), d.baseDamage, d.instigatorId);
			}
			LOG_LINE << "[EXTEND] target=" << targetId << " by=" << casterId << " dots=" << touched << " +" << e.magnitude << "ms" << '\n';
		}
		break;
	}

	case SkillEffectType::Reveal:
		if (e.param == "skills")
			RevealSkills(caster, target, e.durationMs);
		break;

	case SkillEffectType::HardCc:
	case SkillEffectType::SoftCc:
		if (e.durationMs > 0)
		{
			// 뇌섬 (param facing) - 시전자 쪽을 바라보던 적만 (바라보는 방향과 시전자 방향의 내적 > 0).
			// 플레이어만 본다 - 몬스터의 yaw 는 걷던 방향이라 "바라본다" 의 뜻이 없다 (2026-10-11 "뇌섬에 실명 효과가 적용 안 됨")
			if (e.ccType == Protocol::CC_TYPE_BLIND && e.param == "facing" && target->IsPlayer())
			{
				const float yawRad = target->posInfo->yaw() * 3.14159265f / 180.f;
				const float fx = ::cosf(yawRad);
				const float fy = ::sinf(yawRad);
				const float dx = ctx.originX - target->posInfo->x();
				const float dy = ctx.originY - target->posInfo->y();
				if (dx * fx + dy * fy <= 0.f)
					break;
			}

			// 실명의 크기는 규칙 하나 (blind_view_scale) - 서버는 시간만 세고 클라가 시야를 줄인다
			const float magnitude = (e.ccType == Protocol::CC_TYPE_BLIND && e.magnitude <= 0.f) ? rules.blindViewScale : e.magnitude;
			ApplyCc(targetId, casterId, e.ccType, e.durationMs, magnitude);
		}
		break;

	case SkillEffectType::Knockback:
	{
		float dx = target->posInfo->x() - ctx.originX;
		float dy = target->posInfo->y() - ctx.originY;

		// param near + radius_cm - 그 안의 적만 밀린다 (강궁)
		if (e.param == "near" && e.radiusCm > 0.f && (dx * dx + dy * dy) > e.radiusCm * e.radiusCm)
			break;

		// 겹쳐 있으면 조준 방향으로
		if (dx * dx + dy * dy < 1.f)
		{
			dx = ctx.aimX - ctx.originX;
			dy = ctx.aimY - ctx.originY;
		}

		// param side (질풍노도 #108) - 날아가는 경로의 옆으로. 대상이 경로의 어느 쪽에 있는지(외적 부호)로 방향을 고른다.
		// param 에 nostun 이 있으면 벽에 부딪혀도 기절하지 않는다 (갑옷 밀쳐내기 - 사용자 결정 2026-10-11). "side+nostun" 처럼 붙여 쓴다
		if (e.param.find("side") != string::npos)
		{
			const float ax = ctx.aimX - ctx.originX;
			const float ay = ctx.aimY - ctx.originY;
			const float cross = ax * dy - ay * dx;
			const float sign = (cross >= 0.f) ? 1.f : -1.f;
			dx = -ay * sign;
			dy = ax * sign;
		}

		// param far (저 멀리 1109, 2026-10-11) - 거리 = 시전자의 시야 반경. 벽에 부딪히면 그 순간 기절 + 날아간 거리에 비례한 피해 (ScheduleSlam)
		// ("far" · "near" 는 windows.h 매크로라 변수 이름으로 쓰지 않는다)
		const bool flyFar = (e.param.find("far") != string::npos);
		float baseDist = e.distCm;
		if (flyFar)
			baseDist = caster->IsPlayer() ? VisionRadiusOf(static_pointer_cast<Player>(caster), nowUs) : (std::max)(e.distCm, 800.f);

		// 저항은 거리를 줄인다 (공식 C6). CC 길이는 실제 날아가는 시간이라 클라와 끝점 · 시간이 맞는다
		const float dist = baseDist * (1.f - target->resistance * BaseStats::RESISTANCE_MAX_REDUCTION);
		const bool wallStun = (flyFar == false) && (e.param.find("nostun") == string::npos);
		float moved = 0.f;
		bool hitWall = false;
		PushCreature(target, casterId, dx, dy, dist, e.speedCms, wallStun, nowUs, &moved, &hitWall);

		if (flyFar && hitWall)
			ScheduleSlam(caster, target, def, e, moved, SkillKinematics::TravelMs(moved, e.speedCms), ctx, nowUs);
		break;
	}

	case SkillEffectType::Pull:
		// 부름 (param monsters) - 몬스터만 끌고, 끌린 몬스터는 시전자를 노린다
		if (e.param == "monsters")
		{
			if (target->IsMonster() == false)
				break;
			PullCreature(target, caster, e.speedCms > 0.f ? e.speedCms : rules.pullSpeedCms, nowUs);
			OnMonsterDamaged(static_pointer_cast<Monster>(target), casterId, 0, nowUs);
			break;
		}
		PullCreature(target, caster, e.speedCms > 0.f ? e.speedCms : rules.pullSpeedCms, nowUs);
		break;

	case SkillEffectType::Dot:
	{
		if (e.durationMs == 0 || (e.adRatio <= 0.f && e.apRatio <= 0.f))
			break;

		// 면역 (만독불침 등) - 그 종류는 걸리지 않는다
		if (HasImmunity(target, ElementOf(e.param), nowUs))
		{
			LOG_LINE << "[IMMUNE] target=" << targetId << " ignored dot " << e.param << '\n';
			break;
		}

		// 틱당 피해는 걸 때 시전자 스탯으로 고정 - 완화는 틱마다 대상 방어로 (UpdateStatuses)
		DotInstance dot;
		dot.skillId = def.skillId;
		dot.kind = e.param;
		dot.instigatorId = casterId;
		dot.type = e.DamageType();
		dot.baseDamage = static_cast<float>(caster->SkillPower()) * (max(e.adRatio, 0.f) + max(e.apRatio, 0.f)) * (1.f + caster->outgoingDamagePct);
		dot.endUs = nowUs + static_cast<uint64>(e.durationMs) * 1000;
		dot.nextTickUs = nowUs + static_cast<uint64>(rules.dotTickMs) * 1000;

		const uint32 stacks = target->status.AddDot(dot, rules.dotMaxStacks);
		_statusTargets.insert(targetId);

		PushStatus(true, targetId, Protocol::STATUS_DOT, e.param, stacks, e.durationMs, dot.baseDamage, casterId);

		LOG_LINE << "[DOT] target=" << targetId << " by=" << casterId << " kind=" << e.param
			<< " stacks=" << stacks << " tick=" << dot.baseDamage << '\n';

		// 혈기 - 출혈을 걸면 회복 + 공속 (시전자의 패시브). 자해는 제외
		if (target != caster)
			OnDotApplied(caster, e.param, nowUs);
		break;
	}

	case SkillEffectType::Mark:
	{
		if (e.durationMs == 0 || e.param.empty())
			break;

		MarkInstance mark;
		mark.kind = e.param;
		mark.instigatorId = casterId;
		mark.endUs = nowUs + static_cast<uint64>(e.durationMs) * 1000;

		target->status.AddMark(mark);
		_statusTargets.insert(targetId);

		PushStatus(true, targetId, Protocol::STATUS_MARK, e.param, 1, e.durationMs, e.magnitude, casterId);

		// 일편단심 - 표식이 남은 동안 시전자의 모든 피해는 이 적에게만, 그 대신 × (1 + magnitude)
		if (e.param == "devotion")
		{
			caster->focusTargetId = targetId;
			caster->focusUntilUs = mark.endUs;
			caster->focusBonus = e.magnitude;
		}

		LOG_LINE << "[MARK] target=" << targetId << " by=" << casterId << " kind=" << e.param << '\n';
		break;
	}

	case SkillEffectType::Execute:
	{
		// 몬스터 · 보스 제외 (사용자 결정). 피해 뒤 남은 체력이 최대의 magnitude % 이하면 즉시 처치
		if (target->IsMonster() || target->IsAlive() == false || e.magnitude <= 0.f)
			break;
		if (static_cast<float>(target->hp) * 100.f > static_cast<float>(target->maxHp) * e.magnitude)
			break;

		LOG_LINE << "[EXECUTE] target=" << targetId << " by=" << casterId << " hp=" << target->hp << "/" << target->maxHp << '\n';
		DealDamage(caster, casterId, target, target->hp, Protocol::DAMAGE_TYPE_PHYSICAL, false, nowUs);
		break;
	}

	case SkillEffectType::Chain:
		ApplyChain(caster, target, e, def.effectId, nowUs);
		break;

	case SkillEffectType::Delayed:
	{
		// 잠복 - duration 뒤 터진다 (ResolveBursts). 피해는 지금 시전자 힘으로 고정
		PendingBurst& burst = _pendingBursts.emplace_back();
		burst.casterId = casterId;
		burst.targetId = targetId;
		burst.skillId = def.skillId;
		burst.effectIndex = effectIndex;
		burst.atUs = nowUs + static_cast<uint64>(e.durationMs > 0 ? e.durationMs : rules.delayedBurstMs) * 1000;
		burst.baseDamage = static_cast<float>(caster->SkillPower()) * (max(e.adRatio, 0.f) + max(e.apRatio, 0.f)) * (1.f + caster->outgoingDamagePct);
		burst.type = e.DamageType();
		burst.ctx = ctx;
		PushStatus(true, targetId, Protocol::STATUS_DOT, "incubating", 1, e.durationMs, burst.baseDamage, casterId);
		LOG_LINE << "[BURST] planted target=" << targetId << " by=" << casterId << " in=" << e.durationMs << "ms" << '\n';
		break;
	}

	case SkillEffectType::Throw:
		ApplyThrow(caster, target, e, ctx, nowUs);
		break;

	default:
		// Reveal(FoW 없음) · Trail · FallOnHit - 4 단계 / 제외
		break;
	}
}

int32 Room::DealDamage(const CreatureRef& attacker, uint64 attackerId, const CreatureRef& target, int32 damage, Protocol::DamageType type, bool isCrit, uint64 nowUs, const DamageSource& src)
{
	if (target == nullptr || target->IsAlive() == false)
		return 0;

	if (damage < 0)
		damage = 0;

	// 니힐 · 배수진 - 모든 공격 무효
	if (target->IsInvulnerable(nowUs))
		return target->hp;

	// 모자 로스터 - 일편단심 · 피해 배율 · 면역 · 염두
	damage = ApplyDamageMods(attacker, target, damage, src, nowUs);

	// 벽력일창 - 도약 중에 맞으면 추락: 예고된 내리찍기를 지운다 (4 단계). 피해는 그대로 받는다
	if (target->IsLeaping(nowUs))
	{
		const uint64 leaperId = target->objectInfo->object_id();
		const uint32 leapSkillId = target->leapSkillId;
		_pendingTelegraphs.erase(remove_if(_pendingTelegraphs.begin(), _pendingTelegraphs.end(),
			[leaperId, leapSkillId](const PendingTelegraph& p) { return p.casterId == leaperId && p.skillId == leapSkillId; }), _pendingTelegraphs.end());
		target->leapUntilUs = 0;
		target->leapSkillId = 0;
		LOG_LINE << "[LEAP] fall char=" << leaperId << " hit by=" << attackerId << '\n';
	}

	// 보호막 (전사의 다짐) - 체력보다 먼저 깎인다. 막은 만큼은 S_DAMAGE 에 나가지 않는다
	if (damage > 0 && target->HasShield(nowUs))
	{
		const int32 absorbed = min(damage, target->shieldAmount);
		target->shieldAmount -= absorbed;
		damage -= absorbed;
		LOG_LINE << "[SHIELD] char=" << target->objectInfo->object_id() << " absorbed=" << absorbed << " left=" << target->shieldAmount << '\n';
		if (target->shieldAmount <= 0)
			target->shieldUntilUs = nowUs;		// UpdateWindows 가 끝 상태를 보낸다
	}

	// 치명상 방어 (맹수의 본능) - 죽을 피해를 체력 1 로 막고 뒤로 도약
	if (damage >= target->hp && target->IsPlayer())
		TryLethalSave(static_pointer_cast<Player>(target), attacker, OUT damage, nowUs);

	// 테스트 모드 /t god (2026-10-11) - 체력은 1 에서 멈추고 피해 숫자는 그대로 보여 준다
	const int32 shownDamage = damage;
	if (target->testGod && damage >= target->hp)
		damage = max(0, target->hp - 1);

	const uint64 targetId = target->objectInfo->object_id();
	const int32 remainingHp = target->TakeDamage(damage);

	// 그림자 감추기 - 피해를 받으면 모습이 드러난다 (사용자 결정 2026-10-11). 니힐은 무적이라 여기까지 오지 않는다
	if (damage > 0 && target->stealthBreaksOnDamage && target->IsStealthed(nowUs))
		BreakStealth(target, "damaged", nowUs);

	Protocol::DamageInfo& dmg = _pendingDamages.emplace_back();
	dmg.set_attacker_id(attackerId);
	dmg.set_target_id(targetId);
	dmg.set_damage(shownDamage);
	dmg.set_remaining_hp(remainingHp);
	dmg.set_is_crit(isCrit);
	dmg.set_damage_type(type);

	// 안 보이는 적을 맞혔다 - 그 적만 잠깐 보인다 (2026-10-11). 틱(지속 피해 · 장판)은 맞힌 순간이 아니다
	if (src.tick == false && shownDamage > 0 && attacker != nullptr && attacker->IsPlayer())
		RevealOnHit(attacker, target, nowUs);

	if (target->IsMonster())
		OnMonsterDamaged(static_pointer_cast<Monster>(target), attackerId, damage, nowUs);

	if (attacker != nullptr && attacker->IsMonster())
	{
		MonsterRef monsterAttacker = static_pointer_cast<Monster>(attacker);
		MonsterBrain::NoteEngaged(monsterAttacker->brain, nowUs);
		if (PackSlot* pack = PackOf(*monsterAttacker))
			PackBrain::OnEngaged(pack->state, nowUs);
		GMonsterStats.hits.fetch_add(1);
	}

	// 전투 Flush 안이면 Flush 끝으로 미뤄진다 (KillBatch), 밖이면 바로
	if (remainingHp <= 0)
		KillCreature(target, attacker, attackerId, nowUs);

	return remainingHp;
}

bool Room::TryLethalSave(const PlayerRef& victim, const CreatureRef& attacker, OUT int32& damage, uint64 nowUs)
{
	for (int32 i = 0; i < SKILL_SLOT_COUNT; i++)
	{
		SkillSlot& slot = victim->skillSlots[i];
		if (slot.bindState != Protocol::BIND_BOUND || slot.skillId == 0)
			continue;

		const SkillDef* def = SkillTable::Find(slot.skillId);
		if (def == nullptr || def->kind != SkillKind::Passive)
			continue;

		const SkillEffect* save = def->FindEffect(SkillEffectType::LethalSave);
		if (save == nullptr || nowUs < slot.cooldownEndUs)
			continue;

		// 쿨다운 = duration_ms (사용자 결정: 맹수의 본능에 쿨다운). 피해는 체력 1 을 남기는 만큼으로
		slot.cooldownEndUs = nowUs + static_cast<uint64>(save->durationMs) * 1000;
		damage = max(0, victim->hp - 1);
		NotifyPassiveTrigger(victim, def->skillId);		// 클라 슬롯에 쿨다운이 돈다 (2026-10-11)

		const uint64 victimId = victim->objectInfo->object_id();
		const float x = victim->posInfo->x();
		const float y = victim->posInfo->y();

		// 뒤로 도약 - 가해자 반대쪽. 가해자를 모르거나 겹쳐 있으면 바라보는 반대쪽
		float dx = 0.f;
		float dy = 0.f;
		if (attacker != nullptr)
		{
			dx = x - attacker->posInfo->x();
			dy = y - attacker->posInfo->y();
		}
		if (dx * dx + dy * dy < 1.f)
		{
			const float yawRad = victim->posInfo->yaw() * 3.14159265f / 180.f;
			dx = -::cosf(yawRad);
			dy = -::sinf(yawRad);
		}
		PushCreature(victim, victimId, dx, dy, save->distCm, save->speedCms, /*wallStun*/ false, nowUs);

		// 패시브에 붙은 버프 (이속) - 그 슬롯 아이템의 강화 레벨로
		SkillContext ctx;
		ctx.casterId = victimId;
		ctx.originX = x;
		ctx.originY = y;
		ctx.aimX = x;
		ctx.aimY = y;
		ctx.itemLevel = victim->ItemLevelOfSlot(static_cast<Protocol::EquipSlot>(i + 1));

		for (const SkillEffect& e : def->effects)
		{
			if (e.self && e.type == SkillEffectType::SelfBuff)
				ApplySelfEffect(victim, *def, e, ctx, nowUs);
		}

		// 연출 - 발동을 모두에게 (effect_id 만, 스킬 id 는 숨긴다)
		Protocol::SkillHitInfo& hit = _pendingSkillHits.emplace_back();
		hit.set_caster_id(victimId);
		hit.set_effect_id(def->effectId);
		hit.set_target_id(victimId);
		hit.set_impact_x(x);
		hit.set_impact_y(y);
		hit.set_aim_x(x);
		hit.set_aim_y(y);

		LOG_LINE << "[LETHAL SAVE] char=" << victimId << " skill=" << def->skillId
			<< " hp=" << victim->hp << " cd=" << save->durationMs << "ms" << '\n';
		return true;
	}

	return false;
}

void Room::PushCreature(const CreatureRef& target, uint64 instigatorId, float dirX, float dirY, float distCm, float speedCms, bool wallStun, uint64 nowUs,
	OUT float* movedCm, OUT bool* hitWall)
{
	if (movedCm != nullptr)
		*movedCm = 0.f;
	if (hitWall != nullptr)
		*hitWall = false;

	if (target == nullptr || target->IsAlive() == false || distCm <= 0.f || speedCms <= 0.f)
		return;

	const uint64 targetId = target->objectInfo->object_id();

	// 진입 무적은 밀리지도 않는다 (AC-10). 니힐도. 미끼는 밀리지 않는다 (인형)
	if (target->IsPlayer() && static_pointer_cast<Player>(target)->IsSpawnInvulnerable(nowUs))
		return;
	if (target->IsInvulnerable(nowUs) || target->IsDecoy())
		return;

	const float len = ::sqrtf(dirX * dirX + dirY * dirY);
	if (len < 0.0001f)
		return;
	dirX /= len;
	dirY /= len;

	const SkillKinematics::PushResult r = SkillKinematics::Push(Walls(), target->posInfo->x(), target->posInfo->y(), dirX, dirY, distCm);
	const uint32 travelMs = SkillKinematics::TravelMs(r.distCm, speedCms);

	if (movedCm != nullptr)
		*movedCm = r.distCm;
	if (hitWall != nullptr)
		*hitWall = r.hitWall;

	if (r.moved)
	{
		target->posInfo->set_x(r.endX);
		target->posInfo->set_y(r.endY);
		_dirtyMovers.insert(targetId);
	}

	// 날아가는 동안 넉백 CC - 길이는 실제 시간 그대로 (저항은 거리에서 이미 깎였다)
	if (travelMs > 0)
		ApplyCcRaw(target, instigatorId, Protocol::CC_TYPE_KNOCKBACK, travelMs, 0.f, nowUs);

	// 벽 기절 - 벽에 부딪혀 멈췄다 (skill_rules wall_stun_ms). 보통 CC 처럼 저항이 깎는다
	if (r.hitWall && wallStun)
	{
		const uint32 stunMs = target->ComputeCcDurationMs(SkillTable::Rules().wallStunMs);
		if (stunMs > 0)
			ApplyCcRaw(target, instigatorId, Protocol::CC_TYPE_STUN, stunMs, 0.f, nowUs);
	}

	// 클라는 이 값으로 같은 끝점까지 밀려간다 (S_CC.knockbacks). 거리 0 이어도 보낸다 - 벽에 붙어 못 밀린 것도 연출이 있다
	Protocol::KnockbackInfo& info = _pendingKnockbacks.emplace_back();
	info.set_target_id(targetId);
	info.set_instigator_id(instigatorId);
	info.set_dir_x(dirX);
	info.set_dir_y(dirY);
	info.set_distance(r.distCm);
	info.set_speed(speedCms);

	LOG_LINE << "[PUSH] target=" << targetId << " by=" << instigatorId
		<< " dist=" << r.distCm << "/" << distCm << " ms=" << travelMs << (r.hitWall ? " wall" : "") << '\n';
}

void Room::PullCreature(const CreatureRef& target, const CreatureRef& caster, float speedCms, uint64 nowUs)
{
	if (target == nullptr || target->IsAlive() == false || caster == nullptr || speedCms <= 0.f)
		return;

	const uint64 targetId = target->objectInfo->object_id();
	const uint64 casterId = caster->objectInfo->object_id();

	if (target->IsPlayer() && static_pointer_cast<Player>(target)->IsSpawnInvulnerable(nowUs))
		return;

	const float startX = target->posInfo->x();
	const float startY = target->posInfo->y();

	// 시전자 바로 앞(pull_stop_cm)까지. 사이에 벽이 있으면 끌리지 않는다 (사용자 결정). 유령 신부의 축복은 벽을 지난다
	const SkillKinematics::PushResult r = SkillKinematics::PullToward(Walls(), startX, startY,
		caster->posInfo->x(), caster->posInfo->y(), SkillTable::Rules().pullStopCm, IgnoresWalls(caster));

	if (r.moved == false)
	{
		LOG_LINE << "[PULL] target=" << targetId << " by=" << casterId << (r.hitWall ? " blocked=wall" : " blocked=close") << '\n';
		return;
	}

	target->posInfo->set_x(r.endX);
	target->posInfo->set_y(r.endY);
	_dirtyMovers.insert(targetId);

	const uint32 travelMs = SkillKinematics::TravelMs(r.distCm, speedCms);
	if (travelMs > 0)
		ApplyCcRaw(target, casterId, Protocol::CC_TYPE_KNOCKBACK, travelMs, 0.f, nowUs);

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
	info.set_instigator_id(casterId);
	info.set_dir_x(dirX);
	info.set_dir_y(dirY);
	info.set_distance(r.distCm);
	info.set_speed(speedCms);

	LOG_LINE << "[PULL] target=" << targetId << " by=" << casterId << " dist=" << r.distCm << " ms=" << travelMs << '\n';
}

void Room::BlinkCaster(const CreatureRef& caster, const SkillEffect& e, const SkillContext& ctx, uint64 nowUs)
{
	PlayerRef player = dynamic_pointer_cast<Player>(caster);
	if (player == nullptr || e.distCm <= 0.f)
		return;

	float dx = ctx.aimX - ctx.originX;
	float dy = ctx.aimY - ctx.originY;
	const float len = ::sqrtf(dx * dx + dy * dy);
	float reach = e.distCm;

	if (len >= 1.f)
	{
		dx /= len;
		dy /= len;
		reach = min(len, e.distCm);
	}
	else
	{
		const float yawRad = player->posInfo->yaw() * 3.14159265f / 180.f;
		dx = ::cosf(yawRad);
		dy = ::sinf(yawRad);
	}

	const float wantX = ctx.originX + dx * reach;
	const float wantY = ctx.originY + dy * reach;

	// 벽 · 경계 보정 - 가장 가까운 빈 자리 (skill-system R13 · ADR-0006 § ⑦). 못 찾으면 발동 실패 - 쿨다운은 이미 들어갔다 (TEMP)
	float outX = wantX;
	float outY = wantY;
	if (Walls().FindNearestFree(wantX, wantY, SkillKinematics::CAPSULE_RADIUS_CM, 200.f, OUT outX, OUT outY) == false)
	{
		LOG_LINE << "[BLINK] failed - no free spot char=" << ctx.casterId << " at=(" << wantX << "," << wantY << ")" << '\n';
		return;
	}

	player->posInfo->set_x(outX);
	player->posInfo->set_y(outY);
	_dirtyMovers.insert(ctx.casterId);

	// 본인에게는 보정 S_MOVE - 클라가 그 자리로 옮긴다. 그 전에 보낸 C_MOVE 는 거리 초과로 거부되고 같은 자리로 되돌아온다
	Protocol::S_MOVE snap;
	snap.set_correction(true);
	snap.add_infos()->CopyFrom(*player->posInfo);

	SEND_PACKET_DECLARATION(snap);
	if (auto session = player->session.lock())
		session->Send(sendBuffer);

	LOG_LINE << "[BLINK] char=" << ctx.casterId << " to=(" << outX << "," << outY << ") reach=" << reach << '\n';
}

void Room::ApplyOnHitPassives(const CreatureRef& attacker, const CreatureRef& target, uint64 nowUs)
{
	const uint64 attackerId = attacker->objectInfo->object_id();

	for (int32 i = 0; i < SKILL_SLOT_COUNT; i++)
	{
		const SkillSlot& slot = attacker->skillSlots[i];
		if (slot.bindState != Protocol::BIND_BOUND || slot.skillId == 0)
			continue;

		const SkillDef* def = SkillTable::Find(slot.skillId);
		if (def == nullptr || def->kind != SkillKind::Passive)
			continue;

		for (const SkillEffect& e : def->effects)
		{
			if (target->IsAlive() == false)
				break;

			if (e.type == SkillEffectType::OnHit)
			{
				// 속성이 붙어 있다 (연뢰 번개 · 점화 화염) - 면역(피뢰 · 방염)과 속성 피해 증가(이열치열 · 냉혈)가 본다 (2026-10-11 고침)
				bool isCrit = false;
				const int32 extra = attacker->ComputeSkillDamageTo(*target, e.adRatio, e.apRatio, OUT isCrit);
				DamageSource src;
				src.skillId = def->skillId;
				src.elem = ElementOf(e.param);
				DealDamage(attacker, attackerId, target, extra, e.DamageType(), isCrit, nowUs, src);
			}
			else if (e.type == SkillEffectType::Chain)
			{
				// 뇌신 - 적중한 대상에서 번개가 옮겨 간다 (3 단계)
				ApplyChain(attacker, target, e, def->effectId, nowUs);
			}
		}
	}

	// 스태프 속성 - 속성이 깃든 동안 평타에 그 속성 추가 피해 (AP)
	if (attacker->stanceRatio > 0.f && target->IsAlive())
	{
		bool isCrit = false;
		const int32 extra = attacker->ComputeSkillDamageTo(*target, 0.f, attacker->stanceRatio, OUT isCrit);
		DamageSource src;
		src.elem = attacker->stance;
		DealDamage(attacker, attackerId, target, extra, Protocol::DAMAGE_TYPE_MAGIC, isCrit, nowUs, src);
	}

	// 평타 강화 버프 (혼원일체 · 여진, 2026-10-11) - 그 스킬의 on_basic 효과
	if (attacker->status.BuffMax(SkillBuffStat::BasicProc) > 0.f)
		ApplyBasicProcs(attacker, target, nowUs);
}

void Room::UpdateStatuses(uint64 nowUs)
{
	if (_statusTargets.empty())
		return;

	const SkillRules& rules = SkillTable::Rules();
	vector<uint64> cleared;

	// 스냅샷 - 틱 피해가 사망(KillBatch 로 미뤄짐) · 버프 재계산을 일으켜도 집합은 그대로
	const vector<uint64> ids(_statusTargets.begin(), _statusTargets.end());

	for (uint64 id : ids)
	{
		auto findIt = _objects.find(id);
		CreatureRef target = (findIt != _objects.end()) ? dynamic_pointer_cast<Creature>(findIt->second) : nullptr;
		if (target == nullptr)
		{
			cleared.push_back(id);
			continue;
		}

		if (target->IsAlive())
		{
			target->status.TickDots(nowUs, rules.dotTickMs, [&](const DotInstance& d)
				{
					if (target->IsAlive() == false)
						return;

					const int32 damage = target->ApplyMitigation(d.baseDamage, *target, d.type);

					CreatureRef instigator = nullptr;
					auto instIt = _objects.find(d.instigatorId);
					if (instIt != _objects.end())
						instigator = dynamic_pointer_cast<Creature>(instIt->second);

					DamageSource src;
					src.skillId = d.skillId;
					src.elem = ElementOf(d.kind);
					src.tick = true;
					DealDamage(instigator, d.instigatorId, target, damage, d.type, false, nowUs, src);

					// 과다출혈 - 틱마다 핏자국 (스킬에 trail 효과가 있으면)
					if (target->IsAlive())
						DropTrail(target, d, nowUs);
				});
		}

		const bool buffGone = target->status.Expire(nowUs, [&](Protocol::StatusKind kind, const string& param, uint64 instigatorId, uint32 stacks)
			{
				PushStatus(false, id, kind, param, stacks, 0, 0.f, instigatorId);
			});

		if (buffGone)
			target->OnBuffsChanged();

		if (target->status.Empty())
			cleared.push_back(id);
	}

	for (uint64 id : cleared)
		_statusTargets.erase(id);
}

void Room::ResolvePendingHits(uint64 nowUs)
{
	if (_pendingHits.empty())
		return;

	vector<PendingHit> due;
	due.swap(_pendingHits);

	vector<PendingHit> keep;
	keep.reserve(due.size());

	for (PendingHit& p : due)
	{
		if (nowUs < p.nextAtUs)
		{
			keep.push_back(p);
			continue;
		}

		const SkillDef* def = SkillTable::Find(p.skillId);
		if (def == nullptr || p.effectIndex >= def->effects.size())
			continue;

		const SkillEffect& e = def->effects[p.effectIndex];

		auto casterIt = _objects.find(p.casterId);
		auto targetIt = _objects.find(p.targetId);
		CreatureRef caster = (casterIt != _objects.end()) ? dynamic_pointer_cast<Creature>(casterIt->second) : nullptr;
		CreatureRef target = (targetIt != _objects.end()) ? dynamic_pointer_cast<Creature>(targetIt->second) : nullptr;

		// 시전자가 이번 Flush 에 막 죽었으면 남은 타는 동시다 (TR-combat-022) - 그 전에 죽었으면 끊긴다
		if (caster == nullptr || CanStillStrike(caster) == false || target == nullptr || target->IsAlive() == false)
			continue;

		bool isCrit = false;
		const int32 damage = caster->ComputeSkillDamageTo(*target, e.adRatio, e.apRatio, OUT isCrit);
		DamageSource src;
		src.skillId = p.skillId;
		src.elem = ElementOf(e.param);
		src.extraMult = p.bonusDamageMod;
		DealDamage(caster, p.casterId, target, damage, e.DamageType(), isCrit, nowUs, src);

		if (--p.remaining > 0 && target->IsAlive())
		{
			p.nextAtUs += static_cast<uint64>(e.intervalMs) * 1000;
			keep.push_back(p);
		}
	}

	// 처리 중 새로 예약된 것(없어야 하지만)은 뒤에 붙인다
	keep.insert(keep.end(), _pendingHits.begin(), _pendingHits.end());
	_pendingHits.swap(keep);
}

void Room::PushStatus(bool applied, uint64 targetId, Protocol::StatusKind kind, const string& param, uint32 stacks, uint32 remainingMs, float magnitude, uint64 instigatorId)
{
	Protocol::StatusInfo& info = (applied ? _pendingStatusApplied : _pendingStatusExpired).emplace_back();
	info.set_target_id(targetId);
	info.set_kind(kind);
	info.set_param(param);
	info.set_stacks(stacks);
	info.set_remaining_ms(remainingMs);
	info.set_magnitude(magnitude);
	info.set_instigator_id(instigatorId);
}

// ── 투사체 (2 단계) ──

void Room::LaunchProjectile(Projectile p)
{
	const float len = ::sqrtf(p.dirX * p.dirX + p.dirY * p.dirY);
	if (len > 0.0001f)
	{
		p.dirX /= len;
		p.dirY /= len;
	}
	else
	{
		p.dirX = 1.f;
		p.dirY = 0.f;
	}

	p.id = GNextProjectileId.fetch_add(1);

	Protocol::ProjectileInfo& info = _pendingProjectileSpawns.emplace_back();
	info.set_projectile_id(p.id);
	info.set_caster_id(p.casterId);
	info.set_effect_id(p.effectId);
	info.set_x(p.x);
	info.set_y(p.y);
	info.set_dir_x(p.dirX);
	info.set_dir_y(p.dirY);
	info.set_speed_cms(p.speedCms);
	info.set_range_cm(p.homingTargetId != 0 ? 0.f : p.remainingCm);
	info.set_radius_cm(p.radiusCm);
	info.set_target_id(p.homingTargetId);

	if (p.skillId != 0)
		LOG_LINE << "[PROJ] launch id=" << p.id << " caster=" << p.casterId << " skill=" << p.skillId << " range=" << p.remainingCm
			<< " speed=" << p.speedCms << (p.pierce ? " pierce" : "") << '\n';

	_projectiles.push_back(move(p));
}

void Room::LaunchSkillProjectile(const CreatureRef& caster, const SkillDef& def, const SkillContext& ctx, uint64 nowUs)
{
	Projectile p;
	p.casterId = ctx.casterId;
	p.skillId = def.skillId;
	p.effectId = def.effectId;
	p.x = ctx.originX;
	p.y = ctx.originY;
	p.dirX = ctx.aimX - ctx.originX;
	p.dirY = ctx.aimY - ctx.originY;

	// 조준점이 발밑이면 바라보는 방향 (CollectSkillTargets 와 같은 규칙)
	if (p.dirX * p.dirX + p.dirY * p.dirY <= 1.f)
	{
		const float yawRad = caster->posInfo->yaw() * 3.14159265f / 180.f;
		p.dirX = ::cosf(yawRad);
		p.dirY = ::sinf(yawRad);
	}

	p.speedCms = def.projectileSpeedCms;
	// 폭이 넓은 투사체는 그만큼 넓게 맞힌다 (검의 함성 1112 반원 검기, 2026-10-11)
	p.radiusCm = (std::max)(SkillTable::Rules().projectileRadiusCm, def.widthCm * 0.5f);
	p.remainingCm = def.rangeCm;
	p.pierce = def.pierce;
	p.ctx = ctx;
	p.ctx.targetsOnly = true;
	p.lastStepUs = nowUs;

	// 약속의 한 발 (tags homing:arrow) - 시야 안에 확신의 표식이 남은 적이 있으면 그 적에게 반드시 날아간다 (2026-10-11)
	const string homingMark = HomingMarkOf(def);
	if (homingMark.empty() == false)
	{
		p.homingTargetId = FindHomingMarkTarget(caster, homingMark, p.x, p.y, 0.f, nowUs);
		if (p.homingTargetId != 0)
		{
			p.remainingCm = HOMING_SKILL_MAX_CM;		// 사거리 밖이어도 보이는 표식 대상까지 - 안전 상한만
			LOG_LINE << "[PROJ] homing skill=" << def.skillId << " caster=" << ctx.casterId << " -> " << p.homingTargetId << " mark=" << homingMark << '\n';
		}
	}

	LaunchProjectile(move(p));
}

bool Room::CanProjectileHit(const CreatureRef& caster, const CreatureRef& target, uint64 nowUs) const
{
	if (target == nullptr || target->IsAlive() == false)
		return false;
	if (caster->IsMonster() && target->IsMonster())
		return false;
	if (target->IsPlayer() && static_pointer_cast<Player>(target)->IsSpawnInvulnerable(nowUs))
		return false;
	return true;
}

void Room::StepProjectiles(uint64 nowUs)
{
	if (_projectiles.empty())
		return;

	const WallMap& walls = Walls();

	// 통째로 옮겨 놓고 돈다 - 적중 효과가 다른 투사체를 낳아도(지금은 없다) 벡터가 흔들리지 않는다
	vector<Projectile> flying;
	flying.swap(_projectiles);

	for (Projectile& p : flying)
	{
		const float dtSec = (nowUs > p.lastStepUs) ? static_cast<float>(nowUs - p.lastStepUs) / 1'000'000.f : 0.f;
		p.lastStepUs = nowUs;

		auto casterIt = _objects.find(p.casterId);
		CreatureRef caster = (casterIt != _objects.end()) ? dynamic_pointer_cast<Creature>(casterIt->second) : nullptr;

		bool ended = false;
		if (caster == nullptr)
		{
			EndProjectile(p, Protocol::PROJECTILE_END_LOST, 0);
			ended = true;
		}
		else if (p.homingTargetId != 0)
		{
			ended = StepHoming(p, caster, walls, dtSec, nowUs);
		}
		else
		{
			ended = StepStraight(p, caster, walls, dtSec, nowUs);
		}

		if (ended == false)
			_projectiles.push_back(move(p));
	}
}

bool Room::StepStraight(Projectile& p, const CreatureRef& caster, const WallMap& walls, float dtSec, uint64 nowUs)
{
	float step = p.speedCms * dtSec;
	if (step <= 0.f)
		return false;
	if (step > p.remainingCm)
		step = p.remainingCm;

	const float x0 = p.x;
	const float y0 = p.y;
	const float x1 = x0 + p.dirX * step;
	const float y1 = y0 + p.dirY * step;

	// 벽 - 투사체 반지름으로 스윕. 닿는 비율보다 앞에 있는 대상만 맞는다 (유령 신부의 축복은 벽을 지난다)
	const float wallT = IgnoresWalls(caster) ? 1.f : walls.SweepFirstHit(x0, y0, x1, y1, p.radiusCm);

	struct Candidate { float t; CreatureRef target; };
	vector<Candidate> candidates;

	for (auto& item : _objects)
	{
		if (item.first == p.casterId || find(p.hitIds.begin(), p.hitIds.end(), item.first) != p.hitIds.end())
			continue;

		CreatureRef target = dynamic_pointer_cast<Creature>(item.second);
		if (CanProjectileHit(caster, target, nowUs) == false)
			continue;

		const float t = SkillKinematics::SegmentCircleHit(x0, y0, x1, y1, target->posInfo->x(), target->posInfo->y(), p.radiusCm + SkillKinematics::CAPSULE_RADIUS_CM);
		if (t <= 1.f && t < wallT)
			candidates.push_back({ t, target });
	}

	sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.t < b.t; });

	for (const Candidate& c : candidates)
	{
		OnProjectileHit(p, caster, c.target, nowUs);

		if (p.pierce == false)
		{
			p.x = x0 + p.dirX * step * c.t;
			p.y = y0 + p.dirY * step * c.t;
			EndProjectile(p, Protocol::PROJECTILE_END_HIT, c.target->objectInfo->object_id());
			return true;
		}
	}

	if (wallT < 1.f)
	{
		p.x = x0 + p.dirX * step * wallT;
		p.y = y0 + p.dirY * step * wallT;
		EndProjectile(p, Protocol::PROJECTILE_END_WALL, 0);
		return true;
	}

	p.x = x1;
	p.y = y1;
	p.remainingCm -= step;

	if (p.remainingCm <= 0.f)
	{
		EndProjectile(p, Protocol::PROJECTILE_END_EXPIRED, 0);
		return true;
	}
	return false;
}

bool Room::StepHoming(Projectile& p, const CreatureRef& caster, const WallMap& walls, float dtSec, uint64 nowUs)
{
	auto targetIt = _objects.find(p.homingTargetId);
	CreatureRef target = (targetIt != _objects.end()) ? dynamic_pointer_cast<Creature>(targetIt->second) : nullptr;

	// 대상이 사라지거나 죽으면 허공에 - 필중은 "피할 수 없다" 지 "죽은 뒤에도 맞는다" 가 아니다
	if (target == nullptr || target->IsAlive() == false)
	{
		EndProjectile(p, Protocol::PROJECTILE_END_LOST, 0);
		return true;
	}

	const float tx = target->posInfo->x();
	const float ty = target->posInfo->y();
	float dx = tx - p.x;
	float dy = ty - p.y;
	const float dist = ::sqrtf(dx * dx + dy * dy);
	const float reach = p.radiusCm + SkillKinematics::CAPSULE_RADIUS_CM;
	const float step = p.speedCms * dtSec;

	if (dist > 0.001f)
	{
		p.dirX = dx / dist;
		p.dirY = dy / dist;
	}

	// 이번 스텝에 닿는다 - 사이에 벽이 없어야 한다 (유령 신부의 축복은 예외)
	const bool pierce = IgnoresWalls(caster);
	if (dist <= reach + step)
	{
		if (pierce == false && walls.SweepBlocked(p.x, p.y, tx, ty, p.radiusCm))
		{
			EndProjectile(p, Protocol::PROJECTILE_END_WALL, 0);
			return true;
		}

		p.x = tx;
		p.y = ty;

		if (CanProjectileHit(caster, target, nowUs))
			OnProjectileHit(p, caster, target, nowUs);

		EndProjectile(p, Protocol::PROJECTILE_END_HIT, p.homingTargetId);
		return true;
	}

	if (step <= 0.f)
		return false;

	const float x1 = p.x + p.dirX * step;
	const float y1 = p.y + p.dirY * step;

	// 날아가는 중 벽에 막힌다 (사용자 결정 2026-10-10)
	const float wallT = pierce ? 1.f : walls.SweepFirstHit(p.x, p.y, x1, y1, p.radiusCm);
	if (wallT < 1.f)
	{
		p.x += p.dirX * step * wallT;
		p.y += p.dirY * step * wallT;
		EndProjectile(p, Protocol::PROJECTILE_END_WALL, 0);
		return true;
	}

	p.x = x1;
	p.y = y1;

	// 유도 스킬 화살 (포착 · 약속의 한 발) - 끝없이 쫓지 않게 안전 상한 (평타 투사체는 상한 없음 - 대상이 사거리 안에서 출발한다)
	if (p.skillId != 0)
	{
		p.remainingCm -= step;
		if (p.remainingCm <= 0.f)
		{
			EndProjectile(p, Protocol::PROJECTILE_END_EXPIRED, 0);
			return true;
		}
	}
	return false;
}

void Room::OnProjectileHit(Projectile& p, const CreatureRef& caster, const CreatureRef& target, uint64 nowUs)
{
	const uint64 targetId = target->objectInfo->object_id();
	p.hitIds.push_back(targetId);

	// 받아치기(자업자득) - 스킬 투사체를 쏜 쪽으로 되돌려 보낸다. 받아넘기기보다 먼저 (둘 다 켜져 있으면 되돌리는 쪽이 이긴다)
	if (TryReflect(p, caster, target, nowUs))
		return;

	// 받아넘기기(유수) - 전방에서 오는 투사체는 무효 (평타 · 스킬 모두)
	if (TryParry(target, p.x - p.dirX * 10.f, p.y - p.dirY * 10.f, nowUs))
		return;

	// 평타 도착 - ResolveAttacks 의 근접 경로와 같은 계산, 도착 시점 스탯으로
	if (p.skillId == 0)
	{
		if (TryCounter(target, caster, p.casterId, nowUs))
			return;

		bool isCrit = false;
		int32 damage = caster->ComputeDamageTo(*target, caster->autoAttackType, OUT isCrit);

		if (target->status.HasMark("spear", p.casterId))
			damage = static_cast<int32>(damage * (1.f + SkillTable::Rules().markSpearBasicBonus));

		DamageSource basic;
		basic.basic = true;
		const int32 remainingHp = DealDamage(caster, p.casterId, target, damage, caster->autoAttackType, isCrit, nowUs, basic);

		if (remainingHp > 0 && caster->IsPlayer())
			ApplyOnHitPassives(caster, target, nowUs);

		if (remainingHp > 0 && nowUs < caster->petrifyEndUs && find(caster->petrifyTargets.begin(), caster->petrifyTargets.end(), targetId) == caster->petrifyTargets.end())
			caster->petrifyTargets.push_back(targetId);
		return;
	}

	const SkillDef* def = SkillTable::Find(p.skillId);
	if (def == nullptr)
		return;

	// 재입력 대상 = 투사체가 처음 맞힌 것 (올가미 → 당기기)
	if (caster->recastSkillId == p.skillId && caster->recastTargetId == 0)
		caster->recastTargetId = targetId;

	vector<CreatureRef> one;
	one.push_back(target);
	ApplySkillEffects(caster, *def, one, p.ctx, nowUs);

	LOG_LINE << "[PROJ] hit id=" << p.id << " skill=" << p.skillId << " target=" << targetId << '\n';
}

void Room::EndProjectile(const Projectile& p, Protocol::ProjectileEnd reason, uint64 targetId)
{
	Protocol::ProjectileEndInfo& info = _pendingProjectileEnds.emplace_back();
	info.set_projectile_id(p.id);
	info.set_x(p.x);
	info.set_y(p.y);
	info.set_reason(reason);
	info.set_target_id(targetId);

	if (p.skillId != 0 && reason != Protocol::PROJECTILE_END_HIT)
		LOG_LINE << "[PROJ] end id=" << p.id << " skill=" << p.skillId << " reason=" << static_cast<int32>(reason) << '\n';

	// 평타 투사체는 양이 많아 500 발마다 한 줄 - HIT / WALL / LOST 비율 (필중이 벽에 얼마나 막히는지)
	if (p.skillId == 0)
	{
		static atomic<uint64> basicEnds = 0;
		static atomic<uint64> basicHits = 0;
		static atomic<uint64> basicWalls = 0;
		static atomic<uint64> basicLost = 0;

		if (reason == Protocol::PROJECTILE_END_HIT)			basicHits.fetch_add(1);
		else if (reason == Protocol::PROJECTILE_END_WALL)	basicWalls.fetch_add(1);
		else												basicLost.fetch_add(1);

		const uint64 n = basicEnds.fetch_add(1) + 1;
		if (n % 100 == 0)
			LOG_LINE << "[PROJ] basic ends=" << n << " hit=" << basicHits.load() << " wall=" << basicWalls.load() << " lost=" << basicLost.load() << '\n';
	}
}

// ── 3 단계 ──

void Room::PlaceZone(const CreatureRef& caster, const SkillDef& def, uint32 effectIndex, float x, float y, const SkillContext& ctx, uint64 nowUs)
{
	const SkillEffect& e = def.effects[effectIndex];
	const SkillRules& rules = SkillTable::Rules();

	Zone z;
	z.id = _nextZoneId++;
	z.casterId = ctx.casterId;
	z.skillId = def.skillId;
	z.effectIndex = effectIndex;
	z.x = x;
	z.y = y;
	z.radiusCm = e.radiusCm;
	z.endUs = nowUs + static_cast<uint64>(e.durationMs) * 1000;
	z.intervalMs = e.intervalMs > 0 ? e.intervalMs : rules.dotTickMs;
	z.nextTickUs = nowUs + static_cast<uint64>(z.intervalMs) * 1000;
	z.tickBaseDamage = static_cast<float>(caster->SkillPower()) * (max(e.adRatio, 0.f) + max(e.apRatio, 0.f)) * (1.f + caster->outgoingDamagePct);
	z.tickType = e.DamageType();
	z.ccType = e.ccType;
	z.ccMagnitude = e.magnitude;
	z.param = (e.param == "element_path") ? caster->stance : e.param;

	// 스킬의 지속 피해는 장판 안에 있는 동안 갱신된다 (틱마다 쌓이지 않는다)
	for (const SkillEffect& d : def.effects)
	{
		if (d.type != SkillEffectType::Dot || d.self || d.condition != SkillRequire::None)
			continue;

		DotInstance dot;
		dot.skillId = def.skillId;
		dot.kind = d.param;
		dot.instigatorId = ctx.casterId;
		dot.type = d.DamageType();
		dot.baseDamage = static_cast<float>(caster->SkillPower()) * (max(d.adRatio, 0.f) + max(d.apRatio, 0.f)) * (1.f + caster->outgoingDamagePct);
		z.dotTemplates.push_back(dot);
		z.dotDurationMs = max(z.dotDurationMs, d.durationMs);
	}

	Protocol::ZoneInfo& info = _pendingZoneSpawns.emplace_back();
	info.set_zone_id(z.id);
	info.set_caster_id(z.casterId);
	info.set_effect_id(def.effectId);
	info.set_x(z.x);
	info.set_y(z.y);
	info.set_radius_cm(z.radiusCm);
	info.set_duration_ms(e.durationMs);
	info.set_param(z.param);

	LOG_LINE << "[ZONE] place id=" << z.id << " caster=" << z.casterId << " skill=" << def.skillId << " at=(" << x << "," << y << ") r=" << z.radiusCm
		<< " " << z.param << " for=" << e.durationMs << "ms" << '\n';

	_zones.push_back(move(z));
}

void Room::PlaceZonesFor(const CreatureRef& caster, const SkillDef& def, uint32 effectIndex, const SkillContext& ctx, uint64 nowUs)
{
	const SkillEffect& e = def.effects[effectIndex];
	if (e.radiusCm <= 0.f || e.durationMs == 0)
		return;

	float dirX = ctx.aimX - ctx.originX;
	float dirY = ctx.aimY - ctx.originY;
	const float dirLen = ::sqrtf(dirX * dirX + dirY * dirY);
	if (dirLen > 1.f)
	{
		dirX /= dirLen;
		dirY /= dirLen;
	}
	else
	{
		const float yawRad = caster->posInfo->yaw() * 3.14159265f / 180.f;
		dirX = ::cosf(yawRad);
		dirY = ::sinf(yawRad);
	}

	switch (def.shape)
	{
	case SkillShape::CirclePoint:
	{
		// 지점 - 사거리 밖을 찍었으면 사거리 끝 (판정과 같은 규칙)
		const float reach = min(dirLen, def.rangeCm);
		PlaceZone(caster, def, effectIndex, ctx.originX + dirX * reach, ctx.originY + dirY * reach, ctx, nowUs);
		break;
	}

	case SkillShape::Cone:
		// 부채꼴 - 시전자 바로 앞 (네불라 아르카나 2026-10-11 "시전자 바로 앞쪽에 부채꼴 모양으로")
		PlaceConeZone(caster, def, effectIndex, ctx, nowUs);
		break;

	case SkillShape::Line:
	{
		// 경로 - 길이를 따라 지름 간격으로 (화염 질주 · 속성의 길). 벽에 닿으면 거기까지
		const float stepCm = e.radiusCm * 2.f;
		const WallMap& walls = Walls();
		for (float d = e.radiusCm; d <= def.rangeCm; d += stepCm)
		{
			const float x = ctx.originX + dirX * d;
			const float y = ctx.originY + dirY * d;
			if (walls.SweepBlocked(ctx.originX, ctx.originY, x, y, 0.f))
				break;
			PlaceZone(caster, def, effectIndex, x, y, ctx, nowUs);
		}
		break;
	}

	default:
		PlaceZone(caster, def, effectIndex, ctx.originX, ctx.originY, ctx, nowUs);
		break;
	}
}

void Room::UpdateZones(uint64 nowUs)
{
	if (_zones.empty())
		return;

	const SkillRules& rules = SkillTable::Rules();

	// 묶음 장판(열화의 길) - 이번 틱에 묶음에서 이미 맞은 대상 (groupId, targetId). 같은 길의 작은 원이 겹쳐도 한 번만
	vector<pair<uint64, uint64>> groupHits;

	for (size_t zi = 0; zi < _zones.size(); )
	{
		Zone& z = _zones[zi];

		if (nowUs >= z.endUs)
		{
			_pendingZoneEnds.push_back(z.id);
			_zones.erase(_zones.begin() + zi);
			continue;
		}

		if (nowUs >= z.nextTickUs)
		{
			z.nextTickUs += static_cast<uint64>(z.intervalMs) * 1000;

			// 명경지수 - 주인만, 안에 서 있는 동안 평타 사거리 (2026-10-11). 다른 사람에겐 아무 일도 없다
			if (z.param == "puddle")
			{
				UpdatePuddle(z, nowUs);
				zi++;
				continue;
			}

			auto casterIt = _objects.find(z.casterId);
			CreatureRef caster = (casterIt != _objects.end()) ? dynamic_pointer_cast<Creature>(casterIt->second) : nullptr;

			for (auto& item : _objects)
			{
				if (item.first == z.casterId)
					continue;

				CreatureRef target = dynamic_pointer_cast<Creature>(item.second);
				if (target == nullptr || target->IsAlive() == false)
					continue;
				if (caster != nullptr && CanProjectileHit(caster, target, nowUs) == false)
					continue;
				if (target->IsPlayer() && static_pointer_cast<Player>(target)->IsSpawnInvulnerable(nowUs))
					continue;

				// 원 · 부채꼴 (네불라 아르카나 2026-10-11)
				if (z.Contains(target->posInfo->x(), target->posInfo->y()) == false)
					continue;

				const uint64 targetId = item.first;

				if (z.groupId != 0)
				{
					const pair<uint64, uint64> key(z.groupId, targetId);
					if (find(groupHits.begin(), groupHits.end(), key) != groupHits.end())
						continue;
					groupHits.push_back(key);
				}

				if (z.tickBaseDamage > 0.f)
				{
					const int32 damage = target->ApplyMitigation(z.tickBaseDamage, *target, z.tickType);
					DamageSource src;
					src.skillId = z.skillId;
					src.elem = ElementOf(z.param);
					src.tick = true;
					DealDamage(caster, z.casterId, target, damage, z.tickType, false, nowUs, src);
				}

				// 감속 장판 - 틱 간격 + 여유만큼, 나가면 곧 풀린다
				if (z.ccType != Protocol::CC_TYPE_NONE && target->IsAlive())
					ApplyCc(targetId, z.casterId, z.ccType, z.intervalMs + 200, z.ccMagnitude);

				for (const DotInstance& tmpl : z.dotTemplates)
				{
					if (target->IsAlive() == false)
						break;

					DotInstance dot = tmpl;
					dot.endUs = nowUs + static_cast<uint64>(z.dotDurationMs) * 1000;
					dot.nextTickUs = nowUs + static_cast<uint64>(rules.dotTickMs) * 1000;
					const uint32 stacks = target->status.RefreshOrAddDot(dot, rules.dotMaxStacks);
					_statusTargets.insert(targetId);
					PushStatus(true, targetId, Protocol::STATUS_DOT, dot.kind, stacks, z.dotDurationMs, dot.baseDamage, z.casterId);
				}
			}
		}

		zi++;
	}
}

void Room::StartHold(const CreatureRef& caster, const SkillDef& def, uint32 effectIndex, Protocol::EquipSlot slot, const SkillContext& ctx, uint64 nowUs)
{
	const SkillEffect& e = def.effects[effectIndex];

	caster->holdSkillId = def.skillId;
	caster->holdSlot = slot;
	caster->holdEffectIndex = effectIndex;
	caster->holdStartUs = nowUs;
	caster->holdEndUs = (e.durationMs > 0) ? nowUs + static_cast<uint64>(e.durationMs) * 1000 : 0;
	caster->holdNextTickUs = nowUs;
	caster->holdOriginX = caster->posInfo->x();
	caster->holdOriginY = caster->posInfo->y();
	_holders.insert(ctx.casterId);

	PushStatus(true, ctx.casterId, Protocol::STATUS_HOLD, e.param.empty() ? "hold" : e.param, 1, e.durationMs, 0.f, ctx.casterId);
	LOG_LINE << "[HOLD] start char=" << ctx.casterId << " skill=" << def.skillId << " " << e.param << " max=" << e.durationMs << "ms" << '\n';
}

void Room::StopHold(const CreatureRef& caster, uint64 nowUs, const char* why)
{
	if (caster == nullptr || caster->IsHolding() == false)
		return;

	const uint64 casterId = caster->objectInfo->object_id();
	const uint32 skillId = caster->holdSkillId;
	const SkillDef* def = SkillTable::Find(skillId);

	// 쿨다운은 끝날 때 (R3 의 "효과가 끝나는 순간")
	if (def != nullptr)
	{
		if (SkillSlot* skillSlot = caster->GetSlot(caster->holdSlot))
			skillSlot->cooldownEndUs = nowUs + static_cast<uint64>(def->cooldownMs) * 1000;
	}

	const string param = (def != nullptr && caster->holdEffectIndex < def->effects.size()) ? def->effects[caster->holdEffectIndex].param : "";
	const uint64 heldMs = (nowUs > caster->holdStartUs) ? (nowUs - caster->holdStartUs) / 1000 : 0;

	caster->holdSkillId = 0;
	caster->holdSlot = Protocol::SLOT_NONE;
	caster->holdEffectIndex = 0;
	caster->holdStartUs = caster->holdEndUs = caster->holdNextTickUs = 0;
	_holders.erase(casterId);

	// 정좌 - 유지가 끝나면 반격 창도 닫힌다
	if (caster->counterSkillId == skillId)
	{
		caster->counterUntilUs = 0;
		PushStatus(false, casterId, Protocol::STATUS_COUNTER, "counter", 0, 0, 0.f, casterId);
	}

	PushStatus(false, casterId, Protocol::STATUS_HOLD, param.empty() ? "hold" : param, 0, 0, 0.f, casterId);
	LOG_LINE << "[HOLD] stop char=" << casterId << " skill=" << skillId << " why=" << why << " held=" << heldMs << "ms" << '\n';
}

void Room::UpdateHolds(uint64 nowUs)
{
	if (_holders.empty())
		return;

	const vector<uint64> ids(_holders.begin(), _holders.end());

	for (uint64 id : ids)
	{
		auto findIt = _objects.find(id);
		CreatureRef caster = (findIt != _objects.end()) ? dynamic_pointer_cast<Creature>(findIt->second) : nullptr;
		if (caster == nullptr || caster->IsHolding() == false)
		{
			_holders.erase(id);
			continue;
		}

		if (caster->IsAlive() == false)
		{
			StopHold(caster, nowUs, "dead");
			continue;
		}

		if (caster->holdEndUs != 0 && nowUs >= caster->holdEndUs)
		{
			StopHold(caster, nowUs, "max");
			continue;
		}

		const SkillDef* def = SkillTable::Find(caster->holdSkillId);
		if (def == nullptr || caster->holdEffectIndex >= def->effects.size())
		{
			StopHold(caster, nowUs, "nodef");
			continue;
		}

		const SkillEffect& hold = def->effects[caster->holdEffectIndex];
		if (hold.type != SkillEffectType::Hold)
			continue;		// 정좌 - 틱 없음, 반격 창만

		if (nowUs >= caster->holdNextTickUs)
		{
			const uint32 intervalMs = hold.intervalMs > 0 ? hold.intervalMs : SkillTable::Rules().dotTickMs;
			caster->holdNextTickUs = nowUs + static_cast<uint64>(intervalMs) * 1000;
			HoldTick(caster, *def, hold, nowUs);
		}
	}
}

void Room::HoldTick(const CreatureRef& caster, const SkillDef& def, const SkillEffect& hold, uint64 nowUs)
{
	const uint64 casterId = caster->objectInfo->object_id();
	const float cx = caster->posInfo->x();
	const float cy = caster->posInfo->y();
	const float radius = def.radiusCm;

	SkillContext ctx;
	ctx.casterId = casterId;
	ctx.originX = cx;
	ctx.originY = cy;
	ctx.aimX = cx;
	ctx.aimY = cy;
	ctx.itemLevel = caster->castItemLevel;
	ctx.slot = caster->holdSlot;

	// 빈쿨룸 - 자기도 속박 (ApplyCcRaw 를 거치지 않는다 - 유지를 끊지 않게). 틱 간격 + 여유
	const uint32 ccMs = (hold.intervalMs > 0 ? hold.intervalMs : SkillTable::Rules().dotTickMs) + 200;
	if (hold.param == "chains")
	{
		caster->ApplyHardCc(Protocol::CC_TYPE_ROOT, nowUs, ccMs);
		_ccTargets.insert(casterId);
		Protocol::CcEventInfo& info = _pendingCcApplied.emplace_back();
		info.set_target_id(casterId);
		info.set_instigator_id(casterId);
		info.set_cc_type(Protocol::CC_TYPE_ROOT);
		info.set_duration_ms(ccMs);
	}

	// 회오리 - 휘두르는 망치가 벽에 닿았다 (반경 안에 벽) → 자기 기절 · 주위 감속 · 유지 끝 (4 단계)
	if (hold.param == "spin" && Walls().Overlaps(cx, cy, radius))
	{
		OnSpinTouchedWall(caster, def, nowUs);
		return;
	}

	int32 hitCount = 0;
	for (auto& item : _objects)
	{
		if (item.first == casterId)
			continue;

		CreatureRef target = dynamic_pointer_cast<Creature>(item.second);
		if (CanProjectileHit(caster, target, nowUs) == false)
			continue;

		const float dx = target->posInfo->x() - cx;
		const float dy = target->posInfo->y() - cy;
		if (dx * dx + dy * dy > radius * radius)
			continue;

		hitCount++;

		if (hold.param == "chains")
		{
			ApplyCc(item.first, casterId, Protocol::CC_TYPE_ROOT, ccMs, 0.f);
			continue;
		}

		// 회오리 - 틱 피해 + 스킬의 넉백 효과
		if (hold.adRatio > 0.f || hold.apRatio > 0.f)
		{
			bool isCrit = false;
			const int32 damage = caster->ComputeSkillDamageTo(*target, hold.adRatio, hold.apRatio, OUT isCrit);
			DealDamage(caster, casterId, target, damage, hold.DamageType(), isCrit, nowUs);
		}

		for (uint32 i = 0; i < def.effects.size() && target->IsAlive(); i++)
		{
			const SkillEffect& e = def.effects[i];
			if (e.self == false && e.type == SkillEffectType::Knockback && e.condition == SkillRequire::None)
				ApplyTargetEffect(caster, def, i, target, ctx, nowUs);
		}
	}

	// 연출 - 틱마다 한 번 (effect_id 만)
	Protocol::SkillHitInfo& hit = _pendingSkillHits.emplace_back();
	hit.set_caster_id(casterId);
	hit.set_effect_id(def.effectId);
	hit.set_impact_x(cx);
	hit.set_impact_y(cy);
	hit.set_aim_x(cx);
	hit.set_aim_y(cy);

	if (hitCount > 0)
		LOG_LINE << "[HOLD] tick char=" << casterId << " skill=" << def.skillId << " hits=" << hitCount << '\n';
}

bool Room::TryCounter(const CreatureRef& target, const CreatureRef& attacker, uint64 attackerId, uint64 nowUs)
{
	if (target == nullptr || attacker == nullptr || target->IsCountering(nowUs) == false || attacker == target)
		return false;

	const uint64 targetId = target->objectInfo->object_id();
	const float ratio = target->counterRatio;
	const uint32 skillId = target->counterSkillId;

	// 한 번 받아치면 끝
	target->counterUntilUs = 0;

	bool isCrit = false;
	const int32 damage = target->ComputeSkillDamageTo(*attacker, ratio, 0.f, OUT isCrit);
	DealDamage(target, targetId, attacker, damage, Protocol::DAMAGE_TYPE_PHYSICAL, isCrit, nowUs);

	if (const SkillDef* def = SkillTable::Find(skillId))
	{
		Protocol::SkillHitInfo& hit = _pendingSkillHits.emplace_back();
		hit.set_caster_id(targetId);
		hit.set_effect_id(def->effectId);
		hit.set_target_id(attackerId);
		hit.set_impact_x(attacker->posInfo->x());
		hit.set_impact_y(attacker->posInfo->y());
		hit.set_aim_x(attacker->posInfo->x());
		hit.set_aim_y(attacker->posInfo->y());
	}

	LOG_LINE << "[COUNTER] char=" << targetId << " hit " << attackerId << " dmg=" << damage << '\n';

	if (target->IsHolding() && target->holdSkillId == skillId)
		StopHold(target, nowUs, "countered");
	else
		PushStatus(false, targetId, Protocol::STATUS_COUNTER, "counter", 0, 0, 0.f, targetId);

	return true;
}

bool Room::TryParry(const CreatureRef& target, float fromX, float fromY, uint64 nowUs)
{
	if (target == nullptr || target->IsParrying(nowUs) == false)
		return false;

	// 전방 반평면 - 바라보는 방향과 공격이 오는 방향의 내적
	const float yawRad = target->posInfo->yaw() * 3.14159265f / 180.f;
	const float fx = ::cosf(yawRad);
	const float fy = ::sinf(yawRad);
	const float dx = fromX - target->posInfo->x();
	const float dy = fromY - target->posInfo->y();

	if (dx * fx + dy * fy < 0.f)
		return false;

	LOG_LINE << "[PARRY] char=" << target->objectInfo->object_id() << " deflected" << '\n';
	return true;
}

bool Room::TryReflect(Projectile& p, const CreatureRef& caster, const CreatureRef& target, uint64 nowUs)
{
	// 자업자득 (#107) - 스킬 투사체만, 쏜 쪽이 살아 있을 때만. 원래 투사체는 여기서 끝나고(호출자가 HIT 로 닫는다)
	// 받아친 쪽이 같은 스킬의 투사체를 쏜 쪽을 향해 새로 쏜다 - 피해는 받아친 쪽의 계수로 (새 투사체의 ctx.casterId)
	if (target == nullptr || target->IsReflecting(nowUs) == false || p.skillId == 0)
		return false;
	if (caster == nullptr || caster->IsAlive() == false || caster == target)
		return false;

	const SkillDef* def = SkillTable::Find(p.skillId);
	if (def == nullptr)
		return false;

	SkillContext ctx;
	ctx.casterId = target->objectInfo->object_id();
	ctx.originX = target->posInfo->x();
	ctx.originY = target->posInfo->y();
	ctx.aimX = caster->posInfo->x();
	ctx.aimY = caster->posInfo->y();
	ctx.itemLevel = p.ctx.itemLevel;

	Projectile back;
	back.casterId = ctx.casterId;
	back.skillId = def->skillId;
	back.effectId = def->effectId;
	back.x = ctx.originX;
	back.y = ctx.originY;
	back.dirX = ctx.aimX - ctx.originX;
	back.dirY = ctx.aimY - ctx.originY;
	back.speedCms = p.speedCms;
	back.radiusCm = p.radiusCm;
	back.homingTargetId = (p.homingTargetId != 0) ? p.casterId : 0;
	back.remainingCm = (back.homingTargetId != 0) ? HOMING_SKILL_MAX_CM : def->rangeCm;
	back.pierce = p.pierce;
	back.ctx = ctx;
	back.ctx.targetsOnly = true;
	back.lastStepUs = nowUs;
	back.hitIds.push_back(ctx.casterId);		// 되돌린 투사체가 받아친 본인을 다시 맞히지 않게

	LOG_LINE << "[REFLECT] char=" << ctx.casterId << " sent skill=" << p.skillId << " back to " << p.casterId << '\n';
	LaunchProjectile(move(back));
	return true;
}

bool Room::MeetsWeaponRequirement(const CreatureRef& caster, const SkillDef& def) const
{
	if (def.requiresWeapon.empty())
		return true;

	// 갑옷 스킬 "망치를 들고 있을 때만" - 플레이어의 무기 칸 아이템 종류의 풀 id 로 본다 (item_types.csv skill_pool_id). 몬스터는 못 쓴다
	PlayerRef player = dynamic_pointer_cast<Player>(caster);
	if (player == nullptr)
		return false;

	const ItemEntry& weapon = player->equipped[Protocol::SLOT_WEAPON_PRIMARY - 1];
	if (weapon.instanceId == 0)
		return false;

	const ItemTypeDef* type = ItemTable::Find(weapon.itemTypeId);
	if (type == nullptr || type->skillPoolId != def.requiresWeapon)
		return false;

	if (def.requiresWeaponTag.empty())
		return true;

	// "sword:ghost" - 그 무기의 결속 스킬 중 하나가 태그를 가져야 한다 (백귀야행은 원혼 스킬이 있는 검)
	for (uint32 skillId : { weapon.skillIdPrimary, weapon.skillIdSecondary })
	{
		const SkillDef* s = (skillId != 0) ? SkillTable::Find(skillId) : nullptr;
		if (s != nullptr && s->HasTag(def.requiresWeaponTag))
			return true;
	}
	return false;
}

void Room::ApplyChain(const CreatureRef& caster, const CreatureRef& first, const SkillEffect& e, uint32 effectId, uint64 nowUs)
{
	if (first == nullptr || e.hits == 0 || e.radiusCm <= 0.f)
		return;

	const uint64 casterId = caster->objectInfo->object_id();
	vector<uint64> visited;
	visited.push_back(first->objectInfo->object_id());
	CreatureRef current = first;

	for (uint32 jump = 0; jump < e.hits; jump++)
	{
		CreatureRef next = nullptr;
		float bestSq = e.radiusCm * e.radiusCm;

		for (auto& item : _objects)
		{
			if (item.first == casterId || find(visited.begin(), visited.end(), item.first) != visited.end())
				continue;

			CreatureRef candidate = dynamic_pointer_cast<Creature>(item.second);
			if (CanProjectileHit(caster, candidate, nowUs) == false)
				continue;

			const float dx = candidate->posInfo->x() - current->posInfo->x();
			const float dy = candidate->posInfo->y() - current->posInfo->y();
			const float distSq = dx * dx + dy * dy;
			if (distSq <= bestSq)
			{
				bestSq = distSq;
				next = candidate;
			}
		}

		if (next == nullptr)
			break;

		visited.push_back(next->objectInfo->object_id());

		bool isCrit = false;
		const int32 damage = caster->ComputeSkillDamageTo(*next, e.adRatio, e.apRatio, OUT isCrit);
		DamageSource src;
		src.skillId = effectId;
		src.elem = ElementOf(e.param);
		DealDamage(caster, casterId, next, damage, e.DamageType(), isCrit, nowUs, src);

		Protocol::SkillHitInfo& hit = _pendingSkillHits.emplace_back();
		hit.set_caster_id(casterId);
		hit.set_effect_id(effectId);
		hit.set_target_id(next->objectInfo->object_id());
		hit.set_impact_x(next->posInfo->x());
		hit.set_impact_y(next->posInfo->y());
		hit.set_aim_x(current->posInfo->x());
		hit.set_aim_y(current->posInfo->y());

		LOG_LINE << "[CHAIN] caster=" << casterId << " jump=" << (jump + 1) << " " << current->objectInfo->object_id() << " -> " << next->objectInfo->object_id() << " dmg=" << damage << '\n';
		current = next;
	}
}

void Room::ResolveBursts(uint64 nowUs)
{
	if (_pendingBursts.empty())
		return;

	const SkillRules& rules = SkillTable::Rules();
	vector<PendingBurst> due;
	due.swap(_pendingBursts);

	for (PendingBurst& b : due)
	{
		if (nowUs < b.atUs)
		{
			_pendingBursts.push_back(b);
			continue;
		}

		const SkillDef* def = SkillTable::Find(b.skillId);
		auto targetIt = _objects.find(b.targetId);
		CreatureRef target = (targetIt != _objects.end()) ? dynamic_pointer_cast<Creature>(targetIt->second) : nullptr;
		if (def == nullptr || target == nullptr || target->IsAlive() == false || b.effectIndex >= def->effects.size())
			continue;

		auto casterIt = _objects.find(b.casterId);
		CreatureRef caster = (casterIt != _objects.end()) ? dynamic_pointer_cast<Creature>(casterIt->second) : nullptr;
		const SkillEffect& e = def->effects[b.effectIndex];

		const float tx = target->posInfo->x();
		const float ty = target->posInfo->y();

		PushStatus(false, b.targetId, Protocol::STATUS_DOT, "incubating", 0, 0, 0.f, b.casterId);

		// 터진다 - 대상에게 피해
		const int32 damage = target->ApplyMitigation(b.baseDamage, *target, b.type);
		DamageSource src;
		src.skillId = b.skillId;
		src.elem = ElementOf(e.param);
		DealDamage(caster, b.casterId, target, damage, b.type, false, nowUs, src);

		// radius 안으로 스킬의 지속 피해가 퍼진다 (대상 포함)
		int32 spread = 0;
		for (auto& item : _objects)
		{
			if (item.first == b.casterId)
				continue;

			CreatureRef other = dynamic_pointer_cast<Creature>(item.second);
			if (other == nullptr || other->IsAlive() == false)
				continue;
			if (caster != nullptr && CanProjectileHit(caster, other, nowUs) == false)
				continue;

			const float dx = other->posInfo->x() - tx;
			const float dy = other->posInfo->y() - ty;
			if (dx * dx + dy * dy > e.radiusCm * e.radiusCm)
				continue;

			for (const SkillEffect& d : def->effects)
			{
				if (d.type != SkillEffectType::Dot || d.self || d.condition != SkillRequire::None)
					continue;

				DotInstance dot;
				dot.skillId = def->skillId;
				dot.kind = d.param;
				dot.instigatorId = b.casterId;
				dot.type = d.DamageType();
				dot.baseDamage = b.baseDamage / max(e.apRatio + e.adRatio, 0.001f) * (max(d.adRatio, 0.f) + max(d.apRatio, 0.f));
				dot.endUs = nowUs + static_cast<uint64>(d.durationMs) * 1000;
				dot.nextTickUs = nowUs + static_cast<uint64>(rules.dotTickMs) * 1000;
				const uint32 stacks = other->status.AddDot(dot, rules.dotMaxStacks);
				_statusTargets.insert(item.first);
				PushStatus(true, item.first, Protocol::STATUS_DOT, d.param, stacks, d.durationMs, dot.baseDamage, b.casterId);
				spread++;
			}
		}

		Protocol::SkillHitInfo& hit = _pendingSkillHits.emplace_back();
		hit.set_caster_id(b.casterId);
		hit.set_effect_id(def->effectId);
		hit.set_target_id(b.targetId);
		hit.set_impact_x(tx);
		hit.set_impact_y(ty);
		hit.set_aim_x(tx);
		hit.set_aim_y(ty);

		LOG_LINE << "[BURST] target=" << b.targetId << " by=" << b.casterId << " dmg=" << damage << " spread=" << spread << '\n';
	}
}

void Room::UpdateWindows(uint64 nowUs)
{
	if (_windowOwners.empty())
		return;

	const vector<uint64> ids(_windowOwners.begin(), _windowOwners.end());

	for (uint64 id : ids)
	{
		auto findIt = _objects.find(id);
		CreatureRef owner = (findIt != _objects.end()) ? dynamic_pointer_cast<Creature>(findIt->second) : nullptr;
		if (owner == nullptr)
		{
			_windowOwners.erase(id);
			continue;
		}

		// 경독 - 창이 닫히면 모아 둔 적을 한꺼번에 기절
		if (owner->petrifyEndUs != 0 && nowUs >= owner->petrifyEndUs)
		{
			int32 stunned = 0;
			for (uint64 targetId : owner->petrifyTargets)
			{
				ApplyCc(targetId, id, Protocol::CC_TYPE_STUN, owner->petrifyStunMs, 0.f);
				stunned++;
			}

			LOG_LINE << "[PETRIFY] close char=" << id << " stunned=" << stunned << '\n';
			PushStatus(false, id, Protocol::STATUS_STANCE, "petrify", 0, 0, 0.f, id);

			owner->petrifyEndUs = 0;
			owner->petrifyTargets.clear();
		}

		// 배수진 - 10 초 안에 아무도 못 죽였다 → 그 자리에서 죽는다
		if (owner->doomSkillId != 0 && nowUs >= owner->doomUntilUs)
			LiftDoom(owner, false, nowUs);

		// 은신이 시간이 다 돼 끝났다 - 보이는 것은 다음 시야 갱신이 알아서 (FoW)
		if (owner->stealthSkillId != 0 && nowUs >= owner->stealthUntilUs)
		{
			LOG_LINE << "[STEALTH] end char=" << id << " skill=" << owner->stealthSkillId << " why=expired" << '\n';
			owner->stealthSkillId = 0;
			owner->stealthUntilUs = 0;
			owner->stealthBreaksOnDamage = false;
			PushStatus(false, id, Protocol::STATUS_STEALTH, "stealth", 0, 0, 0.f, id);
		}

		// 보호막 - 시간이 다 됐거나 다 깎였다 (깎인 것은 DealDamage 가 0 으로 만든다)
		if (owner->shieldSkillId != 0 && (nowUs >= owner->shieldUntilUs || owner->shieldAmount <= 0))
		{
			LOG_LINE << "[SHIELD] end char=" << id << (owner->shieldAmount <= 0 ? " why=broken" : " why=expired") << '\n';
			owner->shieldSkillId = 0;
			owner->shieldAmount = 0;
			owner->shieldUntilUs = 0;
			PushStatus(false, id, Protocol::STATUS_BUFF, "shield", 0, 0, 0.f, id);
		}

		if (owner->petrifyEndUs == 0 && owner->doomSkillId == 0 && owner->stealthSkillId == 0 && owner->shieldSkillId == 0)
			_windowOwners.erase(id);
	}
}

void Room::SetStance(const CreatureRef& caster, const string& element, float ratio, uint64 nowUs)
{
	const uint64 casterId = caster->objectInfo->object_id();

	if (element == "clear" || element.empty())
	{
		if (caster->stance.empty())
			return;

		PushStatus(false, casterId, Protocol::STATUS_STANCE, caster->stance, 0, 0, 0.f, casterId);
		LOG_LINE << "[STANCE] clear char=" << casterId << " was=" << caster->stance << '\n';
		caster->stance.clear();
		caster->stanceRatio = 0.f;
		return;
	}

	// 교체 - 지속 없음 (사용자 결정)
	if (caster->stance.empty() == false && caster->stance != element)
		PushStatus(false, casterId, Protocol::STATUS_STANCE, caster->stance, 0, 0, 0.f, casterId);

	caster->stance = element;
	caster->stanceRatio = ratio;
	PushStatus(true, casterId, Protocol::STATUS_STANCE, element, 1, 0, ratio, casterId);
	LOG_LINE << "[STANCE] char=" << casterId << " " << element << " ratio=" << ratio << '\n';
}

void Room::ApplyThrow(const CreatureRef& caster, const CreatureRef& target, const SkillEffect& e, const SkillContext& ctx, uint64 nowUs)
{
	if (target == nullptr || target->IsAlive() == false)
		return;

	const uint64 casterId = ctx.casterId;
	const uint64 targetId = target->objectInfo->object_id();

	// 시전자 뒤로 - 대상 → 시전자 방향으로 (거리 + dist) 만큼
	float dx = caster->posInfo->x() - target->posInfo->x();
	float dy = caster->posInfo->y() - target->posInfo->y();
	const float gap = ::sqrtf(dx * dx + dy * dy);
	if (gap < 1.f)
	{
		const float yawRad = caster->posInfo->yaw() * 3.14159265f / 180.f;
		dx = -::cosf(yawRad);
		dy = -::sinf(yawRad);
	}

	PushCreature(target, casterId, dx, dy, gap + e.distCm, e.speedCms, /*wallStun*/ false, nowUs);

	// 떨어진 자리 둘레의 적에게 피해 (던져진 대상은 이미 스킬 피해를 받았다)
	const float lx = target->posInfo->x();
	const float ly = target->posInfo->y();
	int32 hitCount = 0;

	for (auto& item : _objects)
	{
		if (item.first == casterId || item.first == targetId)
			continue;

		CreatureRef other = dynamic_pointer_cast<Creature>(item.second);
		if (CanProjectileHit(caster, other, nowUs) == false)
			continue;

		const float ox = other->posInfo->x() - lx;
		const float oy = other->posInfo->y() - ly;
		if (ox * ox + oy * oy > e.radiusCm * e.radiusCm)
			continue;

		bool isCrit = false;
		const int32 damage = caster->ComputeSkillDamageTo(*other, e.adRatio, e.apRatio, OUT isCrit);
		DealDamage(caster, casterId, other, damage, e.DamageType(), isCrit, nowUs);
		hitCount++;
	}

	LOG_LINE << "[THROW] target=" << targetId << " by=" << casterId << " to=(" << lx << "," << ly << ") splash=" << hitCount << '\n';
}

// ── 모자 로스터 #78 ~ #104 ──

namespace
{
	// 효과 param → 피해 속성 (면역 · damage_mod elem 이 본다)
	string ElementOfParam(const string& param)
	{
		if (param == "fire" || param == "burn" || param == "fire_path")	return "fire";
		if (param == "lightning")											return "lightning";
		if (param == "poison")												return "poison";
		if (param == "bleed" || param == "heavy_bleed")						return "bleed";
		if (param == "frost")												return "frost";
		return "";
	}
}

string Room::ElementOf(const string& param) const
{
	return ElementOfParam(param);
}

bool Room::HasImmunity(const CreatureRef& creature, const string& elem, uint64 nowUs) const
{
	if (creature == nullptr || elem.empty())
		return false;

	// 버프 (만독불침)
	if (creature->status.HasBuff(SkillBuffStat::Immune, elem))
		return true;

	// 패시브 (피뢰 · 방염) - 장착 슬롯의 패시브 스킬
	for (int32 i = 0; i < SKILL_SLOT_COUNT; i++)
	{
		const SkillSlot& slot = creature->skillSlots[i];
		if (slot.bindState != Protocol::BIND_BOUND || slot.skillId == 0)
			continue;
		const SkillDef* def = SkillTable::Find(slot.skillId);
		if (def == nullptr || def->kind != SkillKind::Passive)
			continue;
		for (const SkillEffect& e : def->effects)
		{
			if (e.type == SkillEffectType::Immune && e.durationMs == 0 && e.param == elem)
				return true;
		}
	}
	return false;
}

bool Room::IgnoresWalls(const CreatureRef& creature) const
{
	return creature != nullptr && creature->status.IgnoresWalls();
}

bool Room::IsAlone(const CreatureRef& creature, float radiusCm) const
{
	if (creature == nullptr || radiusCm <= 0.f)
		return true;

	const uint64 selfId = creature->objectInfo->object_id();
	for (auto& item : _objects)
	{
		if (item.first == selfId)
			continue;
		CreatureRef other = dynamic_pointer_cast<Creature>(item.second);
		if (other == nullptr || other->IsAlive() == false || other->IsDecoy())
			continue;
		const float dx = other->posInfo->x() - creature->posInfo->x();
		const float dy = other->posInfo->y() - creature->posInfo->y();
		if (dx * dx + dy * dy <= radiusCm * radiusCm)
			return false;
	}
	return true;
}

int32 Room::ApplyDamageMods(const CreatureRef& attacker, const CreatureRef& target, int32 damage, const DamageSource& src, uint64 nowUs)
{
	if (damage <= 0)
		return damage;

	float mult = 1.f;

	if (attacker != nullptr && attacker != target)
	{
		// 일편단심 - 표식 상대에게만, 그 대신 증폭
		if (attacker->IsFocusing(nowUs))
		{
			if (target->objectInfo->object_id() != attacker->focusTargetId)
				return 0;
			mult += attacker->focusBonus;
		}

		const SkillDef* skill = (src.skillId != 0) ? SkillTable::Find(src.skillId) : nullptr;
		float distSq = -1.f;

		auto selectorMatches = [&](const string& sel) -> bool
			{
				if (sel.rfind("tag:", 0) == 0)
					return skill != nullptr && skill->HasTag(sel.substr(4));
				if (sel.rfind("elem:", 0) == 0)
					return src.elem.empty() == false && src.elem == sel.substr(5);
				if (sel.rfind("skill:", 0) == 0)
					return src.skillId != 0 && to_string(src.skillId) == sel.substr(6);
				if (sel == "range")
				{
					if (distSq < 0.f)
					{
						const float dx = target->posInfo->x() - attacker->posInfo->x();
						const float dy = target->posInfo->y() - attacker->posInfo->y();
						distSq = dx * dx + dy * dy;
					}
					const float longRange = SkillTable::Rules().longRangeCm;
					return distSq >= longRange * longRange;
				}
				return false;
			};

		// 버프 형태의 damage_mod (내가 검이 된다 · 야성 · 명사수 · 청류 · 이열치열 · 냉혈).
		// ";still"(건곤일척)은 여기서 보지 않는다 - 발동 때 소모해 src.extraMult 로 온다 (TakeStillBonus)
		for (const BuffInstance& b : attacker->status.Buffs())
		{
			if (b.stat == SkillBuffStat::DamageMod && nowUs < b.endUs && b.param.find(";still") == string::npos && selectorMatches(b.param))
				mult += b.magnitude;
		}
		mult += src.extraMult;

		// 패시브 형태 (원한 · 월척)
		for (int32 i = 0; i < SKILL_SLOT_COUNT; i++)
		{
			const SkillSlot& slot = attacker->skillSlots[i];
			if (slot.bindState != Protocol::BIND_BOUND || slot.skillId == 0)
				continue;
			const SkillDef* def = SkillTable::Find(slot.skillId);
			if (def == nullptr || def->kind != SkillKind::Passive)
				continue;
			for (const SkillEffect& e : def->effects)
			{
				if (e.type == SkillEffectType::DamageMod && e.durationMs == 0 && selectorMatches(e.param))
					mult += e.magnitude;
			}
		}
	}

	damage = static_cast<int32>(::lroundf(damage * mult));

	// 면역 (피뢰 · 방염 · 만독불침)
	if (HasImmunity(target, src.elem, nowUs))
	{
		LOG_LINE << "[IMMUNE] target=" << target->objectInfo->object_id() << " " << src.elem << " blocked " << damage << '\n';
		return 0;
	}

	// 염두 - 받는 첫 공격 절반, 그 뒤 쿨다운
	if (damage > 0)
	{
		for (int32 i = 0; i < SKILL_SLOT_COUNT; i++)
		{
			SkillSlot& slot = target->skillSlots[i];
			if (slot.bindState != Protocol::BIND_BOUND || slot.skillId == 0 || nowUs < slot.cooldownEndUs)
				continue;
			const SkillDef* def = SkillTable::Find(slot.skillId);
			if (def == nullptr || def->kind != SkillKind::Passive)
				continue;
			const SkillEffect* reduce = def->FindEffect(SkillEffectType::DamageReduceOnce);
			if (reduce == nullptr)
				continue;

			slot.cooldownEndUs = nowUs + static_cast<uint64>(reduce->durationMs) * 1000;
			const int32 before = damage;
			damage = static_cast<int32>(::lroundf(damage * (1.f - reduce->magnitude)));
			LOG_LINE << "[REDUCE] target=" << target->objectInfo->object_id() << " skill=" << def->skillId << " " << before << " -> " << damage << '\n';
			NotifyPassiveTrigger(target, def->skillId);		// 클라 슬롯에 쿨다운이 돈다 (2026-10-11 "염두 쿨다운 적용되는 거 안 보여")
			break;
		}
	}

	return damage;
}

void Room::ApplyPassiveEffect(const CreatureRef& owner, const SkillDef& def, uint32 effectIndex, const CreatureRef& target, Protocol::EquipSlot slot, float scale, uint64 nowUs)
{
	const SkillEffect& e = def.effects[effectIndex];
	const uint64 ownerId = owner->objectInfo->object_id();

	SkillContext ctx;
	ctx.casterId = ownerId;
	ctx.originX = owner->posInfo->x();
	ctx.originY = owner->posInfo->y();
	ctx.aimX = ctx.originX;
	ctx.aimY = ctx.originY;
	ctx.itemLevel = owner->IsPlayer() ? static_pointer_cast<Player>(owner)->ItemLevelOfSlot(slot) : 0;
	ctx.slot = slot;

	if (e.type == SkillEffectType::Heal)
	{
		Heal(owner, ownerId, static_cast<int32>(::lroundf(owner->maxHp * e.magnitude * scale)), nowUs);
		return;
	}

	if (e.self)
		ApplySelfEffect(owner, def, e, ctx, nowUs);
	else if (target != nullptr)
		ApplyTargetEffect(owner, def, effectIndex, target, ctx, nowUs);
}

void Room::OnSkillDamageLanded(const CreatureRef& attacker, const CreatureRef& target, uint32 skillId, uint64 nowUs)
{
	for (int32 i = 0; i < SKILL_SLOT_COUNT; i++)
	{
		const SkillSlot& slot = attacker->skillSlots[i];
		if (slot.bindState != Protocol::BIND_BOUND || slot.skillId == 0)
			continue;
		const SkillDef* def = SkillTable::Find(slot.skillId);
		if (def == nullptr || def->kind != SkillKind::Passive)
			continue;
		for (uint32 k = 0; k < def->effects.size(); k++)
		{
			const SkillEffect& e = def->effects[k];
			if (e.condition == SkillRequire::OnSkill && e.conditionId == skillId && target->IsAlive())
				ApplyPassiveEffect(attacker, *def, k, target, static_cast<Protocol::EquipSlot>(i + 1), 1.f, nowUs);
		}
	}
}

void Room::OnDotApplied(const CreatureRef& attacker, const string& kind, uint64 nowUs)
{
	for (int32 i = 0; i < SKILL_SLOT_COUNT; i++)
	{
		const SkillSlot& slot = attacker->skillSlots[i];
		if (slot.bindState != Protocol::BIND_BOUND || slot.skillId == 0)
			continue;
		const SkillDef* def = SkillTable::Find(slot.skillId);
		if (def == nullptr || def->kind != SkillKind::Passive)
			continue;
		for (uint32 k = 0; k < def->effects.size(); k++)
		{
			const SkillEffect& e = def->effects[k];
			if (e.condition != SkillRequire::OnDot)
				continue;

			// on_dot:bleed 는 heavy_bleed 도 - 그때는 두 배 (혈기 "과다 출혈이면 증폭")
			const bool heavy = (kind == "heavy_bleed");
			if (e.conditionParam != kind && (e.conditionParam != "bleed" || heavy == false))
				continue;

			ApplyPassiveEffect(attacker, *def, k, nullptr, static_cast<Protocol::EquipSlot>(i + 1), heavy ? 2.f : 1.f, nowUs);
		}
	}
}

void Room::TriggerDashMarks(const CreatureRef& dasher, float dirX, float dirY, uint64 nowUs)
{
	const float len = ::sqrtf(dirX * dirX + dirY * dirY);
	if (len < 1.f || dasher->IsPlayer() == false)
		return;
	dirX /= len;
	dirY /= len;

	constexpr float HALF_ANGLE_COS = 0.866f;		// 30° - "나를 향해"
	const uint64 dasherId = dasher->objectInfo->object_id();

	for (auto& item : _objects)
	{
		if (item.first == dasherId)
			continue;
		CreatureRef watcher = dynamic_pointer_cast<Creature>(item.second);
		if (watcher == nullptr || watcher->IsAlive() == false || watcher->IsPlayer() == false)
			continue;

		for (int32 i = 0; i < SKILL_SLOT_COUNT; i++)
		{
			const SkillSlot& slot = watcher->skillSlots[i];
			if (slot.bindState != Protocol::BIND_BOUND || slot.skillId == 0)
				continue;
			const SkillDef* def = SkillTable::Find(slot.skillId);
			if (def == nullptr || def->kind != SkillKind::Passive)
				continue;

			bool wants = false;
			for (const SkillEffect& e : def->effects)
				wants |= (e.condition == SkillRequire::OnDashAtMe);
			if (wants == false)
				continue;

			const float dx = watcher->posInfo->x() - dasher->posInfo->x();
			const float dy = watcher->posInfo->y() - dasher->posInfo->y();
			const float dist = ::sqrtf(dx * dx + dy * dy);
			const float reach = def->radiusCm > 0.f ? def->radiusCm : 800.f;
			if (dist < 1.f || dist > reach || (dx * dirX + dy * dirY) / dist < HALF_ANGLE_COS)
				continue;

			for (uint32 k = 0; k < def->effects.size(); k++)
			{
				if (def->effects[k].condition == SkillRequire::OnDashAtMe)
					ApplyPassiveEffect(watcher, *def, k, dasher, static_cast<Protocol::EquipSlot>(i + 1), 1.f, nowUs);
			}
			LOG_LINE << "[DASH MARK] watcher=" << item.first << " marked dasher=" << dasherId << '\n';
		}
	}
}

void Room::Cleanse(const CreatureRef& creature, const string& what, uint64 nowUs)
{
	const uint64 id = creature->objectInfo->object_id();
	uint32 dots = 0;

	if (what == "all")
	{
		// CC 전부 (지속 피해 포함). 버프는 남긴다
		auto clear = [&](uint64& endUs, Protocol::CcType type)
			{
				if (endUs == 0 || nowUs >= endUs)
					return;
				endUs = 0;
				Protocol::CcEventInfo& info = _pendingCcExpired.emplace_back();
				info.set_target_id(id);
				info.set_cc_type(type);
			};
		clear(creature->stunEndUs, Protocol::CC_TYPE_STUN);
		clear(creature->rootEndUs, Protocol::CC_TYPE_ROOT);
		clear(creature->launchEndUs, Protocol::CC_TYPE_LAUNCH);
		clear(creature->silenceEndUs, Protocol::CC_TYPE_SILENCE);
		clear(creature->healReductionEndUs, Protocol::CC_TYPE_HEAL_REDUCTION);
		clear(creature->blindEndUs, Protocol::CC_TYPE_BLIND);
		clear(creature->tauntEndUs, Protocol::CC_TYPE_TAUNT);
		creature->tauntSourceId = 0;
		creature->healReductionMagnitude = 0.f;
		if (creature->slowSources.empty() == false)
		{
			creature->slowSources.clear();
			creature->RefreshActiveSlow(nowUs);
			Protocol::CcEventInfo& info = _pendingCcExpired.emplace_back();
			info.set_target_id(id);
			info.set_cc_type(Protocol::CC_TYPE_SLOW);
		}

		for (const DotInstance& d : creature->status.Dots())
			PushStatus(false, id, Protocol::STATUS_DOT, d.kind, 0, 0, 0.f, d.instigatorId);
		dots = creature->status.RemoveDots("");
	}
	else
	{
		for (const DotInstance& d : creature->status.Dots())
		{
			if (d.kind == what)
				PushStatus(false, id, Protocol::STATUS_DOT, d.kind, 0, 0, 0.f, d.instigatorId);
		}
		dots = creature->status.RemoveDots(what);
	}

	LOG_LINE << "[CLEANSE] char=" << id << " " << what << " dots=" << dots << '\n';
}

void Room::Heal(const CreatureRef& creature, uint64 healerId, int32 amount, uint64 nowUs)
{
	if (creature == nullptr || creature->IsAlive() == false || amount <= 0)
		return;

	// 치유 감소 (CC) 적용
	if (nowUs < creature->healReductionEndUs)
		amount = static_cast<int32>(amount * (1.f - creature->healReductionMagnitude));

	const int32 before = creature->hp;
	creature->hp = min(creature->maxHp, creature->hp + amount);
	const int32 healed = creature->hp - before;
	if (healed <= 0)
		return;

	// 회복은 음수 피해로 (DamageInfo.damage < 0) - 클라는 체력만 갱신한다
	Protocol::DamageInfo& dmg = _pendingDamages.emplace_back();
	dmg.set_attacker_id(healerId);
	dmg.set_target_id(creature->objectInfo->object_id());
	dmg.set_damage(-healed);
	dmg.set_remaining_hp(creature->hp);
	dmg.set_is_crit(false);
	dmg.set_damage_type(Protocol::DAMAGE_TYPE_MAGIC);

	LOG_LINE << "[HEAL] char=" << creature->objectInfo->object_id() << " +" << healed << " -> " << creature->hp << '\n';
}

void Room::RevealNearest(const CreatureRef& caster, const SkillDef& def, uint64 nowUs)
{
	PlayerRef player = dynamic_pointer_cast<Player>(caster);
	if (player == nullptr)
		return;

	const uint64 casterId = caster->objectInfo->object_id();
	CreatureRef nearest = nullptr;
	float bestSq = 0.f;

	for (auto& item : _objects)
	{
		if (item.first == casterId)
			continue;
		CreatureRef other = dynamic_pointer_cast<Creature>(item.second);
		if (other == nullptr || other->IsAlive() == false || other->IsDecoy() || other->IsStealthed(nowUs))
			continue;
		const float dx = other->posInfo->x() - caster->posInfo->x();
		const float dy = other->posInfo->y() - caster->posInfo->y();
		const float distSq = dx * dx + dy * dy;
		if (nearest == nullptr || distSq < bestSq)
		{
			nearest = other;
			bestSq = distSq;
		}
	}

	if (nearest == nullptr)
	{
		LOG_LINE << "[REVEAL] char=" << casterId << " nobody on the floor" << '\n';
		return;
	}

	// FoW (2026-10-11) - "위치를 알아낸다" = 그 생물이 duration 동안 시야와 상관없이 보인다. 그 자리 연출은 아래 S_SKILL_HIT
	const SkillEffect* reveal = def.FindEffect(SkillEffectType::Reveal);
	const uint32 revealMs = (reveal != nullptr && reveal->durationMs > 0) ? reveal->durationMs : 4000;
	RevealTo(player, nearest->objectInfo->object_id(), nowUs + static_cast<uint64>(revealMs) * 1000);

	// 시전자에게만 - 그 자리에 스킬 연출 (S_SKILL_HIT, 방송 아님)
	Protocol::S_SKILL_HIT pkt;
	Protocol::SkillHitInfo* hit = pkt.add_hits();
	hit->set_caster_id(casterId);
	hit->set_effect_id(def.effectId);
	hit->set_target_id(nearest->objectInfo->object_id());
	hit->set_impact_x(nearest->posInfo->x());
	hit->set_impact_y(nearest->posInfo->y());
	hit->set_aim_x(nearest->posInfo->x());
	hit->set_aim_y(nearest->posInfo->y());

	SEND_PACKET_DECLARATION(pkt);
	if (auto session = player->session.lock())
		session->Send(sendBuffer);

	LOG_LINE << "[REVEAL] char=" << casterId << " nearest=" << nearest->objectInfo->object_id() << " dist=" << ::sqrtf(bestSq) << '\n';
}

void Room::RevealSkills(const CreatureRef& caster, const CreatureRef& target, uint32 durationMs)
{
	PlayerRef player = dynamic_pointer_cast<Player>(caster);
	if (player == nullptr || target == nullptr)
		return;

	Protocol::S_SKILL_REVEAL pkt;
	pkt.set_target_id(target->objectInfo->object_id());
	pkt.set_duration_ms(durationMs);
	for (int32 i = 0; i < SKILL_SLOT_COUNT; i++)
	{
		const SkillSlot& s = target->skillSlots[i];
		if (s.skillId == 0)
			continue;
		Protocol::SkillInfo* info = pkt.add_skills();
		info->set_slot(static_cast<Protocol::EquipSlot>(i + 1));
		info->set_skill_id(s.skillId);
		info->set_bind_state(s.bindState);
		info->set_shadowed_by(s.shadowedBy);
	}

	SEND_PACKET_DECLARATION(pkt);
	if (auto session = player->session.lock())
		session->Send(sendBuffer);

	LOG_LINE << "[REVEAL] char=" << caster->objectInfo->object_id() << " skills of " << target->objectInfo->object_id() << " n=" << pkt.skills_size() << '\n';
}

void Room::MoveToMark(const CreatureRef& caster, const SkillDef& def, const SkillEffect& e, const string& markKind, uint64 nowUs)
{
	PlayerRef player = dynamic_pointer_cast<Player>(caster);
	if (player == nullptr)
		return;

	const uint64 casterId = caster->objectInfo->object_id();
	const float reach = e.distCm > 0.f ? e.distCm : def.radiusCm;
	CreatureRef best = nullptr;
	float bestSq = reach * reach;

	for (auto& item : _objects)
	{
		if (item.first == casterId)
			continue;
		CreatureRef other = dynamic_pointer_cast<Creature>(item.second);
		if (other == nullptr || other->IsAlive() == false || other->status.HasMark(markKind, casterId) == false)
			continue;
		const float dx = other->posInfo->x() - caster->posInfo->x();
		const float dy = other->posInfo->y() - caster->posInfo->y();
		const float distSq = dx * dx + dy * dy;
		if (distSq <= bestSq)
		{
			best = other;
			bestSq = distSq;
		}
	}

	if (best == nullptr)
	{
		LOG_LINE << "[TO_MARK] char=" << casterId << " no " << markKind << " mark in " << reach << '\n';
		return;
	}

	// 표식 상대 바로 앞 (pull_stop_cm) 으로 - 블링크와 같은 보정 경로
	float dx = caster->posInfo->x() - best->posInfo->x();
	float dy = caster->posInfo->y() - best->posInfo->y();
	const float dist = ::sqrtf(dx * dx + dy * dy);
	if (dist < 1.f)
		return;
	const float stop = SkillTable::Rules().pullStopCm;
	const float wantX = best->posInfo->x() + dx / dist * stop;
	const float wantY = best->posInfo->y() + dy / dist * stop;

	float outX = wantX;
	float outY = wantY;
	if (Walls().FindNearestFree(wantX, wantY, SkillKinematics::CAPSULE_RADIUS_CM, 200.f, OUT outX, OUT outY) == false)
		return;

	player->posInfo->set_x(outX);
	player->posInfo->set_y(outY);
	_dirtyMovers.insert(casterId);

	Protocol::S_MOVE snap;
	snap.set_correction(true);
	snap.add_infos()->CopyFrom(*player->posInfo);
	SEND_PACKET_DECLARATION(snap);
	if (auto session = player->session.lock())
		session->Send(sendBuffer);

	LOG_LINE << "[TO_MARK] char=" << casterId << " -> " << best->objectInfo->object_id() << " at=(" << outX << "," << outY << ")" << '\n';
}

void Room::StartDoom(const CreatureRef& caster, const SkillDef& def, const SkillEffect& e, uint64 nowUs)
{
	const uint64 casterId = caster->objectInfo->object_id();
	caster->doomSkillId = def.skillId;
	caster->doomUntilUs = nowUs + static_cast<uint64>(e.durationMs) * 1000;
	caster->invulnUntilUs = max(caster->invulnUntilUs, caster->doomUntilUs);
	_windowOwners.insert(casterId);

	PushStatus(true, casterId, Protocol::STATUS_INVULN, "doom", 1, e.durationMs, 0.f, casterId);
	LOG_LINE << "[DOOM] start char=" << casterId << " for=" << e.durationMs << "ms" << '\n';
}

void Room::LiftDoom(const CreatureRef& caster, bool killed, uint64 nowUs)
{
	const uint64 casterId = caster->objectInfo->object_id();
	const uint32 skillId = caster->doomSkillId;
	if (skillId == 0)
		return;

	caster->doomSkillId = 0;
	caster->doomUntilUs = 0;
	caster->invulnUntilUs = 0;

	// 저주의 버프 전부 제거
	for (const BuffInstance& b : caster->status.Buffs())
	{
		if (b.skillId == skillId)
			PushStatus(false, casterId, Protocol::STATUS_BUFF, b.param.empty() ? string(SkillTable::BuffStatName(b.stat)) : string(SkillTable::BuffStatName(b.stat)) + ":" + b.param, 0, 0, 0.f, casterId);
	}
	caster->status.RemoveBuffs(skillId);
	caster->OnBuffsChanged();
	PushStatus(false, casterId, Protocol::STATUS_INVULN, "doom", 0, 0, 0.f, casterId);

	if (killed)
	{
		Heal(caster, casterId, caster->maxHp, nowUs);
		LOG_LINE << "[DOOM] lifted char=" << casterId << " - killed a player, full heal" << '\n';
		return;
	}

	// 아무도 못 죽였다 - 그 자리에서 죽는다 (치명상 방어도 못 막는다). 테스트 모드 /t god 는 체력 1 로 남긴다
	if (caster->testGod)
	{
		DealDamage(caster, casterId, caster, caster->hp, Protocol::DAMAGE_TYPE_MAGIC, false, nowUs);
		LOG_LINE << "[DOOM] expired char=" << casterId << " - test god mode, left at 1 hp" << '\n';
		return;
	}
	LOG_LINE << "[DOOM] expired char=" << casterId << " - dies" << '\n';
	const int32 hp = caster->hp;
	caster->TakeDamage(hp);

	Protocol::DamageInfo& dmg = _pendingDamages.emplace_back();
	dmg.set_attacker_id(casterId);
	dmg.set_target_id(casterId);
	dmg.set_damage(hp);
	dmg.set_remaining_hp(0);
	dmg.set_damage_type(Protocol::DAMAGE_TYPE_MAGIC);

	KillCreature(caster, caster, casterId, nowUs);
}

// ── 도발 ──

void Room::StepTaunts(uint64 nowUs)
{
	if (_ccTargets.empty())
		return;

	const SkillRules& rules = SkillTable::Rules();
	const WallMap& walls = Walls();
	constexpr float STEP_SEC = MOVE_FLUSH_MS / 1000.f;		// FlushMoves 간격 - 틱마다 같은 거리

	for (uint64 targetId : _ccTargets)
	{
		auto findIt = _objects.find(targetId);
		CreatureRef target = (findIt != _objects.end()) ? dynamic_pointer_cast<Creature>(findIt->second) : nullptr;
		if (target == nullptr || target->IsAlive() == false || target->IsTaunted(nowUs) == false)
			continue;

		// 넉백 · 끌기가 겹치면 그쪽이 위치를 쓰고 있다 - 이 틱은 쉰다
		if (nowUs < target->knockbackEndUs)
			continue;

		auto sourceIt = _objects.find(target->tauntSourceId);
		CreatureRef source = (sourceIt != _objects.end()) ? dynamic_pointer_cast<Creature>(sourceIt->second) : nullptr;
		if (source == nullptr || source->IsAlive() == false)
		{
			// 시전자가 사라졌다 - 도발이 풀린다
			target->tauntEndUs = 0;
			target->tauntSourceId = 0;
			Protocol::CcEventInfo& info = _pendingCcExpired.emplace_back();
			info.set_target_id(targetId);
			info.set_cc_type(Protocol::CC_TYPE_TAUNT);
			continue;
		}

		const float x0 = target->posInfo->x();
		const float y0 = target->posInfo->y();
		float dx = source->posInfo->x() - x0;
		float dy = source->posInfo->y() - y0;
		const float dist = ::sqrtf(dx * dx + dy * dy);

		// 시전자를 바라본다 (CanTurn 이 막혀 있어 서버가 돌린다)
		if (dist > 1.f)
			target->posInfo->set_yaw(::atan2f(dy, dx) * 180.f / 3.14159265f);

		const float room = dist - rules.tauntStopCm;
		float step = min(rules.tauntSpeedCms * STEP_SEC, room);
		if (dist > 1.f && step > 0.f)
		{
			dx /= dist;
			dy /= dist;

			// 벽에 닿으면 그 앞에서 멈춘다 (끌려가다 벽에 끼지 않게 - 캡슐 반지름 스윕)
			const float x1 = x0 + dx * step;
			const float y1 = y0 + dy * step;
			const float t = walls.SweepFirstHit(x0, y0, x1, y1, SkillKinematics::CAPSULE_RADIUS_CM);
			step *= t;

			if (step > 0.01f)
			{
				target->posInfo->set_x(x0 + dx * step);
				target->posInfo->set_y(y0 + dy * step);
			}
		}

		target->posInfo->set_state(Protocol::MOVE_STATE_RUN);
		_dirtyMovers.insert(targetId);

		// 본인 클라는 보정으로만 움직인다 - 매 틱 5 cm 안팎이라 끊겨 보이지 않는다
		if (target->IsPlayer())
		{
			Protocol::S_MOVE snap;
			snap.set_correction(true);
			snap.add_infos()->CopyFrom(*target->posInfo);
			SEND_PACKET_DECLARATION(snap);
			if (auto session = static_pointer_cast<Player>(target)->session.lock())
				session->Send(sendBuffer);
		}
	}
}

// ── 4 단계 ──

void Room::SpawnDecoy(const CreatureRef& caster, const SkillEffect& e, const SkillContext& ctx, uint64 nowUs)
{
	const SkillRules& rules = SkillTable::Rules();
	const int32 decoyHp = static_cast<int32>(e.magnitude > 0.f ? e.magnitude : static_cast<float>(rules.decoyHp));

	auto decoy = make_shared<Decoy>(GNextDecoyId.fetch_add(1), ctx.casterId, nowUs + static_cast<uint64>(e.durationMs) * 1000, max(1, decoyHp));
	decoy->posInfo->set_x(caster->posInfo->x());
	decoy->posInfo->set_y(caster->posInfo->y());
	decoy->posInfo->set_z(caster->posInfo->z());
	decoy->posInfo->set_yaw(caster->posInfo->yaw());
	decoy->posInfo->set_state(Protocol::MOVE_STATE_IDLE);

	if (AddObject(decoy) == false)
		return;

	const uint64 id = decoy->objectInfo->object_id();
	_decoys.insert(id);
	_pendingSpawns.push_back(id);

	PushStatus(true, ctx.casterId, Protocol::STATUS_DECOY, "decoy", 1, e.durationMs, 0.f, ctx.casterId);
	LOG_LINE << "[DECOY] spawn id=" << id << " owner=" << ctx.casterId << " hp=" << decoy->hp << " for=" << e.durationMs << "ms" << '\n';
}

void Room::DespawnDecoy(const CreatureRef& decoy, const char* why)
{
	const uint64 id = decoy->objectInfo->object_id();
	const uint64 ownerId = decoy->IsDecoy() ? static_pointer_cast<Decoy>(decoy)->ownerId : 0;
	const bool isClone = decoy->IsDecoy() && static_pointer_cast<Decoy>(decoy)->isClone;

	_decoys.erase(id);
	_clones.erase(id);
	_statusTargets.erase(id);
	RemoveObject(id);

	Protocol::S_DESPAWN despawnPkt;
	despawnPkt.add_object_ids(id);
	SEND_PACKET_BROADCAST(despawnPkt);

	if (ownerId != 0)
		PushStatus(false, ownerId, Protocol::STATUS_DECOY, isClone ? "clone" : "decoy", 0, 0, 0.f, ownerId);

	LOG_LINE << "[DECOY] despawn id=" << id << " why=" << why << '\n';
}

void Room::UpdateDecoys(uint64 nowUs)
{
	if (_decoys.empty())
		return;

	const vector<uint64> ids(_decoys.begin(), _decoys.end());
	for (uint64 id : ids)
	{
		auto findIt = _objects.find(id);
		if (findIt == _objects.end())
		{
			_decoys.erase(id);
			continue;
		}

		CreatureRef decoy = dynamic_pointer_cast<Creature>(findIt->second);
		if (decoy == nullptr || decoy->IsDecoy() == false)
		{
			_decoys.erase(id);
			continue;
		}

		if (nowUs >= static_pointer_cast<Decoy>(decoy)->expiresUs)
			DespawnDecoy(decoy, "expired");
	}
}

void Room::DropTrail(const CreatureRef& bleeder, const DotInstance& dot, uint64 nowUs)
{
	const SkillDef* def = SkillTable::Find(dot.skillId);
	if (def == nullptr)
		return;

	const SkillEffect* trail = def->FindEffect(SkillEffectType::Trail);
	if (trail == nullptr)
		return;

	const float x = bleeder->posInfo->x();
	const float y = bleeder->posInfo->y();

	// 서 있으면 한 자리에 쌓이지 않게 - 같은 주인의 마지막 자국에서 pickup 거리 안이면 건너뛴다
	for (auto it = _trails.rbegin(); it != _trails.rend(); ++it)
	{
		if (it->ownerId != dot.instigatorId)
			continue;
		const float dx = it->x - x;
		const float dy = it->y - y;
		const float pickup = SkillTable::Rules().trailPickupCm;
		if (dx * dx + dy * dy < pickup * pickup)
			return;
		break;
	}

	TrailPoint t;
	t.zoneId = _nextZoneId++;
	t.ownerId = dot.instigatorId;
	t.x = x;
	t.y = y;
	t.expiresUs = nowUs + static_cast<uint64>(trail->durationMs) * 1000;
	t.buffMagnitude = trail->magnitude;
	t.skillId = dot.skillId;
	_trails.push_back(t);

	Protocol::ZoneInfo& info = _pendingZoneSpawns.emplace_back();
	info.set_zone_id(t.zoneId);
	info.set_caster_id(t.ownerId);
	info.set_effect_id(def->effectId);
	info.set_x(x);
	info.set_y(y);
	info.set_radius_cm(SkillTable::Rules().trailPickupCm * 0.5f);
	info.set_duration_ms(trail->durationMs);
	info.set_param("blood");
}

void Room::UpdateTrails(uint64 nowUs)
{
	if (_trails.empty())
		return;

	const SkillRules& rules = SkillTable::Rules();
	const float pickupSq = rules.trailPickupCm * rules.trailPickupCm;
	constexpr uint64 TRAIL_STATUS_RESEND_US = 500'000;		// 밟고 있는 동안 버프는 매 Flush 갱신, S_STATUS 는 0.5 초에 한 번

	// 핏자국은 밟아도 지워지지 않는다 - 시간이 지나면 생긴 순서대로 사라진다 (사용자 결정 2026-10-11)
	for (size_t i = 0; i < _trails.size(); )
	{
		if (nowUs >= _trails[i].expiresUs)
		{
			_pendingZoneEnds.push_back(_trails[i].zoneId);
			_trails.erase(_trails.begin() + i);
			continue;
		}
		i++;
	}

	// 주인이 자기 핏자국 위에 서 있으면 이속 버프 (같은 버프는 갱신). 한 주인에 한 번
	vector<uint64> refreshed;
	for (const TrailPoint& t : _trails)
	{
		if (find(refreshed.begin(), refreshed.end(), t.ownerId) != refreshed.end())
			continue;

		auto ownerIt = _objects.find(t.ownerId);
		CreatureRef owner = (ownerIt != _objects.end()) ? dynamic_pointer_cast<Creature>(ownerIt->second) : nullptr;
		if (owner == nullptr || owner->IsAlive() == false)
			continue;

		const float dx = owner->posInfo->x() - t.x;
		const float dy = owner->posInfo->y() - t.y;
		if (dx * dx + dy * dy > pickupSq)
			continue;

		refreshed.push_back(t.ownerId);

		BuffInstance buff;
		buff.skillId = t.skillId;
		buff.stat = SkillBuffStat::MoveSpeed;
		buff.magnitude = t.buffMagnitude;
		buff.endUs = nowUs + static_cast<uint64>(rules.trailBuffMs) * 1000;
		const bool fresh = owner->status.AddBuff(buff);
		owner->OnBuffsChanged();
		_statusTargets.insert(t.ownerId);

		uint64& lastPushUs = _trailBuffPushedUs[t.ownerId];
		if (fresh || nowUs >= lastPushUs + TRAIL_STATUS_RESEND_US)
		{
			lastPushUs = nowUs;
			PushStatus(true, t.ownerId, Protocol::STATUS_BUFF, SkillTable::BuffStatName(SkillBuffStat::MoveSpeed), 1, rules.trailBuffMs, t.buffMagnitude, t.ownerId);
			if (fresh)
				LOG_LINE << "[TRAIL] on char=" << t.ownerId << " +" << t.buffMagnitude << " ms for=" << rules.trailBuffMs << "ms" << '\n';
		}
	}
}

void Room::OnSpinTouchedWall(const CreatureRef& caster, const SkillDef& def, uint64 nowUs)
{
	const uint64 casterId = caster->objectInfo->object_id();
	const float cx = caster->posInfo->x();
	const float cy = caster->posInfo->y();

	LOG_LINE << "[HOLD] wall char=" << casterId << " skill=" << def.skillId << '\n';
	StopHold(caster, nowUs, "wall");
	ApplyWallEffects(caster, def, nowUs);
}

void Room::ApplyWallEffects(const CreatureRef& caster, const SkillDef& def, uint64 nowUs)
{
	const uint64 casterId = caster->objectInfo->object_id();
	const float cx = caster->posInfo->x();
	const float cy = caster->posInfo->y();

	// requires=wall 효과 - 자기 기절(self_cc) · 주위 감속(soft_cc, radius_cm). 회오리(유지 중 벽) · 질풍노도(대시 경로가 벽에 닿음)
	for (const SkillEffect& e : def.effects)
	{
		if (e.condition != SkillRequire::Wall)
			continue;

		if (e.self && e.type == SkillEffectType::SelfCc && e.durationMs > 0)
		{
			ApplyCc(casterId, casterId, e.ccType, e.durationMs, e.magnitude);
			continue;
		}

		if (e.self == false && e.type == SkillEffectType::SoftCc && e.durationMs > 0)
		{
			const float radius = e.radiusCm > 0.f ? e.radiusCm : def.radiusCm;
			for (auto& item : _objects)
			{
				if (item.first == casterId)
					continue;

				CreatureRef target = dynamic_pointer_cast<Creature>(item.second);
				if (CanProjectileHit(caster, target, nowUs) == false)
					continue;

				const float dx = target->posInfo->x() - cx;
				const float dy = target->posInfo->y() - cy;
				if (dx * dx + dy * dy <= radius * radius)
					ApplyCc(item.first, casterId, e.ccType, e.durationMs, e.magnitude);
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
	// 유령 신랑의 저주 - effect.ignores_walls 면 창 동안 C_MOVE 벽 검사를 건너뛴다 (클라도 같은 열을 읽어 스윕을 건너뛴다)
	player->moveIgnoresWallsUntilUs = effect.ignoresWalls ? player->moveExceptionEndUs : 0;

	LOG_LINE << "[DASH WINDOW] caster=" << player->objectInfo->object_id()
		<< " speed=" << effect.speedCms
		<< " dur=" << (durUs / 1000) << "ms"
		<< (effect.ignoresWalls ? " walls=pass" : "") << '\n';
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

	// 치유 패킷이 없다 - 몬스터 귀환과 같은 방식으로 피해 0 · 남은 체력을 보낸다 (클라 OnDamaged 가 재동기화로 읽는다).
	// 새 최대치도 싣는다 - 예전엔 방 안의 다른 클라가 입장 스냅샷의 최대치를 런 끝까지 썼다 (체력 바 비율이 틀림)
	if (player->ApplyEquipmentStats(false) && player->IsAlive())
	{
		Protocol::DamageInfo& resync = _pendingDamages.emplace_back();
		resync.set_attacker_id(0);
		resync.set_target_id(player->objectInfo->object_id());
		resync.set_damage(0);
		resync.set_remaining_hp(player->hp);
		resync.set_max_hp(player->maxHp);
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

	// 미끼 - 죽음이 아니라 사라짐 (S_DIED 없음, 사망 커밋 없음)
	if (victim->IsDecoy())
	{
		DespawnDecoy(victim, "hit");
		return;
	}

	// 원인은 가해자 기준이다 - 예전에는 피해자 종류를 넣어 몬스터에게 죽은 플레이어가 PLAYER 로 나갔다 (monster-ai.md Core Rule 19)
	Protocol::DiedInfo& died = _pendingDeaths.emplace_back();
	died.set_victim_id(victimId);
	died.set_instigator_id(instigatorId);
	died.set_cause((instigator != nullptr && instigator->IsMonster()) ? Protocol::DEATH_CAUSE_MONSTER : Protocol::DEATH_CAUSE_PLAYER);

	// 죽은 대상이 공격 중이었다면 그 공격도 없앤다.
	victim->attackTargetId = 0;
	victim->attackHitAtUs = 0;
	CancelCast(victim);

	// 배수진 - 저주 중에 적 플레이어를 처치했다 → 해제 + 만피
	if (instigator != nullptr && victim->IsPlayer() && instigator->IsDoomed(nowUs))
		LiftDoom(instigator, true, nowUs);

	// 일편단심 - 표식 대상이 죽으면 집중도 풀린다 (남은 시간 동안 다른 적에게 피해 0 이 되지 않게)
	for (auto& item : _objects)
	{
		CreatureRef other = dynamic_pointer_cast<Creature>(item.second);
		if (other != nullptr && other->focusTargetId == victimId)
		{
			other->focusTargetId = 0;
			other->focusUntilUs = 0;
		}
	}

	// 상태 효과 · 재입력 · 유지 · 창은 죽음과 함께 끝난다 - 클라는 S_DIED 로 지운다 (따로 expired 를 보내지 않는다)
	if (victim->IsHolding())
		StopHold(victim, nowUs, "died");
	victim->status.Clear();
	victim->OnBuffsChanged();
	victim->recastSkillId = 0;
	victim->counterUntilUs = 0;
	victim->parryUntilUs = 0;
	victim->reflectUntilUs = 0;
	victim->petrifyEndUs = 0;
	victim->petrifyTargets.clear();
	_statusTargets.erase(victimId);
	_windowOwners.erase(victimId);

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

	// 무리 - 순찰 길의 시작점에 줄을 서서 태어난다 (2026-10-11 몬스터 개선)
	InitPacks(nowUs);
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
	// 테스트 모드 더미는 자리(슬롯) 없이 산다 - 몬스터를 끈 서버(FBTK_MONSTERS=0)에서도 판단 · 시체 정리가 돌아야 한다
	if (_monsterSlots.empty() && _monsters.empty() && _packs.empty())
		return;

	if (_dummySpawns.empty() == false)
		DropOrphanDummies();

	// ── 시체 유예가 끝난 몬스터를 방에서 뺀다 ──
	{
		Protocol::S_DESPAWN despawnPkt;
		for (auto it = _monsters.begin(); it != _monsters.end(); )
		{
			const MonsterRef& m = it->second;
			if (m->diedAtUs != 0 && nowUs >= m->diedAtUs + MonsterConfig::CORPSE_LINGER_US)
			{
				const uint64 deadId = it->first;
				despawnPkt.add_object_ids(deadId);
				RemoveObject(deadId);
				it = _monsters.erase(it);
				_dummySpawns.erase(deadId);		// 더미의 자리 - 몬스터가 가리키던 것이라 몬스터를 지운 뒤에
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

		// 진입 무적 중인 플레이어는 감지 · 목표가 아니다 (monster-ai.md Core Rule 12). 은신 · 야생의 체취도 (FoW, 2026-10-11)
		const Player* player = static_cast<const Player*>(object.get());
		if (player->IsAlive() && player->IsSpawnInvulnerable(nowUs) == false && player->IsStealthed(nowUs) == false && player->IsScented(nowUs) == false)
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
	const WallMap& walls = Walls();
	auto lineClear = [&walls](float ax, float ay, float bx, float by) { return walls.SweepBlocked(ax, ay, bx, by, 0.f) == false; };

	for (auto& [id, m] : _monsters)
	{
		if (m->diedAtUs != 0)
			continue;

		// 무리의 몸 - 판단은 무리 단위로 아래 UpdatePacks 에서 (2026-10-11)
		if (m->packSlot != 0)
			continue;

		// 테스트 모드 고정 더미 - 판단하지 않는다. 그 자리에 서서 맞기만 한다 (밀려나면 밀린 자리에)
		if (m->aiFixed)
		{
			m->decision = BrainOutput();
			continue;
		}

		// 실명 (뇌섬 · 번-쩍) - 아무것도 보지 못해 그 자리에서 헤맨다: 쫓지도 때리지도 않는다 (2026-10-11 "뇌섬에 실명 효과가 적용 안 됨")
		if (nowUs < m->blindEndUs)
		{
			m->decision = BrainOutput();
			continue;
		}

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

	// 무리 - 순찰 · 공유 어그로 · 복귀 (RoomPacks.cpp)
	UpdatePacks(nowUs, lineClear);

	GMonsterStats.RecordAi(Utils::NowMicroseconds() - startUs);
}

void Room::StepMonsters(uint64 nowUs)
{
	if (_monsters.empty())
		return;

	const WallMap& walls = Walls();
	auto blocked = [&walls](float ax, float ay, float bx, float by, float r) { return walls.SweepBlocked(ax, ay, bx, by, r); };

	uint32 entries = 0;

	for (auto& [id, m] : _monsters)
	{
		if (m->diedAtUs != 0)
			continue;

		// 무리의 몸은 StepPacks 가 줄 순서로 걷게 한다
		if (m->packSlot != 0)
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

	// 무리 - 머리는 순찰 길, 뒤는 머리 발자국 (교전 · 복귀 때는 몸마다 목표)
	StepPacks(nowUs);
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

		// 야생의 체취 · 은신 - 맞아도 그 사람을 표적으로 삼지 않는다 (FoW · 갑옷 단검 1129, 2026-10-11)
		if (CreatureRef attacker = dynamic_pointer_cast<Creature>(findIt->second); attacker != nullptr
			&& (attacker->IsScented(nowUs) || attacker->IsStealthed(nowUs)))
			return;
	}

	// 무리의 몸 - 어그로는 무리 전체의 것 (2026-10-11 "모든 어그로는 몬스터 무리가 공유")
	if (monster->packSlot != 0)
	{
		OnPackMemberDamaged(monster, attackerId, ax, ay, nowUs);
		return;
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

	// 무리의 몸 - 머리였으면 다음 몸이 앞장선다, 전멸이면 무리째 리스폰 대기 (2026-10-11)
	if (monster->packSlot != 0)
		OnPackMemberDied(monster, nowUs);

	// 테스트 모드 더미 - 리스폰 · 드랍 없음 (바닥에 가방이 쌓이지 않게)
	if (monster->isDummy)
	{
		LOG_LINE << "[TEST] dummy died id=" << id << " type=" << type.nameKey << " by=" << instigatorId << '\n';
		return;
	}

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
			// 시체가 벽에 붙어 있으면 가방이 벽 안에 생긴다 - 가까운 빈 자리로. 못 찾으면 집(자리 검증이 벽 밖을 보장).
			// 무리의 몸은 "집" 이 순찰 길 시작점이라 수백 m 떨어질 수 있다 - 더 넓게 찾고, 그래도 없으면 시체 자리 그대로
			float bx = monster->posInfo->x();
			float by = monster->posInfo->y();
			const float search = (monster->packSlot != 0) ? MonsterConfig::DROP_FREE_SEARCH_CM * 3.f : MonsterConfig::DROP_FREE_SEARCH_CM;
			if (Walls().FindNearestFree(bx, by, MonsterConfig::DROP_FREE_RADIUS_CM, search, OUT bx, OUT by) == false)
			{
				if (monster->packSlot == 0)
				{
					bx = monster->spawn->x;
					by = monster->spawn->y;
				}
				else
				{
					bx = monster->posInfo->x();
					by = monster->posInfo->y();
				}
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
			// 나가는 사람의 S_DESPAWN 을 한 패킷으로 모아 한 번 방송한다 (escape-timer.md § Edge Cases "배치로 합칠 수 있다").
			// 30명이 따로 방송하면 30 × 방 30명 = 송신 약 900번 - 실측 완충 틱 16 ~ 46 ms 중 95 % 가 그 방송이었다 (2026-10-08).
			// 남은 사람이 받는 결과는 같다(클라가 id 마다 지운다). 같은 틱에 나가는 사람끼리는 서로의 S_DESPAWN 을 받지 않는다 -
			// 예전엔 id 가 큰 쪽만 작은 쪽 것을 받았다. 탈출한 클라는 던전을 떠날 때 남의 액터를 지우지 않는다(사망 · 탈출 공통, 별건)
			const uint64 completeBeginUs = Utils::NowMicroseconds();
			vector<uint64> leaving;

			for (uint64 id : occupants)
				BeginEscapeCommitInto(id, &leaving);

			const uint64 broadcastBeginUs = Utils::NowMicroseconds();
			if (leaving.empty() == false)
			{
				Protocol::S_DESPAWN despawnPkt;
				for (uint64 id : leaving)
					despawnPkt.add_object_ids(id);
				SEND_PACKET_DECLARATION(despawnPkt);
				Broadcast(sendBuffer);
			}

			const uint64 doneUs = Utils::NowMicroseconds();
			const int64 watchers = count_if(_objects.begin(), _objects.end(), [](const auto& kv) { return kv.second->IsPlayer(); });
			LOG_LINE << "[ESCAPE] completed tower=" << t.towerId << " n=" << leaving.size() << " watchers=" << watchers
				<< " us=" << (doneUs - completeBeginUs) << " despawn_broadcast_us=" << (doneUs - broadcastBeginUs) << '\n';

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

	JournalEntry journal;
	journal.kind = JournalKind::Expiry;
	journal.characterId = playerId;
	journal.expiry = snap;

	QueueCommit(move(journal),
		[snap](DBConnection* conn, CommitMode mode, const string& journalId) { return TryCommitExpiry(conn, snap, mode, journalId); },
		[session, snap](DBConnection* conn, bool committed, CommitMode)
		{
			GDeathCommits.End(snap.characterId);

			LOG_LINE << "[EXPIRE] " << (committed ? "committed" : "abandoned") << " - char=" << snap.characterId << '\n';

			// 끊긴 몸이면 그 세션으로는 보낼 곳이 없다 - 그사이 다시 로그인해 베이스캠프에 있으면 그쪽으로 결과를 (리뷰 ③)
			if (session == nullptr)
			{
				ResendBaseCampIfRelogged(conn, nullptr, snap.characterId);
				return;
			}

			session->place.store(SessionPlace::BaseCamp);

			// 포기면 아무것도 지워지지 않았다 - 결과 배너 없이(RUN_END_NONE), 동기화가 실제 상태를 보여 준다
			Protocol::S_LEAVE_GAME leavePkt;
			leavePkt.set_reason(committed ? Protocol::RUN_END_EXPIRED : Protocol::RUN_END_NONE);
			leavePkt.set_gold_kept(committed ? snap.goldKept : 0);
			leavePkt.set_items_destroyed(committed ? snap.itemsDestroyed : 0);
			SEND_PACKET_DECLARATION(leavePkt);
			session->Send(sendBuffer);

			SendBaseCampInventory(conn, session, snap.characterId);
			ResendBaseCampIfRelogged(conn, session, snap.characterId);
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
