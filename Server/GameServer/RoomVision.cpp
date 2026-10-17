#include "pch.h"
#include "Room.h"
#include "Player.h"
#include "Monster.h"
#include "Decoy.h"
#include "SkillTable.h"
#include "WallTable.h"
#include "Vision.h"
#include "ServerStats.h"
#include "GameSession.h"

// FoW (전장의 안개, design/gdd/fog-of-war.md · 2026-10-11 사용자 지시) - 서버 권위 시야.
//
// 플레이어마다 "이 클라에 스폰돼 있는 생물" 집합(Player::fowKnown)을 든다. FlushMoves 앞(30 Hz)에서 다시 판정해
//   보이게 됐다 → 그 사람에게만 S_SPAWN + 그 생물의 CC · 상태 스냅샷
//   안 보인 지 grace 가 지났다 (은신은 바로) → 그 사람에게만 S_DESPAWN
// 이동 · 전투 이벤트는 SendVisible 이 이 집합으로 거른다. 집합에 없는 생물의 위치 · 사건은 그 클라에 가지 않는다 = 월핵이 볼 것이 없다.

// FBTK_FOW_LOG=<character id> - 그 사람이 받은 FoW 스폰 · 디스폰을 한 줄씩 남긴다 (실기동 증거용, 0 = 끔)
atomic<uint64> GFowLogViewer = 0;

namespace
{
	bool IsFowSubject(const ObjectRef& object)
	{
		return object->IsPlayer() || object->IsMonster() || object->IsDecoy();
	}

	// 테스트 모드 FoW 끔의 이벤트 필터 반경 - 층(280 m)보다 크면 된다
	constexpr float TEST_SEE_ALL_RADIUS_CM = 1'000'000.f;

	// S_VISION 이 바뀌었는지만 보는 요약 (Player.h VisionSig). 따라가는 시야원은 위치를 넣지 않는다 - 클라가 그 생물을 따라 그린다
	void MakeVisionSig(float radiusCm, float cameraScale, const vector<VisionExtra>& extras, OUT VisionSig& sig)
	{
		sig.valid = true;
		sig.radius = ::lroundf(radiusCm);
		sig.cameraPct = ::lroundf(cameraScale * 100.f);
		sig.extras.clear();
		for (const VisionExtra& e : extras)
		{
			VisionSig::Extra& x = sig.extras.emplace_back();
			x.sourceId = e.sourceId;
			x.followId = e.followId;
			x.radius = ::lroundf(e.radiusCm);
			x.x = (e.followId == 0) ? ::lroundf(e.x) : 0;
			x.y = (e.followId == 0) ? ::lroundf(e.y) : 0;
			x.needsLos = e.needsLos;
			x.seesStealth = e.seesStealth;
		}
	}
}

void Room::SendVisible(int32 fullCount, const function<int32(const PlayerRef&)>& count,
	const function<SendBufferRef()>& makeFull, const function<SendBufferRef(const PlayerRef&)>& makeMine)
{
	if (fullCount <= 0)
		return;

	SendBufferRef shared;
	uint64 sharedSends = 0;
	uint64 customSends = 0;
	const bool fow = Vision::Enabled();

	for (auto& item : _objects)
	{
		if (item.second->IsPlayer() == false)
			continue;

		PlayerRef viewer = static_pointer_cast<Player>(item.second);
		GameSessionRef session = viewer->session.lock();
		if (session == nullptr)
			continue;

		// FoW 를 끄면 거르지 않는다 - 예전 Broadcast 와 같다
		const int32 n = fow ? count(viewer) : fullCount;
		if (n <= 0)
			continue;

		if (n == fullCount)
		{
			if (shared == nullptr)
				shared = makeFull();
			session->Send(shared);
			sharedSends++;
		}
		else
		{
			session->Send(makeMine(viewer));
			customSends++;
		}
	}

	if (fow)
		GStats.AddFowSends(sharedSends, customSends);
}

bool Room::CanSee(const PlayerRef& viewer, uint64 objectId) const
{
	if (objectId == 0)
		return false;
	if (Vision::Enabled() == false)
		return true;
	if (objectId == viewer->objectInfo->object_id())
		return true;
	if (viewer->fowKnown.find(objectId) != viewer->fowKnown.end())
		return true;

	// 모르는 id - 생물이 아니면(가방 · 상자) 숨기지 않는다. 이미 방을 떠난 것은 알려 주지 않는다
	auto it = _objects.find(objectId);
	if (it == _objects.end())
		return false;
	return IsFowSubject(it->second) == false;
}

bool Room::NearViewer(const PlayerRef& viewer, float x, float y, float extraCm) const
{
	const float r = (viewer->visionRadiusCm > 0.f ? viewer->visionRadiusCm : Vision::Rules().radiusCm) + extraCm;
	const float dx = x - viewer->posInfo->x();
	const float dy = y - viewer->posInfo->y();
	return dx * dx + dy * dy <= r * r;
}

float Room::VisionBonusOf(const CreatureRef& creature) const
{
	float bonus = 0.f;
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
			if (e.type == SkillEffectType::Vision)
				bonus += e.magnitude;
		}
	}
	return bonus;
}

float Room::VisionRadiusOf(const PlayerRef& player, uint64 nowUs) const
{
	const VisionRules& rules = Vision::Rules();
	float radius = rules.radiusCm * (1.f + VisionBonusOf(player));

	// 실명 - 보이는 반경 자체가 줄어든다 (사용자 결정 2026-10-11 "FoW 가 오면 주변 전부 암흑 시야"). 클라는 암흑 오버레이로 같은 반경을 그린다
	if (nowUs < player->blindEndUs)
		radius *= (player->blindScale > 0.f && player->blindScale <= 1.f) ? player->blindScale : SkillTable::Rules().blindViewScale;

	return (std::max)(radius, rules.minRadiusCm);
}

void Room::UpdateVisibility(uint64 nowUs)
{
	if (Vision::Enabled() == false)
		return;

	const uint64 beginUs = Utils::NowMicroseconds();
	const WallMap& walls = Walls();
	const uint64 graceUs = static_cast<uint64>(Vision::Rules().graceMs) * 1000;

	_fowSubjects.clear();
	for (auto& item : _objects)
	{
		if (IsFowSubject(item.second))
			_fowSubjects.push_back(static_pointer_cast<Creature>(item.second));
	}

	uint64 pairs = 0;
	uint64 losChecks = 0;
	uint64 spawned = 0;
	uint64 despawned = 0;
	vector<CreatureRef> fresh;

	for (auto& item : _objects)
	{
		if (item.second->IsPlayer() == false)
			continue;

		PlayerRef viewer = static_pointer_cast<Player>(item.second);
		GameSessionRef session = viewer->session.lock();
		if (session == nullptr)
		{
			// 끊긴 몸 - 받을 클라가 없다. 다시 붙으면 처음부터 받는다 (HandleReattach → ResetVision)
			viewer->fowKnown.clear();
			continue;
		}

		const uint64 viewerId = item.first;

		// ── 시야원 - 내 자리 + 추가 시야원(분신 · 종이매 · 탐지). 끝난 것 · 따라가던 생물이 사라진 것은 뺀다 ──
		const float cameraScale = 1.f + VisionBonusOf(viewer);
		const float radius = VisionRadiusOf(viewer, nowUs);

		// 테스트 모드 FoW 끔 (/t fow 0) - 이벤트 필터(NearViewer · 투사체)도 전부 통과하게 반경을 층보다 크게
		viewer->visionRadiusCm = viewer->testSeeAll ? TEST_SEE_ALL_RADIUS_CM : radius;

		_fowSources.clear();
		{
			Vision::Source own;
			own.x = viewer->posInfo->x();
			own.y = viewer->posInfo->y();
			own.radiusCm = radius;
			own.needsLos = true;
			own.seesStealth = false;
			_fowSources.push_back(own);
		}

		// PVE 협동 - 아군의 시야를 같이 본다 (ADR-0014 D8). 아군을 따라가는 추가 시야원이라 클라 안개도 그 둘레를 연다.
		// 아군이 방을 떠나면 아래 루프가 따라갈 생물을 못 찾아 지운다
		if (viewer->teamId != 0)		// PVE 협동 + PvP 층 던전 파티 (party-betrayal R3) - 팀이 있으면 아군 시야를 같이 본다
		{
			for (auto& other : _objects)
			{
				if (other.first == viewerId || other.second->IsPlayer() == false)
					continue;
				PlayerRef ally = static_pointer_cast<Player>(other.second);
				if (SameTeam(*viewer, *ally) == false || ally->IsAlive() == false)
					continue;

				VisionExtra shared;
				shared.sourceId = ally->objectInfo->object_id();
				shared.followId = shared.sourceId;
				shared.radiusCm = VisionRadiusOf(ally, nowUs);
				shared.needsLos = true;
				shared.kind = "ally";
				AddVisionExtra(viewer, shared);
			}
		}

		for (size_t i = 0; i < viewer->visionExtras.size(); )
		{
			const VisionExtra& e = viewer->visionExtras[i];
			bool alive = (e.untilUs == 0 || nowUs < e.untilUs);
			float x = e.x;
			float y = e.y;

			if (alive && e.followId != 0)
			{
				auto followIt = _objects.find(e.followId);
				if (followIt == _objects.end())
					alive = false;
				else
				{
					x = followIt->second->posInfo->x();
					y = followIt->second->posInfo->y();
				}
			}

			if (alive == false)
			{
				LOG_LINE << "[FOW] vision source gone char=" << viewerId << " source=" << e.sourceId << '\n';
				viewer->visionExtras.erase(viewer->visionExtras.begin() + i);
				continue;
			}

			Vision::Source src;
			src.x = x;
			src.y = y;
			src.radiusCm = e.radiusCm;
			src.needsLos = e.needsLos;
			src.seesStealth = e.seesStealth;
			_fowSources.push_back(src);
			i++;
		}

		for (auto it = viewer->fowRevealed.begin(); it != viewer->fowRevealed.end(); )
		{
			if (nowUs >= it->second)
				it = viewer->fowRevealed.erase(it);
			else
				++it;
		}

		// 반경 0 = 클라가 안개를 그리지 않는다 (US1VisionSubsystem::IsActive) - FoW 끔
		SendVisionIfChanged(viewer, viewer->testSeeAll ? 0.f : radius, cameraScale, nowUs);

		// ── 생물마다 ──
		Protocol::S_SPAWN spawnPkt;
		Protocol::S_DESPAWN despawnPkt;
		fresh.clear();

		for (const CreatureRef& subject : _fowSubjects)
		{
			const uint64 subjectId = subject->objectInfo->object_id();
			if (subjectId == viewerId)
				continue;

			pairs++;
			const bool stealthed = subject->IsStealthed(nowUs);
			bool seen = false;

			if (subject->IsDecoy() && static_pointer_cast<Decoy>(subject)->ownerId == viewerId)
			{
				seen = true;		// 내 미끼 · 분신은 늘 안다
			}
			else if (SameTeam(*viewer, *subject))
			{
				seen = true;		// PVE 협동 아군 (과 아군의 미끼 · 분신) - 은신이어도 늘 안다 (ADR-0014)
			}
			else if (viewer->fowRevealed.find(subjectId) != viewer->fowRevealed.end())
			{
				seen = true;		// 외로운 외침 · 종이매 - 시야와 상관없이
			}
			else if (viewer->testSeeAll && stealthed == false)
			{
				seen = true;		// 테스트 모드 FoW 끔 - 은신만 여전히 탐지가 있어야 보인다 (아래 시야원 판정)
			}
			else
			{
				const float sx = subject->posInfo->x();
				const float sy = subject->posInfo->y();
				for (const Vision::Source& src : _fowSources)
				{
					if (stealthed && src.seesStealth == false)
						continue;

					const float dx = sx - src.x;
					const float dy = sy - src.y;
					if (dx * dx + dy * dy > src.radiusCm * src.radiusCm)
						continue;

					if (src.needsLos)
					{
						losChecks++;
						if (walls.SightBlocked(src.x, src.y, sx, sy))
							continue;
					}

					seen = true;
					break;
				}
			}

			auto known = viewer->fowKnown.find(subjectId);
			if (seen)
			{
				if (known == viewer->fowKnown.end())
				{
					viewer->fowKnown.emplace(subjectId, nowUs);
					subject->objectInfo->set_hp(subject->hp);
					subject->objectInfo->set_max_hp(subject->maxHp);
					spawnPkt.add_players()->CopyFrom(*subject->objectInfo);
					fresh.push_back(subject);
				}
				else
				{
					known->second = nowUs;
				}
			}
			else if (known != viewer->fowKnown.end() && (stealthed || nowUs - known->second >= graceUs))
			{
				// 은신은 유예 없이 바로 사라진다 - 유예 동안 보이면 은신이 아니다
				viewer->fowKnown.erase(known);
				despawnPkt.add_object_ids(subjectId);
			}
		}

		if (viewerId == GFowLogViewer.load() && (despawnPkt.object_ids_size() > 0 || spawnPkt.players_size() > 0))
		{
			LogLine out;
			out << "[FOW] viewer=" << viewerId << " at=(" << ::lroundf(viewer->posInfo->x()) << "," << ::lroundf(viewer->posInfo->y()) << ") r=" << ::lroundf(radius);
			for (const Protocol::ObjectInfo& info : spawnPkt.players())
				out << " +" << info.object_id() << "(" << ::lroundf(info.pos_info().x()) << "," << ::lroundf(info.pos_info().y()) << ")";
			for (uint64 id : despawnPkt.object_ids())
				out << " -" << id;
			out << '\n';
		}

		if (despawnPkt.object_ids_size() > 0)
		{
			despawned += despawnPkt.object_ids_size();
			SEND_PACKET(despawnPkt);
		}

		if (spawnPkt.players_size() > 0)
		{
			spawned += spawnPkt.players_size();
			{
				SEND_PACKET(spawnPkt);
			}
			SendSnapshots(viewer, fresh, nowUs);
		}
	}

	GStats.AddFowWork(pairs, losChecks);
	GStats.AddFowChurn(spawned, despawned);
	GStats.AddFowUpdateUs(Utils::NowMicroseconds() - beginUs);
}

void Room::SendVisionIfChanged(const PlayerRef& viewer, float radiusCm, float cameraScale, uint64 nowUs)
{
	// 스레드마다 하나를 다시 쓴다 - 대부분 그대로라 할당 없이 비교만 하고 끝난다
	thread_local VisionSig sig;
	MakeVisionSig(radiusCm, cameraScale, viewer->visionExtras, OUT sig);
	if (sig == viewer->visionSent)
		return;
	viewer->visionSent = sig;

	GameSessionRef session = viewer->session.lock();
	if (session == nullptr)
		return;

	Protocol::S_VISION pkt;
	pkt.set_radius_cm(radiusCm);
	pkt.set_camera_scale(cameraScale);

	for (const VisionExtra& e : viewer->visionExtras)
	{
		Protocol::VisionSource* s = pkt.add_sources();
		s->set_source_id(e.sourceId);
		s->set_follow_id(e.followId);
		s->set_x(e.x);
		s->set_y(e.y);
		s->set_radius_cm(e.radiusCm);
		s->set_remaining_ms(e.untilUs > nowUs ? static_cast<uint32>((e.untilUs - nowUs) / 1000) : 0);
		s->set_needs_los(e.needsLos);
		s->set_sees_stealth(e.seesStealth);
		s->set_kind(e.kind);
	}

	LOG_LINE << "[FOW] vision char=" << viewer->objectInfo->object_id() << " radius=" << ::lroundf(radiusCm) << " camera=" << cameraScale
		<< " sources=" << pkt.sources_size() << '\n';

	SEND_PACKET(pkt);
}

void Room::AppendCcState(const CreatureRef& target, uint64 nowUs, OUT Protocol::S_CC_STATE& pkt) const
{
	Protocol::CcStateInfo* state = pkt.add_states();
	state->set_target_id(target->objectInfo->object_id());
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
	addSlot(target->blindEndUs,			Protocol::CC_TYPE_BLIND,			SkillTable::Rules().blindViewScale);
	addSlot(target->tauntEndUs,			Protocol::CC_TYPE_TAUNT,			0.f);

	uint64 slowEndUs = 0;
	for (const SlowSource& src : target->slowSources)
	{
		if (src.endUs > slowEndUs)
			slowEndUs = src.endUs;
	}

	addSlot(slowEndUs, Protocol::CC_TYPE_SLOW, target->activeSlow);
}

void Room::AppendStatusSnapshot(const CreatureRef& target, uint64 nowUs, OUT Protocol::S_STATUS& pkt) const
{
	const uint64 id = target->objectInfo->object_id();

	auto add = [&](Protocol::StatusKind kind, const string& param, uint32 stacks, uint64 endUs, float magnitude, uint64 instigatorId)
		{
			Protocol::StatusInfo* s = pkt.add_applied();
			s->set_target_id(id);
			s->set_kind(kind);
			s->set_param(param);
			s->set_stacks(stacks);
			s->set_remaining_ms(endUs > nowUs ? static_cast<uint32>((endUs - nowUs) / 1000) : 0);		// 0 = 끝이 없다 (속성)
			s->set_magnitude(magnitude);
			s->set_instigator_id(instigatorId);
		};

	// 지속 피해 - 종류마다 한 줄 (S_STATUS 와 같다: 중첩 수 · 가장 늦게 끝나는 것)
	map<string, const DotInstance*> latest;
	for (const DotInstance& d : target->status.Dots())
	{
		if (d.endUs <= nowUs)
			continue;
		const DotInstance*& slot = latest[d.kind];
		if (slot == nullptr || d.endUs > slot->endUs)
			slot = &d;
	}
	for (const auto& [kind, dot] : latest)
		add(Protocol::STATUS_DOT, kind, target->status.DotStacks(kind), dot->endUs, dot->baseDamage, dot->instigatorId);

	for (const BuffInstance& b : target->status.Buffs())
	{
		if (b.endUs <= nowUs)
			continue;
		const string label = b.param.empty() ? string(SkillTable::BuffStatName(b.stat)) : string(SkillTable::BuffStatName(b.stat)) + ":" + b.param;
		add(Protocol::STATUS_BUFF, label, 1, b.endUs, b.magnitude, id);
	}

	for (const MarkInstance& m : target->status.Marks())
	{
		if (m.endUs > nowUs)
			add(Protocol::STATUS_MARK, m.kind, 1, m.endUs, 0.f, m.instigatorId);
	}

	if (target->IsInvulnerable(nowUs))
		add(Protocol::STATUS_INVULN, target->IsDoomed(nowUs) ? "doom" : "invuln", 1, target->invulnUntilUs, 0.f, id);
	if (target->stance.empty() == false)
		add(Protocol::STATUS_STANCE, target->stance, 1, 0, target->stanceRatio, id);
	if (nowUs < target->petrifyEndUs)
		add(Protocol::STATUS_STANCE, "petrify", 1, target->petrifyEndUs, static_cast<float>(target->petrifyStunMs), id);
	if (target->IsCountering(nowUs))
		add(Protocol::STATUS_COUNTER, "counter", 1, target->counterUntilUs, target->counterRatio, id);
	if (target->IsParrying(nowUs))
		add(Protocol::STATUS_PARRY, "parry", 1, target->parryUntilUs, 0.f, id);
	if (target->IsReflecting(nowUs))
		add(Protocol::STATUS_PARRY, "reflect", 1, target->reflectUntilUs, 0.f, id);

	if (target->IsHolding())
	{
		string param = "hold";
		if (const SkillDef* def = SkillTable::Find(target->holdSkillId); def != nullptr && target->holdEffectIndex < def->effects.size()
			&& def->effects[target->holdEffectIndex].param.empty() == false)
			param = def->effects[target->holdEffectIndex].param;
		add(Protocol::STATUS_HOLD, param, 1, target->holdEndUs, 0.f, id);
	}
}

void Room::SendSnapshots(const PlayerRef& viewer, const vector<CreatureRef>& fresh, uint64 nowUs)
{
	GameSessionRef session = viewer->session.lock();
	if (session == nullptr || fresh.empty())
		return;

	Protocol::S_CC_STATE ccPkt;
	Protocol::S_STATUS statusPkt;

	for (const CreatureRef& c : fresh)
	{
		AppendCcState(c, nowUs, ccPkt);

		// CC 가 하나도 없으면 줄을 뺀다 - 막 보이게 된 생물마다 빈 줄을 보낼 이유가 없다
		const Protocol::CcStateInfo& last = ccPkt.states(ccPkt.states_size() - 1);
		if (last.slots_size() == 0 && last.active_slow() == 0.f)
			ccPkt.mutable_states()->RemoveLast();

		AppendStatusSnapshot(c, nowUs, statusPkt);
	}

	if (ccPkt.states_size() > 0)
	{
		SEND_PACKET(ccPkt);
	}

	if (statusPkt.applied_size() > 0)
	{
		SEND_PACKET(statusPkt);
	}
}

void Room::ForgetObject(uint64 objectId)
{
	for (auto& item : _objects)
	{
		if (item.second->IsPlayer() == false)
			continue;

		Player* player = static_cast<Player*>(item.second.get());
		player->fowKnown.erase(objectId);
		player->fowRevealed.erase(objectId);
		// 그 생물을 따라가던 시야원은 다음 갱신이 뺀다 (followId 를 못 찾는다)
	}
}

bool Room::CanTarget(const CreatureRef& attacker, const CreatureRef& target, uint64 nowUs) const
{
	if (target == nullptr)
		return false;
	if (attacker == target || Vision::Enabled() == false)
		return true;

	// 은신은 대상으로 지정할 수 없다 - 범위기 · 투사체에는 맞는다 (기본안)
	if (target->IsStealthed(nowUs))
		return false;

	// 이 클라가 모르는 생물(벽 너머 · 시야 밖)을 지정했다 - 정상 클라는 화면에 없는 것을 누를 수 없다
	if (attacker->IsPlayer())
		return CanSee(static_pointer_cast<Player>(attacker), target->objectInfo->object_id());

	return true;
}

void Room::StartStealth(const CreatureRef& creature, uint32 skillId, uint32 durationMs, bool breaksOnDamage, uint64 nowUs)
{
	const uint64 id = creature->objectInfo->object_id();
	creature->stealthUntilUs = nowUs + static_cast<uint64>(durationMs) * 1000;
	creature->stealthSkillId = skillId;
	creature->stealthBreaksOnDamage = breaksOnDamage;
	_windowOwners.insert(id);

	// 이 생물을 노리던 평타 · 대상 지정 시전은 놓친다 - 사라진 것을 계속 때리지 않게
	int32 dropped = 0;
	for (auto& item : _objects)
	{
		if (item.first == id)
			continue;

		CreatureRef other = AsCreature(item.second);
		if (other == nullptr)
			continue;

		if (other->attackTargetId == id && other->attackHitAtUs != 0)
		{
			other->attackTargetId = 0;
			other->attackHitAtUs = 0;
			_attackers.erase(item.first);
			_pendingAttackCancels.push_back(item.first);
			dropped++;
		}

		if (other->IsCasting() && other->castTargetId == id)
		{
			CancelCast(other);
			dropped++;
		}
	}

	PushStatus(true, id, Protocol::STATUS_STEALTH, "stealth", 1, durationMs, 0.f, id);
	LOG_LINE << "[STEALTH] start char=" << id << " skill=" << skillId << " for=" << durationMs << "ms breaks_on_damage=" << (breaksOnDamage ? 1 : 0)
		<< " dropped=" << dropped << '\n';
}

void Room::BreakStealth(const CreatureRef& creature, const char* why, uint64 nowUs)
{
	if (creature == nullptr || creature->stealthSkillId == 0 || creature->IsStealthed(nowUs) == false)
		return;

	const uint64 id = creature->objectInfo->object_id();
	LOG_LINE << "[STEALTH] end char=" << id << " skill=" << creature->stealthSkillId << " why=" << why << '\n';

	creature->stealthUntilUs = 0;
	creature->stealthSkillId = 0;
	creature->stealthBreaksOnDamage = false;
	PushStatus(false, id, Protocol::STATUS_STEALTH, "stealth", 0, 0, 0.f, id);
}

void Room::AddVisionExtra(const PlayerRef& owner, const VisionExtra& extra)
{
	for (VisionExtra& e : owner->visionExtras)
	{
		if (e.sourceId == extra.sourceId)
		{
			e = extra;
			return;
		}
	}
	owner->visionExtras.push_back(extra);
}

void Room::RevealTo(const PlayerRef& viewer, uint64 subjectId, uint64 untilUs)
{
	uint64& until = viewer->fowRevealed[subjectId];
	until = (std::max)(until, untilUs);
}

// 안 보이는 적을 맞혔다 - 그 적만 잠깐 보인다 (사용자 결정 2026-10-11 "공격이 적에게 맞으면 암흑 시야에서 적만 잠깐 보였으면").
// 바로 S_SPAWN 을 보내 아는 집합에 넣는다 - 이 Flush 끝의 S_DAMAGE 가 걸러지지 않고 숫자가 뜬다. 은신은 드러내지 않는다
void Room::RevealOnHit(const CreatureRef& attacker, const CreatureRef& target, uint64 nowUs)
{
	constexpr uint64 REVEAL_ON_HIT_US = 1'200'000;

	if (Vision::Enabled() == false || attacker == nullptr || target == nullptr || attacker == target)
		return;
	if (attacker->IsPlayer() == false || IsFowSubject(target) == false || target->IsStealthed(nowUs))
		return;

	PlayerRef viewer = static_pointer_cast<Player>(attacker);
	const uint64 targetId = target->objectInfo->object_id();
	RevealTo(viewer, targetId, nowUs + REVEAL_ON_HIT_US);

	if (viewer->fowKnown.find(targetId) != viewer->fowKnown.end())
		return;		// 이미 화면에 있다 - 공개 시간만 늘렸다

	GameSessionRef session = viewer->session.lock();
	if (session == nullptr)
		return;

	viewer->fowKnown.emplace(targetId, nowUs);
	target->objectInfo->set_hp(target->hp);
	target->objectInfo->set_max_hp(target->maxHp);

	Protocol::S_SPAWN spawnPkt;
	spawnPkt.add_players()->CopyFrom(*target->objectInfo);
	{
		SEND_PACKET(spawnPkt);
	}

	vector<CreatureRef> fresh{ target };
	SendSnapshots(viewer, fresh, nowUs);

	LOG_LINE << "[FOW] reveal on hit viewer=" << viewer->objectInfo->object_id() << " target=" << targetId << '\n';
}
