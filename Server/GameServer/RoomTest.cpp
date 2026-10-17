#include "pch.h"
#include "Room.h"
#include "Player.h"
#include "Monster.h"
#include "MonsterTable.h"
#include "ItemTable.h"
#include "SkillTable.h"
#include "ObjectUtils.h"
#include "GameSession.h"
#include "Vision.h"
#include <cmath>

// 테스트 모드 (FBTK_TEST_MODE=1, 2026-10-11 사용자 지시 "더미 적을 세우거나 아이템과 스킬을 마음대로 갈아끼워서 테스트")
// 결정 · 범위: design/quick-specs/test-mode-2026-10-11.md. 클라는 F7 패널(US1TestModeWidget)이 /t 채팅 명령을 보낸다.
//
// 🔴 테스트 장비는 DB 에 남지 않는다 - 바꾼 아이템은 메모리만 바뀌고(탈출 · 사망 커밋은 소유 · 상태만 쓴다),
//    빈 칸에 만든 아이템(isTest)은 커밋에서 빠진다. 다음 접속은 DB 의 원래 장비로 돌아온다.

namespace
{
	constexpr int32		DUMMY_MAX_PER_CALL	= 10;
	constexpr float		DUMMY_RING_CM		= 160.f;	// 여러 마리면 지점 둘레에 이만큼 떨어뜨려 세운다
	constexpr float		DUMMY_FREE_SEARCH_CM = 300.f;
	constexpr float		DUMMY_DEFAULT_AHEAD_CM = 400.f;	// 지점을 안 주면 바라보는 방향 앞
	constexpr uint32	TEST_LEVEL_MAX		= 20;

	// 그 아이템 종류의 풀에 스킬이 있는가. 0 = 스킬 없음 (언제나 된다)
	bool InPool(const ItemTypeDef& type, uint32 skillId)
	{
		if (skillId == 0)
			return true;
		const vector<SkillPoolEntry>* pool = ItemTable::FindPool(type.skillPoolId);
		if (pool == nullptr)
			return false;
		return any_of(pool->begin(), pool->end(), [skillId](const SkillPoolEntry& e) { return e.skillId == skillId; });
	}
}

void Room::TestGear(uint64 playerId, TestGearRequest req)
{
	JOB_TAG();
	auto it = _objects.find(playerId);
	PlayerRef player = (it != _objects.end()) ? dynamic_pointer_cast<Player>(it->second) : nullptr;
	if (player == nullptr || player->IsAlive() == false)
		return;

	// ── 검사 먼저 - 하나라도 틀리면 아무것도 바꾸지 않는다 ──
	const ItemTypeDef* weapon = nullptr;
	if (req.weaponType != 0)
	{
		weapon = ItemTable::Find(req.weaponType);
		if (weapon == nullptr || weapon->equipSlot != Protocol::SLOT_WEAPON_PRIMARY
			|| InPool(*weapon, req.weaponSkill1) == false || InPool(*weapon, req.weaponSkill2) == false
			|| (req.weaponSkill1 != 0 && req.weaponSkill1 == req.weaponSkill2))
		{
			LOG_LINE << "[TEST] gear rejected - weapon type=" << req.weaponType << " skills=" << req.weaponSkill1 << "," << req.weaponSkill2
				<< " (not a weapon, a skill outside its pool, or the same skill twice) char=" << playerId << '\n';
			return;
		}
	}

	const ItemTypeDef* parts[4] = {};
	for (int32 k = 0; k < 4; k++)
	{
		if (req.types[k] == 0)
			continue;

		parts[k] = ItemTable::Find(req.types[k]);
		const Protocol::EquipSlot slot = static_cast<Protocol::EquipSlot>(Protocol::SLOT_HELMET + k);
		if (parts[k] == nullptr || parts[k]->equipSlot != slot || InPool(*parts[k], req.skills[k]) == false)
		{
			LOG_LINE << "[TEST] gear rejected - slot " << static_cast<int32>(slot) << " type=" << req.types[k] << " skill=" << req.skills[k]
				<< " (wrong slot or a skill outside its pool) char=" << playerId << '\n';
			return;
		}
	}

	const Protocol::ItemGrade grade = static_cast<Protocol::ItemGrade>(clamp(req.grade, 0, static_cast<int32>(Protocol::ITEM_GRADE_LEGENDARY)));
	const uint32 level = min(req.level, TEST_LEVEL_MAX);

	// ── 적용 - 칸에 아이템이 있으면 그것을 바꾸고, 없으면 테스트 아이템을 만든다 ──
	auto apply = [&](int32 index, uint32 typeId, uint32 skill1, uint32 skill2)
		{
			ItemEntry& e = player->equipped[index];
			if (e.instanceId == 0)
			{
				e = ItemEntry{};
				e.instanceId = GNextInstanceId.fetch_add(1);
				e.isNew = true;
				e.isTest = true;
				e.grade = Protocol::ITEM_GRADE_COMMON;
			}
			e.itemTypeId = typeId;
			e.skillIdPrimary = skill1;
			e.skillIdSecondary = skill2;
			if (grade != Protocol::ITEM_GRADE_NONE)
				e.grade = grade;
			e.level = level;
		};

	if (weapon != nullptr)
		apply(Protocol::SLOT_WEAPON_PRIMARY - 1, weapon->typeId, req.weaponSkill1, req.weaponSkill2);
	for (int32 k = 0; k < 4; k++)
	{
		if (parts[k] != nullptr)
			apply(Protocol::SLOT_HELMET - 1 + k, parts[k]->typeId, req.skills[k], 0);
	}

	// 스킬이 바뀌었으니 쿨다운 · 유지 · 재입력은 처음부터 (클라 패널도 자기 예측 쿨다운을 지운다)
	CancelCast(player);
	if (player->IsHolding())
		StopHold(player, Utils::NowMicroseconds(), "test");
	player->recastSkillId = 0;

	OnEquipmentChanged(player, "test");
	for (int32 i = 0; i < SKILL_SLOT_COUNT; i++)
		player->skillSlots[i].cooldownEndUs = 0;
	SendEquipSync(player);
	SendInventorySync(player);

	LOG_LINE << "[TEST] gear char=" << playerId << " weapon=" << req.weaponType << " [" << req.weaponSkill1 << "," << req.weaponSkill2 << "]"
		<< " helmet=" << req.types[0] << ":" << req.skills[0] << " armor=" << req.types[1] << ":" << req.skills[1]
		<< " boots=" << req.types[2] << ":" << req.skills[2] << " trinket=" << req.types[3] << ":" << req.skills[3]
		<< " grade=" << static_cast<int32>(grade) << " level=" << level << '\n';
}

void Room::TestSpawnDummies(uint64 playerId, uint32 monsterTypeId, int32 count, bool ai, float x, float y)
{
	JOB_TAG();
	auto it = _objects.find(playerId);
	PlayerRef player = (it != _objects.end()) ? dynamic_pointer_cast<Player>(it->second) : nullptr;
	if (player == nullptr || player->IsAlive() == false)
		return;

	const MonsterTypeDef* type = MonsterTable::FindType(monsterTypeId);
	if (type == nullptr)
	{
		LOG_LINE << "[TEST] spawn rejected - unknown monster type " << monsterTypeId << " char=" << playerId << '\n';
		return;
	}

	count = clamp(count, 1, DUMMY_MAX_PER_CALL);

	// 지점이 없거나 이상하면 바라보는 방향 앞
	const float px = player->posInfo->x();
	const float py = player->posInfo->y();
	if (std::isfinite(x) == false || std::isfinite(y) == false || (x == 0.f && y == 0.f))
	{
		const float yawRad = player->posInfo->yaw() * 3.14159265f / 180.f;
		x = px + ::cosf(yawRad) * DUMMY_DEFAULT_AHEAD_CM;
		y = py + ::sinf(yawRad) * DUMMY_DEFAULT_AHEAD_CM;
	}

	const uint64 nowUs = Utils::NowMicroseconds();
	int32 spawned = 0;

	for (int32 i = 0; i < count; i++)
	{
		float wantX = x;
		float wantY = y;
		if (count > 1)
		{
			const float a = 2.f * 3.14159265f * static_cast<float>(i) / static_cast<float>(count);
			wantX += ::cosf(a) * DUMMY_RING_CM;
			wantY += ::sinf(a) * DUMMY_RING_CM;
		}

		float freeX = wantX;
		float freeY = wantY;
		if (Walls().FindNearestFree(wantX, wantY, type->radiusCm, DUMMY_FREE_SEARCH_CM, OUT freeX, OUT freeY) == false)
			continue;

		// 집 = 세운 자리. 몬스터 AI 면 끌려가다 여기로 돌아온다 (leash)
		auto spawn = make_shared<MonsterSpawnDef>();
		spawn->floorId = _floorId;
		spawn->typeId = type->typeId;
		spawn->x = freeX;
		spawn->y = freeY;
		spawn->z = player->posInfo->z();
		spawn->yaw = ::atan2f(py - freeY, px - freeX) * 180.f / 3.14159265f;		// 나를 보고 선다

		const uint64 id = GNextMonsterId.fetch_add(1);
		MonsterRef monster = make_shared<Monster>(id, *type, *spawn);
		MonsterBrain::Reset(monster->brain, nowUs);
		monster->lastStepUs = nowUs;
		monster->isDummy = true;
		monster->aiFixed = (ai == false);
		monster->dummyOwnerId = playerId;

		if (AddObject(monster) == false)
			continue;

		_monsters[id] = monster;
		_dummySpawns[id] = spawn;
		_pendingSpawns.push_back(id);
		GMonsterStats.alive.fetch_add(1);
		spawned++;

		LOG_LINE << "[TEST] dummy spawn id=" << id << " type=" << type->nameKey << " ai=" << (ai ? 1 : 0)
			<< " at=(" << ::lroundf(freeX) << "," << ::lroundf(freeY) << ") owner=" << playerId << '\n';
	}

	if (spawned < count)
		LOG_LINE << "[TEST] spawn - " << (count - spawned) << " of " << count << " had no free spot near (" << ::lroundf(x) << "," << ::lroundf(y) << ")\n";
}

void Room::TestSetDummyMode(uint64 playerId, bool ai)
{
	JOB_TAG();
	const uint64 nowUs = Utils::NowMicroseconds();
	int32 changed = 0;

	for (auto& [id, m] : _monsters)
	{
		if (m->isDummy == false || m->dummyOwnerId != playerId || m->diedAtUs != 0)
			continue;

		m->aiFixed = (ai == false);

		// 어느 쪽이든 처음부터 - 고정으로 바뀌면 하던 공격 · 시전을 버리고, AI 로 바뀌면 Idle 에서 다시 감지한다
		MonsterBrain::Reset(m->brain, nowUs);
		m->decision = BrainOutput();
		CancelCast(m);
		if (m->attackHitAtUs != 0)
		{
			m->attackTargetId = 0;
			m->attackHitAtUs = 0;
			_attackers.erase(id);
			_pendingAttackCancels.push_back(id);
		}
		changed++;
	}

	LOG_LINE << "[TEST] dummy mode char=" << playerId << " ai=" << (ai ? 1 : 0) << " dummies=" << changed << '\n';
}

void Room::TestClearDummies(uint64 playerId)
{
	JOB_TAG();
	Protocol::S_DESPAWN despawnPkt;

	for (auto it = _monsters.begin(); it != _monsters.end(); )
	{
		const MonsterRef& m = it->second;
		if (m->isDummy == false || m->dummyOwnerId != playerId)
		{
			++it;
			continue;
		}

		const uint64 id = it->first;
		if (m->diedAtUs == 0)
			GMonsterStats.alive.fetch_sub(1);

		CancelCast(m);
		_attackers.erase(id);
		_statusTargets.erase(id);
		despawnPkt.add_object_ids(id);
		RemoveObject(id);
		it = _monsters.erase(it);
		_dummySpawns.erase(id);
	}

	if (despawnPkt.object_ids_size() > 0)
	{
		SEND_PACKET_BROADCAST(despawnPkt);
	}

	// 테스트 무리(/t pack)의 몸도 더미다 - 무리 자리를 비워 둔다 (다음 /t pack 이 다시 쓴다)
	DropTestPacks(playerId);

	LOG_LINE << "[TEST] dummies cleared char=" << playerId << " n=" << despawnPkt.object_ids_size() << '\n';
}

void Room::TestHeal(uint64 playerId)
{
	JOB_TAG();
	const uint64 nowUs = Utils::NowMicroseconds();

	// 나 - 체력 · 지속 피해 (CC 는 시간이 지나면 풀린다 - 클라 CC 표시와 어긋나지 않게 건드리지 않는다)
	auto it = _objects.find(playerId);
	PlayerRef player = (it != _objects.end()) ? dynamic_pointer_cast<Player>(it->second) : nullptr;
	if (player != nullptr && player->IsAlive())
	{
		if (player->status.RemoveDots("") > 0)
			_statusTargets.insert(playerId);
		Heal(player, playerId, player->maxHp, nowUs);
	}

	// 내 더미 - 만피 (몬스터 귀환과 같은 피해 0 · 남은 체력 패킷)
	int32 dummies = 0;
	for (auto& [id, m] : _monsters)
	{
		if (m->isDummy == false || m->dummyOwnerId != playerId || m->diedAtUs != 0)
			continue;

		m->hp = m->maxHp;
		Protocol::DamageInfo& heal = _pendingDamages.emplace_back();
		heal.set_attacker_id(0);
		heal.set_target_id(id);
		heal.set_damage(0);
		heal.set_remaining_hp(m->hp);
		dummies++;
	}

	LOG_LINE << "[TEST] heal char=" << playerId << " dummies=" << dummies << '\n';
}

void Room::TestResetCooldowns(uint64 playerId)
{
	JOB_TAG();
	auto it = _objects.find(playerId);
	PlayerRef player = (it != _objects.end()) ? dynamic_pointer_cast<Player>(it->second) : nullptr;
	if (player == nullptr)
		return;

	for (int32 i = 0; i < SKILL_SLOT_COUNT; i++)
		player->skillSlots[i].cooldownEndUs = 0;

	LOG_LINE << "[TEST] cooldowns reset char=" << playerId << '\n';
}

void Room::TestGod(uint64 playerId, bool on)
{
	JOB_TAG();
	auto it = _objects.find(playerId);
	PlayerRef player = (it != _objects.end()) ? dynamic_pointer_cast<Player>(it->second) : nullptr;
	if (player == nullptr)
		return;

	player->testGod = on;
	LOG_LINE << "[TEST] god char=" << playerId << " " << (on ? "ON - hp stops at 1" : "off") << '\n';
}

void Room::TestFow(uint64 playerId, bool fowOn)
{
	JOB_TAG();
	auto it = _objects.find(playerId);
	PlayerRef player = (it != _objects.end()) ? dynamic_pointer_cast<Player>(it->second) : nullptr;
	if (player == nullptr)
		return;

	// 다음 시야 갱신(30 Hz)이 알아서 맞춘다 - 끄면 전부 S_SPAWN, 켜면 시야 밖이 유예 뒤 S_DESPAWN. S_VISION 반경 0 이면 클라 안개도 꺼진다
	player->testSeeAll = (fowOn == false);
	LOG_LINE << "[TEST] fow char=" << playerId << (fowOn ? " ON" : " OFF - sees everything but stealth")
		<< (Vision::Enabled() ? "" : " (server FoW is off - FBTK_FOW=0, nothing to change)") << '\n';
}

void Room::DropOrphanDummies()
{
	// 주인이 층을 떠났다(탈출 · 사망 뒤 퇴장 · 다른 층) - 그 사람만 지울 수 있던 더미가 영원히 서 있지 않게
	vector<uint64> owners;
	for (const auto& [id, m] : _monsters)
	{
		if (m->isDummy && m->practice == false && _objects.find(m->dummyOwnerId) == _objects.end()
			&& find(owners.begin(), owners.end(), m->dummyOwnerId) == owners.end())
			owners.push_back(m->dummyOwnerId);
	}

	for (uint64 owner : owners)
	{
		LOG_LINE << "[TEST] owner " << owner << " left the floor - removing their dummies\n";
		TestClearDummies(owner);
	}
}
