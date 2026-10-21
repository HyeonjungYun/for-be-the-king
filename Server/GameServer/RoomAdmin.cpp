#include "pch.h"
#include "Room.h"
#include "Player.h"
#include "Monster.h"
#include "MonsterBrain.h"
#include "Bag.h"
#include "GameSession.h"
#include "PartyManager.h"
#include "CheatGuard.h"
#include "EventLog.h"

// 관리 포트의 방 스냅샷 (2026-10-21 · AdminServer · Tools/ServerMonitor/PROTOCOL.md).
// 🔴 전부 방 잡 안에서 (DoAsync) - _objects · 플레이어 필드를 그 방의 스레드에서만 읽는다. 관리 스레드는 결과 문자열만 받는다

namespace
{
	using EventFormat::JsonEscape;

	// 캐릭터 이름은 Player 가 아니라 세션의 캐릭터 목록에 있다
	string NameOf(const PlayerRef& player, uint64* accountOut = nullptr)
	{
		const uint64 id = player->objectInfo->object_id();
		if (GameSessionRef session = player->session.lock())
		{
			if (accountOut != nullptr)
				*accountOut = session->accountId.load();
			for (const Protocol::CharacterInfo& c : session->characters)
				if (c.object_info().object_id() == id)
					return c.name();
		}
		return string();
	}

	const char* KindName(RoomKind kind)
	{
		switch (kind)
		{
		case RoomKind::World: return "pvp";
		case RoomKind::PveSolo: return "pve_solo";
		case RoomKind::PveCoop: return "pve_coop";
		case RoomKind::BaseCamp: return "camp";
		}
		return "?";
	}

	string Num(float v) { return to_string(::lroundf(v)); }
}

string Room::AdminSummaryJson()
{
	int32 players = 0, monsters = 0, bags = 0;
	for (const auto& [id, object] : _objects)
	{
		if (object->IsPlayer()) players++;
		else if (object->IsMonster()) monsters++;
		else if (object->IsBag()) bags++;
	}
	return "{\"tag\":\"" + Tag() + "\",\"kind\":\"" + KindName(_kind) + "\",\"floor\":" + to_string(_floorId)
		+ ",\"players\":" + to_string(players) + ",\"monsters\":" + to_string(monsters) + ",\"bags\":" + to_string(bags) + "}";
}

string Room::AdminPlayersJson()
{
	string out = "[";
	bool first = true;
	for (const auto& [id, object] : _objects)
	{
		if (object->IsPlayer() == false)
			continue;
		const PlayerRef player = static_pointer_cast<Player>(object);
		uint64 account = 0;
		const string name = NameOf(player, &account);
		out += first ? "" : ",";
		first = false;
		out += "{\"id\":" + to_string(id) + ",\"name\":\"" + JsonEscape(name) + "\",\"acc\":" + to_string(account)
			+ ",\"room\":\"" + Tag() + "\",\"state\":\"" + (_kind == RoomKind::BaseCamp ? "camp" : "room") + "\""
			+ ",\"hp\":" + to_string(player->hp) + ",\"mhp\":" + to_string(player->maxHp)
			+ ",\"x\":" + Num(player->posInfo->x()) + ",\"y\":" + Num(player->posInfo->y())
			+ ",\"bot\":" + (name.rfind("bot_", 0) == 0 ? "true" : "false")
			+ ",\"party\":" + to_string(GParties.PartyOf(id)) + "}";
	}
	return out + "]";
}

string Room::AdminPlayerJson(uint64 characterId)
{
	auto it = _objects.find(characterId);
	if (it == _objects.end() || it->second->IsPlayer() == false)
		return string();
	const PlayerRef player = static_pointer_cast<Player>(it->second);
	const uint64 nowUs = Utils::NowMicroseconds();
	uint64 account = 0;
	const string name = NameOf(player, &account);

	string gear = "[";
	for (int32 i = 0; i < SKILL_SLOT_COUNT; i++)
	{
		const ItemEntry& e = player->equipped[i];
		if (e.itemTypeId == 0)
			continue;
		gear += (gear.size() > 1 ? "," : "");
		gear += "{\"slot\":" + to_string(i + 1) + ",\"type\":" + to_string(e.itemTypeId) + ",\"grade\":" + to_string(static_cast<int32>(e.grade))
			+ ",\"level\":" + to_string(e.level) + ",\"skill\":" + to_string(e.skillIdPrimary) + ",\"skill2\":" + to_string(e.skillIdSecondary)
			+ ",\"lone\":" + (e.lonePath ? "true" : "false") + "}";
	}
	gear += "]";

	string skills = "[";
	for (int32 i = 0; i < SKILL_SLOT_COUNT; i++)
	{
		const SkillSlot& s = player->skillSlots[i];
		if (s.skillId == 0)
			continue;
		skills += (skills.size() > 1 ? "," : "");
		const uint64 cdMs = (s.cooldownEndUs > nowUs) ? (s.cooldownEndUs - nowUs) / 1000 : 0;
		skills += "{\"slot\":" + to_string(i + 1) + ",\"skill\":" + to_string(s.skillId) + ",\"bind\":" + to_string(static_cast<int32>(s.bindState))
			+ ",\"cd_ms\":" + to_string(cdMs) + "}";
	}
	skills += "]";

	string inv = "[";
	for (const ItemEntry& e : player->inventory)
	{
		inv += (inv.size() > 1 ? "," : "");
		inv += "{\"type\":" + to_string(e.itemTypeId) + ",\"grade\":" + to_string(static_cast<int32>(e.grade)) + ",\"n\":" + to_string(e.stackCount) + "}";
	}
	inv += "]";

	const uint64 runLeftMs = (player->runDeadlineUs > nowUs) ? (player->runDeadlineUs - nowUs) / 1000 : 0;
	return "{\"id\":" + to_string(characterId) + ",\"name\":\"" + JsonEscape(name) + "\",\"acc\":" + to_string(account)
		+ ",\"room\":\"" + Tag() + "\",\"kind\":\"" + KindName(_kind) + "\""
		+ ",\"x\":" + Num(player->posInfo->x()) + ",\"y\":" + Num(player->posInfo->y()) + ",\"yaw\":" + Num(player->posInfo->yaw())
		+ ",\"hp\":" + to_string(player->hp) + ",\"mhp\":" + to_string(player->maxHp) + ",\"alive\":" + (player->IsAlive() ? "true" : "false")
		+ ",\"run_gold\":" + to_string(player->runGold) + ",\"run_left_ms\":" + to_string(runLeftMs)
		+ ",\"party\":" + to_string(GParties.PartyOf(characterId)) + ",\"team\":" + to_string(player->teamId)
		+ ",\"vision\":" + Num(player->visionRadiusCm)
		+ ",\"cheat\":" + to_string(player->cheat.Score(nowUs)) + ",\"cheat_top\":\"" + JsonEscape(player->cheat.Summary()) + "\""
		+ ",\"stunned\":" + (nowUs < player->stunEndUs ? "true" : "false")
		+ ",\"gear\":" + gear + ",\"skills\":" + skills + ",\"inv\":" + inv + "}";
}

string Room::AdminMapJson(bool withRoutes)
{
	const uint64 nowUs = Utils::NowMicroseconds();
	string out;
	out.reserve(8192);
	out += "{\"room\":\"" + Tag() + "\",\"kind\":\"" + KindName(_kind) + "\",\"floor\":" + to_string(_floorId) + ",\"objs\":[";
	bool first = true;
	auto sep = [&]() { out += first ? "" : ","; first = false; };

	for (const auto& [id, object] : _objects)
	{
		const float x = object->posInfo->x();
		const float y = object->posInfo->y();
		if (object->IsPlayer())
		{
			const PlayerRef p = static_pointer_cast<Player>(object);
			const string name = NameOf(p);
			sep();
			out += "{\"id\":" + to_string(id) + ",\"k\":\"p\",\"x\":" + Num(x) + ",\"y\":" + Num(y) + ",\"yaw\":" + Num(object->posInfo->yaw())
				+ ",\"hp\":" + to_string(p->hp) + ",\"mhp\":" + to_string(p->maxHp) + ",\"n\":\"" + JsonEscape(name) + "\""
				+ ",\"bot\":" + (name.rfind("bot_", 0) == 0 ? "1" : "0") + ",\"party\":" + to_string(GParties.PartyOf(id))
				+ ",\"vr\":" + Num(p->visionRadiusCm) + "}";
		}
		else if (object->IsMonster())
		{
			const MonsterRef m = static_pointer_cast<Monster>(object);
			if (m->IsAlive() == false)
				continue;
			uint32 packId = 0;
			if (m->packSlot != 0 && m->packSlot <= _packs.size())
				packId = _packs[m->packSlot - 1]->def.packId;
			sep();
			out += "{\"id\":" + to_string(id) + ",\"k\":\"m\",\"x\":" + Num(x) + ",\"y\":" + Num(y) + ",\"hp\":" + to_string(m->hp) + ",\"mhp\":" + to_string(m->maxHp)
				+ ",\"n\":\"" + JsonEscape(m->type != nullptr ? m->type->nameKey : string()) + "\",\"st\":\"" + MonsterStateName(m->brain.state) + "\""
				+ ",\"tgt\":" + to_string(m->brain.targetId) + ",\"pack\":" + to_string(packId) + "}";
		}
		else if (object->IsBag())
		{
			const BagRef b = static_pointer_cast<Bag>(object);
			sep();
			out += "{\"id\":" + to_string(id) + ",\"k\":\"bag\",\"x\":" + Num(x) + ",\"y\":" + Num(y) + ",\"chest\":" + (b->IsChest() ? "1" : "0")
				+ ",\"open\":" + (b->viewers.empty() ? "0" : "1") + ",\"looted\":" + (b->lootedAtUs != 0 ? "1" : "0")
				+ ",\"items\":" + to_string(b->items.size()) + ",\"owner\":" + to_string(b->ownerCharacterId) + "}";
		}
		else if (object->IsDecoy())
		{
			sep();
			out += "{\"id\":" + to_string(id) + ",\"k\":\"decoy\",\"x\":" + Num(x) + ",\"y\":" + Num(y) + "}";
		}
	}
	for (const Zone& z : _zones)
	{
		sep();
		out += "{\"id\":" + to_string(z.id) + ",\"k\":\"zone\",\"x\":" + Num(z.x) + ",\"y\":" + Num(z.y) + ",\"r\":" + Num(z.radiusCm)
			+ ",\"caster\":" + to_string(z.casterId) + ",\"skill\":" + to_string(z.skillId) + ",\"left_ms\":" + to_string(z.endUs > nowUs ? (z.endUs - nowUs) / 1000 : 0) + "}";
	}
	for (const Projectile& p : _projectiles)
	{
		sep();
		out += "{\"id\":" + to_string(p.id) + ",\"k\":\"proj\",\"x\":" + Num(p.x) + ",\"y\":" + Num(p.y) + ",\"dx\":" + to_string(p.dirX) + ",\"dy\":" + to_string(p.dirY)
			+ ",\"r\":" + Num(p.radiusCm) + ",\"caster\":" + to_string(p.casterId) + "}";
	}
	for (const EscapeTowerState& t : _towers)
	{
		sep();
		out += "{\"id\":" + to_string(t.wireId) + ",\"k\":\"tower\",\"x\":" + Num(t.x) + ",\"y\":" + Num(t.y) + ",\"n\":\"" + JsonEscape(t.towerId) + "\""
			+ ",\"gauge_ms\":" + to_string(t.gauge.progressUs / 1000) + ",\"filling\":" + (t.gauge.state == Protocol::ESCAPE_GAUGE_FILLING ? "1" : "0")
			+ ",\"occupants\":" + to_string(t.lastOccupants.size()) + "}";
	}
	out += "]";

	if (withRoutes)
	{
		out += ",\"routes\":[";
		bool firstRoute = true;
		for (const unique_ptr<PackSlot>& slot : _packs)
		{
			if (slot == nullptr || slot->route.points.empty())
				continue;
			out += firstRoute ? "" : ",";
			firstRoute = false;
			out += "{\"pack\":" + to_string(slot->def.packId) + ",\"pts\":[";
			for (size_t i = 0; i < slot->route.points.size(); i++)
				out += (i ? "," : "") + string("[") + Num(slot->route.points[i].first) + "," + Num(slot->route.points[i].second) + "]";
			out += "]}";
		}
		out += "]";
	}
	return out + "}";
}
