#include "pch.h"
#include "Room.h"
#include "Player.h"
#include "Monster.h"
#include "MonsterTable.h"
#include "BaseCampTable.h"
#include "GameSession.h"
#include "ServerStats.h"

// 베이스캠프 방 (2026-10-17 - design/quick-specs/basecamp-map-2026-10-17.md).
// 플레이어 한 명의 개인 방. 세션 place 는 BaseCamp 그대로라 창고 · 강화 · 착용 · 입장 · 파티는 예전 경로로 돈다.
// 이 방이 더하는 것: 걸어 다니는 몸 · 허수아비(맞으면 곧 만피) · 캠프 벽. 런이 아니라 죽지 않고 저장하지 않는다

namespace
{
	constexpr uint64 PRACTICE_HEAL_DELAY_US = 300'000;		// 마지막으로 맞고 이만큼 지나면 만피 - "피가 달아도 바로바로"
}

void Room::SpawnPracticeDummy()
{
	if (BaseCampTable::Loaded() == false || BaseCampTable::DummyTypeId() == 0)
		return;

	const MonsterTypeDef* type = MonsterTable::FindType(BaseCampTable::DummyTypeId());
	if (type == nullptr)
	{
		LOG_LINE << "[CAMP] no dummy - unknown monster type " << BaseCampTable::DummyTypeId() << '\n';
		return;
	}

	const BaseCampPoint& at = BaseCampTable::Dummy();
	auto spawn = make_shared<MonsterSpawnDef>();
	spawn->floorId = _floorId;
	spawn->typeId = type->typeId;
	spawn->x = at.x;
	spawn->y = at.y;
	spawn->z = 100.f;
	spawn->yaw = at.yaw;

	const uint64 nowUs = Utils::NowMicroseconds();
	const uint64 id = GNextMonsterId.fetch_add(1);
	MonsterRef monster = make_shared<Monster>(id, *type, *spawn);
	MonsterBrain::Reset(monster->brain, nowUs);
	monster->lastStepUs = nowUs;
	monster->isDummy = true;		// 리스폰 · 드랍 없음
	monster->aiFixed = true;		// 판단하지 않는다 - 서서 맞기만
	monster->practice = true;
	monster->testGod = true;		// 체력은 1 에서 멈춘다 - 죽지 않는다

	if (AddObject(monster) == false)
		return;

	_monsters[id] = monster;
	_dummySpawns[id] = spawn;
	_pendingSpawns.push_back(id);
	GMonsterStats.alive.fetch_add(1);
}

void Room::HealPracticeDummy(const MonsterRef& m, uint64 nowUs)
{
	if (m->hp >= m->maxHp || nowUs < m->lastCombatUs + PRACTICE_HEAL_DELAY_US)
		return;

	LOG_LINE << "[CAMP] dummy healed " << Tag() << " id=" << m->objectInfo->object_id() << " " << m->hp << " -> " << m->maxHp << '\n';
	m->hp = m->maxHp;

	// 몬스터 귀환 · /t heal 과 같은 방식 - 피해 0 · 남은 체력 (클라는 체력 바만 채운다)
	Protocol::DamageInfo& heal = _pendingDamages.emplace_back();
	heal.set_attacker_id(0);
	heal.set_target_id(m->objectInfo->object_id());
	heal.set_damage(0);
	heal.set_remaining_hp(m->hp);
}

void Room::DropBaseCampBody(PlayerRef player)
{
	JOB_TAG();
	if (player == nullptr)
		return;

	const uint64 id = player->objectInfo->object_id();
	if (_objects.find(id) != _objects.end())
	{
		CancelCast(player);
		RemoveObject(id);		// 패킷 없이 - 이 방엔 나밖에 없고, 클라는 다음 S_ENTER_GAME 으로 화면을 바꾼다
	}

	if (GameSessionRef session = player->session.lock())
	{
		PlayerRef current = player;
		session->player.compare_exchange_strong(current, nullptr);
	}

	LOG_LINE << "[CAMP] body dropped " << Tag() << " char=" << id << '\n';
}

void Room::BaseCampApplyItems(PlayerRef player, vector<Protocol::ItemInstance> changed)
{
	JOB_TAG();
	if (player == nullptr || _objects.find(player->objectInfo->object_id()) == _objects.end())
		return;

	for (const Protocol::ItemInstance& c : changed)
	{
		// 지금 있는 자리에서 뺀다 - 착용 칸 · 소지
		for (ItemEntry& worn : player->equipped)
		{
			if (worn.instanceId == c.instance_id())
				worn = ItemEntry();
		}
		erase_if(player->inventory, [&](const ItemEntry& e) { return e.instanceId == c.instance_id(); });

		ItemEntry e;
		e.instanceId = c.instance_id();
		e.itemTypeId = c.item_type_id();
		e.grade = c.grade();
		e.level = c.level();
		e.skillIdPrimary = c.skills_size() > 0 ? c.skills(0).skill_id() : 0;
		e.skillIdSecondary = c.skills_size() > 1 ? c.skills(1).skill_id() : 0;
		e.lonePath = c.lone_path();

		// 새 자리 - 착용 · 소지만 몸이 든다 (창고 · 파기는 몸에서 사라진다)
		if (c.state() == Protocol::ITEM_STATE_EQUIPPED && c.slot() >= Protocol::SLOT_WEAPON_PRIMARY && c.slot() <= Protocol::SLOT_TRINKET)
			player->equipped[c.slot() - 1] = e;
		else if (c.state() == Protocol::ITEM_STATE_CARRIED)
			player->inventory.push_back(e);
	}

	// 무기 자세 - 스폰 스냅샷 값도 맞춘다 (OnEquipmentChanged 의 S_WEAPON_CHANGE 가 이 값을 보낸다)
	const ItemEntry& weapon = player->equipped[Protocol::SLOT_WEAPON_PRIMARY - 1];
	player->objectInfo->set_weapon_type_id(weapon.instanceId != 0 ? weapon.itemTypeId : 0);

	OnEquipmentChanged(player, "camp");
	SendEquipSync(player);
}
