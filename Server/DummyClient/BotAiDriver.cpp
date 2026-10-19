#include "pch.h"
#include "BotAiDriver.h"
#include "BotPath.h"
#include "ClientPacketHandler.h"
#include <cfloat>
#include <fstream>
#include <sstream>

extern std::atomic<uint64> GSentThisWindow;		// DummyClient.cpp - 10 s 보고의 "send rate" (이동 패킷 수)

namespace BotAiDriver
{
	atomic<bool>	GAi = false;
	atomic<bool>	GAiTrace = false;
	atomic<uint64>	GAiDecisions = 0;
	atomic<uint64>	GAiBranchChanges = 0;
	atomic<uint64>	GAiAttacks = 0;
	atomic<uint64>	GAiSkills = 0;
	atomic<uint64>	GAiLootOpens = 0;
	atomic<uint64>	GAiBlockedSteps = 0;

	namespace
	{
		constexpr float		MOVE_SPEED = 340.f;
		constexpr float		WAYPOINT_REACHED_CM = 5.f;
		constexpr float		REPLAN_GOAL_DRIFT_CM = 200.f;	// 목표가 이만큼 옮겨지면 길을 다시 찾는다
		constexpr uint64	REPLAN_INTERVAL_US = 2'000'000;
		constexpr uint64	ATTACK_INTERVAL_US = 1'000'000;
		constexpr uint64	CAST_HOLD_EXTRA_US = 200'000;
		constexpr float		DASH_SPEED = 1200.f;
		constexpr float		DASH_DIST = 400.f;
		constexpr uint64	DASH_DURATION_US = static_cast<uint64>(DASH_DIST / DASH_SPEED * 1'000'000.0);

		BotAI::Brain*				GBrain = nullptr;
		BotAI::SkillBook			GSkills;
		vector<BotAI::Tower>		GTowers;		// 층 0
		vector<BotAI::Poi>			GPois;			// 층 0 - 상자 자리 · 집 가운데 · 타워 (탐색이 둘러볼 곳)
		BotAI::PersonaBook			GPersonas;		// bot_personas.csv - 없으면 기본 성격 (옛 두뇌)

		// item_types.csv 무기 - 평타 사거리 · 공속 · 원거리 (두뇌가 무기에 맞춰 거리를 잡는다, 2026-10-20)
		struct WeaponInfo { float rangeCm = 0.f; float attackSpeed = 1.f; bool ranged = false; };
		unordered_map<uint32, WeaponInfo>	GWeapons;

		// bot_loadouts.csv - 무기 + Shift · Q 스킬로 내 빌드를 알아본다 (서버가 입힌 무작위 빌드 → 성격)
		struct LoadoutRow { string code; string role; uint32 weapon = 0; uint32 shift = 0; uint32 q = 0; };
		vector<LoadoutRow>			GLoadouts;

		// skill_recovery.csv - 스킬을 쓴 뒤 다음 스킬까지 서버가 막는 시간 (동작 클립). 안 지키면 reason=recovery 로 거절된다
		unordered_map<uint32, uint32>	GRecoveryMs;
		constexpr uint64	RECOVERY_SLACK_US = 60'000;		// 서버 · 봇 시계 차
		constexpr uint64	BASIC_ONLY_SLACK_US = 400'000;	// 기본 공격만 버프는 서버가 시전을 처리한 때부터 센다 - 끝 무렵 누른 스킬이 basic_only 로 거절됐다 (실기동 11 건)

		constexpr uint64	PERSONA_WAIT_US = 1'500'000;		// 입장 뒤 스킬 칸이 이만큼 안 오면 무기만으로 정한다
		constexpr uint64	ATTACK_JITTER_US = 120'000;			// 평타 간격 + 0 ~ 이만큼 (사람 손)
		constexpr float		DASH_STOP_SHORT_CM = 100.f;			// 대상 돌진은 대상 앞 이만큼에서 멈춘다 (서버 ARRIVE_STOP_CM 90 보다 조금 앞)
		constexpr float		DASH_DIST_SAFETY = 0.9f;				// 표 거리 × - 창 끝을 넘지 않게
		constexpr uint64	DASH_START_DELAY_US = 70'000;		// 시전 끝 + 이만큼 뒤에 몸을 날린다 - 서버 속도 창이 열리는 때 (패킷 · 방 큐 지연)
		constexpr uint64	RECAST_MIN_US = 350'000;			// 재입력 스킬의 둘째 누름 - 시전 끝 + 이만큼 ~
		constexpr uint64	RECAST_MAX_US = 900'000;

		// 머리 줄 + 쉼표 줄 → 열 이름으로 찾는 행들 (따옴표 없음)
		vector<unordered_map<string, string>> ReadRows(const string& text)
		{
			vector<unordered_map<string, string>> rows;
			stringstream ss(text);
			string line;
			vector<string> header;
			while (getline(ss, line))
			{
				if (line.empty() == false && line.back() == '\r') line.pop_back();
				if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) line = line.substr(3);
				if (line.empty()) continue;
				vector<string> cells;
				string cur;
				for (char ch : line) { if (ch == ',') { cells.push_back(cur); cur.clear(); } else cur.push_back(ch); }
				cells.push_back(cur);
				if (header.empty()) { header = cells; continue; }
				unordered_map<string, string> row;
				for (size_t i = 0; i < header.size() && i < cells.size(); i++)
					row[header[i]] = cells[i];
				rows.push_back(move(row));
			}
			return rows;
		}

		float Num(const unordered_map<string, string>& row, const char* key)
		{
			auto it = row.find(key);
			return (it == row.end()) ? 0.f : static_cast<float>(::atof(it->second.c_str()));
		}

		// 서버 ItemTable::DefaultDataDir 와 같은 규칙 (BotPath 도 같다)
		string DataDir()
		{
			char* raw = nullptr;
			size_t length = 0;
			if (_dupenv_s(&raw, &length, "FBTK_DATA_DIR") == 0 && raw != nullptr)
			{
				const string dir = raw;
				::free(raw);
				return dir;
			}
			char exePath[MAX_PATH] = {};
			::GetModuleFileNameA(nullptr, exePath, MAX_PATH);
			string dir = exePath;
			dir.erase(dir.find_last_of("\\/") + 1);
			return dir + "..\\..\\Data";
		}

		bool ReadFile(const string& path, OUT string& text)
		{
			ifstream file(path, ios::binary);
			if (file.is_open() == false)
				return false;
			stringstream ss;
			ss << file.rdbuf();
			text = ss.str();
			return true;
		}

		float Dist(float ax, float ay, float bx, float by)
		{
			const float dx = ax - bx;
			const float dy = ay - by;
			return ::sqrtf(dx * dx + dy * dy);
		}

		// 그쪽을 바라본다 (UE yaw - x 축에서 반시계, 도)
		void FaceToward(const BotSessionRef& bot, float x, float y)
		{
			const float dx = x - bot->x;
			const float dy = y - bot->y;
			if (dx * dx + dy * dy > 1.f)
				bot->ai.yaw = ::atan2f(dy, dx) * 57.29578f;
		}

		void SendMove(const BotSessionRef& bot, uint64 nowUs, bool running)
		{
			Protocol::C_MOVE pkt;
			Protocol::PosInfo* info = pkt.mutable_info();
			info->set_object_id(bot->objectId.load());
			info->set_x(bot->x);
			info->set_y(bot->y);
			info->set_z(bot->z);
			info->set_yaw(bot->ai.yaw);
			info->set_state(running ? Protocol::MOVE_STATE_RUN : Protocol::MOVE_STATE_IDLE);
			if (GNoClientMs.load() == false)
				pkt.set_client_ms(nowUs / 1000);
			bot->Send(ClientPacketHandler::MakeSendBuffer(pkt));
			++GSentThisWindow;
		}

		// 길을 따라 한 걸음. 목표가 바뀌었거나 오래됐으면 다시 찾는다.
		// 막힘: 서버가 벽(엄폐)에서 자리를 되돌려 1 s 넘게 제자리면 가던 방향의 반대로 1.5 m 물러선 뒤 길과 탐색 지점을 버린다 (실기동 2026-10-12: 엄폐 모서리에서 170 번 거부)
		// 서버가 받아 줄 걸음만 옮긴다 (벽 · 엄폐물 · 바다 - BotPath::SegmentClear). 막히면 false - 자리는 그대로
		bool TryStep(const BotSessionRef& bot, float nx, float ny)
		{
			if (BotPath::IsLoaded() && bot->floorId == BotPath::LoadedFloor() && BotPath::SegmentClear(bot->x, bot->y, nx, ny) == false)
			{
				++GAiBlockedSteps;
				return false;
			}
			bot->x = nx;
			bot->y = ny;
			return true;
		}

		void StepToward(const BotSessionRef& bot, float goalX, float goalY, uint64 nowUs, float deltaTime, float speedScale = 1.f)
		{
			BotAiState& ai = bot->ai;
			if (nowUs < ai.detourUntilUs)
			{
				const float dx = ai.detourX - bot->x;
				const float dy = ai.detourY - bot->y;
				const float dist = ::sqrtf(dx * dx + dy * dy);
				const float step = MOVE_SPEED * deltaTime;
				const float ratio = (dist > 1.f) ? ((step < dist) ? (step / dist) : 1.f) : 0.f;
				if (TryStep(bot, bot->x + dx * ratio, bot->y + dy * ratio) == false)
					ai.detourUntilUs = 0;		// 물러설 쪽도 막혔다 - 길을 다시 찾는다
				return;
			}
			if (ai.progressUs == 0 || Dist(bot->x, bot->y, ai.progressX, ai.progressY) > 30.f)
			{
				ai.progressX = bot->x;
				ai.progressY = bot->y;
				ai.progressUs = nowUs;
			}
			else if (nowUs - ai.progressUs > 1'000'000)
			{
				float dirX = 0.f, dirY = 0.f;
				if (ai.pathIndex < ai.path.size())
				{
					dirX = ai.path[ai.pathIndex].first - bot->x;
					dirY = ai.path[ai.pathIndex].second - bot->y;
				}
				else
				{
					dirX = goalX - bot->x;
					dirY = goalY - bot->y;
				}
				const float len = ::sqrtf(dirX * dirX + dirY * dirY);
				if (len > 1.f) { dirX /= len; dirY /= len; }
				ai.detourX = bot->x - dirX * 150.f;
				ai.detourY = bot->y - dirY * 150.f;
				ai.detourUntilUs = nowUs + 600'000;
				ai.path.clear();
				ai.pathPlannedUs = 0;
				ai.progressUs = nowUs;
				ai.stuckCount++;
				ai.bb.wanderUntilUs = 0;		// 탐색 중이었다면 새 지점
				// 같은 자리(3 m 안)에서 세 번째면 목표를 버린다 - 길찾기가 벽을 지나는 길을 준 것(집 안 상자 등, 실기동 2026-10-12: 한 봇이 240 s 중 102 번 물러섰다).
				// 가방은 "열어 봤다" 로 두고 대상을 잊는다. 물러서는 방향은 무작위 3 m
				const bool sameSpot = ai.stuckCount >= 3 && Dist(bot->x, bot->y, ai.stuckX, ai.stuckY) < 300.f;
				if (sameSpot)
				{
					{
						lock_guard<mutex> guard(bot->itemLock);
						if (ai.bb.lootId != 0)
							ai.lootTried.insert(ai.bb.lootId);
					}
					ai.bb.targetId = 0;
					ai.bb.lootId = 0;
					const float a = ai.bb.Random01() * 6.2831853f;
					ai.detourX = bot->x + ::cosf(a) * 300.f;
					ai.detourY = bot->y + ::sinf(a) * 300.f;
					ai.detourUntilUs = nowUs + 1'200'000;
					ai.stuckCount = 0;
				}
				ai.stuckX = bot->x;
				ai.stuckY = bot->y;
				if (GAiTrace.load())
					LOG_LINE << "[AI] char=" << bot->objectId.load() << " stuck at=(" << ::lroundf(bot->x) << "," << ::lroundf(bot->y) << ") " << (sameSpot ? "gave up the goal, random detour" : "backing off") << " (" << ai.stuckCount << ")\n";
				return;
			}
			const bool drifted = Dist(goalX, goalY, ai.pathGoalX, ai.pathGoalY) > REPLAN_GOAL_DRIFT_CM;
			if (drifted || ai.path.empty() || nowUs - ai.pathPlannedUs > REPLAN_INTERVAL_US)
			{
				if (BotPath::IsLoaded() && bot->floorId == BotPath::LoadedFloor())
					ai.path = BotPath::Find(bot->x, bot->y, goalX, goalY);
				else
					ai.path = { { goalX, goalY } };
				if (ai.path.empty())
					ai.path = { { goalX, goalY } };
				ai.pathIndex = 0;
				ai.pathGoalX = goalX;
				ai.pathGoalY = goalY;
				ai.pathPlannedUs = nowUs;
			}
			while (ai.pathIndex < ai.path.size())
			{
				const float wx = ai.path[ai.pathIndex].first - bot->x;
				const float wy = ai.path[ai.pathIndex].second - bot->y;
				if (wx * wx + wy * wy > WAYPOINT_REACHED_CM * WAYPOINT_REACHED_CM)
					break;
				ai.pathIndex++;
			}
			if (ai.pathIndex >= ai.path.size())
				return;

			float speed = MOVE_SPEED * (std::clamp)(speedScale, 0.3f, 1.f);		// 둘러볼 때는 걷는다 (성격 explore_speed)
			if (nowUs < bot->slowUntilUs.load())
				speed = (std::min)(speed, (std::max)(MOVE_SPEED * 0.5f, MOVE_SPEED * (1.f - bot->slowMagnitude.load())));
			const float tx = ai.path[ai.pathIndex].first;
			const float ty = ai.path[ai.pathIndex].second;
			FaceToward(bot, tx, ty);
			const float dx = tx - bot->x;
			const float dy = ty - bot->y;
			const float dist = ::sqrtf(dx * dx + dy * dy);
			const float step = speed * deltaTime;
			const float ratio = (step < dist) ? (step / dist) : 1.f;
			if (TryStep(bot, bot->x + dx * ratio, bot->y + dy * ratio) == false)
			{
				// 꺾는 점이 엄폐물 모서리를 스친다 - 다음 틱에 지금 자리에서 길을 다시 찾는다. 계속 막히면 위의 막힘 판정(1 s)이 물러서게 한다
				ai.path.clear();
				ai.pathPlannedUs = 0;
			}
		}

		BotAI::World BuildWorld(const BotSessionRef& bot, uint64 nowUs)
		{
			BotAI::World w;
			w.nowUs = nowUs;
			w.self.id = bot->objectId.load();
			w.self.x = bot->x;
			w.self.y = bot->y;
			w.self.hardCc = nowUs < bot->hardCcUntilUs.load();
			w.self.casting = nowUs < bot->castUnitlUs;
			w.self.dashing = nowUs < bot->dashUntilUs.load();
			w.self.moveSpeedCms = MOVE_SPEED;
			w.self.attackReadyAtUs = bot->nextAttackAtUs;
			w.walkable = &BotPath::Walkable;		// 탐색 · 도망 지점은 섬 안 · 벽 밖 (bounds 거부를 막는다)
			if (bot->floorId == 0)
				w.pois = GPois;

			lock_guard<mutex> guard(bot->itemLock);
			const BotAiState& ai = bot->ai;
			w.self.hp = ai.hp;
			w.self.maxHp = ai.maxHp;
			w.self.runEndsAtUs = ai.runEndsAtUs;
			w.self.gaugePermille = ai.gaugePermille;
			w.self.inEscapeZone = ai.inZone;
			w.self.lastDamagedUs = ai.lastDamagedUs;
			w.self.weaponTypeId = ai.weaponTypeId;
			if (auto weapon = GWeapons.find(ai.weaponTypeId); weapon != GWeapons.end())
			{
				w.self.attackRangeCm = weapon->second.rangeCm;
				w.self.ranged = weapon->second.ranged;
				if (nowUs < ai.rangeBuffUntilUs)
					w.self.attackRangeCm = (std::max)(100.f, w.self.attackRangeCm + static_cast<float>(ai.rangeBuffCm));
			}
			int32 carried = 0, value = 0;
			for (const auto& [id, item] : bot->items)
			{
				if (item.state() == Protocol::ITEM_STATE_CARRIED)
				{
					carried++;
					value += static_cast<int32>(item.grade());		// 등급 합 (NONE 0 · COMMON 1 ...)
				}
			}
			w.self.carriedCount = carried;
			w.self.carriedValue = value;

			for (const auto& [id, c] : ai.creatures)
			{
				BotAI::Creature cr;
				cr.id = id;
				cr.kind = c.monster ? BotAI::CreatureKind::Monster : BotAI::CreatureKind::Player;
				cr.x = c.x;
				cr.y = c.y;
				cr.hp = c.hp;
				cr.maxHp = c.maxHp;
				cr.ally = c.ally;
				cr.lastHitMeUs = c.lastHitMeUs;
				cr.monsterTypeId = c.monsterTypeId;
				cr.power = c.monster ? 1.f : 1.f;
				cr.firstSeenUs = c.firstSeenUs;
				w.creatures.push_back(cr);
			}
			for (const auto& [id, pos] : bot->bags)
			{
				BotAI::Loot l;
				l.id = id;
				l.x = pos.first;
				l.y = pos.second;
				l.chest = false;
				l.triedUs = ai.lootTried.count(id) > 0;
				w.loot.push_back(l);
			}
			for (const BotAI::Tower& t : GTowers)
				w.towers.push_back(t);
			for (const auto& [slot, skillId] : ai.skillSlots)
			{
				BotAI::SkillSlot s;
				if (GSkills.Fill(slot, skillId, s) == false)
					continue;
				auto it = ai.skillReadyAtUs.find(slot);
				s.readyAtUs = (it != ai.skillReadyAtUs.end()) ? it->second : 0;
				s.readyAtUs = (std::max)(s.readyAtUs, ai.skillLockUntilUs);		// 방금 쓴 스킬의 회복 · 기본 공격만 버프 동안은 아무 칸도 못 쓴다
				w.skills.push_back(s);
			}
			return w;
		}

		void Trace(const BotSessionRef& bot, const BotAI::Action& a, const BotAI::World& w)
		{
			static const char* names[] = { "Idle", "MoveTo", "Attack", "UseSkill", "OpenLoot", "Escape" };
			LOG_LINE << "[AI] char=" << bot->objectId.load() << " branch=" << a.branch << " action=" << names[static_cast<int32>(a.type)]
				<< " reason=\"" << a.reason << "\" target=" << a.targetId << " to=(" << ::lroundf(a.x) << "," << ::lroundf(a.y) << ")"
				<< " hp=" << w.self.hp << "/" << w.self.maxHp << " at=(" << ::lroundf(w.self.x) << "," << ::lroundf(w.self.y) << ")"
				<< " seen=" << w.creatures.size() << " loot=" << w.loot.size() << '\n';
		}

		// 이번 판의 성격 - 서버가 입힌 빌드(무기 + Shift · Q)를 표에서 찾아 그 role 로. 못 찾으면 무기만 맞는 빌드의 role, 그것도 없으면 balanced
		void AssignPersona(const BotSessionRef& bot, uint64 nowUs)
		{
			BotAiState& ai = bot->ai;
			uint32 weapon = 0, shift = 0, q = 0;
			size_t slots = 0;
			{
				lock_guard<mutex> guard(bot->itemLock);
				weapon = ai.weaponTypeId;
				slots = ai.skillSlots.size();
				for (const auto& [slot, skillId] : ai.skillSlots)
				{
					if (slot == Protocol::SLOT_WEAPON_PRIMARY) shift = skillId;
					else if (slot == Protocol::SLOT_WEAPON_SECONDARY) q = skillId;
				}
			}
			if (slots == 0 && nowUs < bot->enteredAtUs + PERSONA_WAIT_US)
				return;		// 스킬 칸이 아직 안 왔다

			const LoadoutRow* match = nullptr;
			for (const LoadoutRow& l : GLoadouts)
				if (l.weapon == weapon && l.shift == shift && l.q == q) { match = &l; break; }
			string role = "balanced";
			if (match != nullptr)
			{
				role = match->role;
				ai.build = match->code;
			}
			else
			{
				// 무기만 맞는 빌드들 중 칠판 난수로 하나의 성격 (봇마다 섞이게)
				vector<const LoadoutRow*> same;
				for (const LoadoutRow& l : GLoadouts)
					if (l.weapon == weapon) same.push_back(&l);
				if (same.empty() == false)
					role = same[ai.bb.NextRandom() % same.size()]->role;
			}
			ai.bb.persona = GPersonas.Make(role, ai.bb);
			ai.personaSet = true;
			LOG_LINE << "[AI] char=" << bot->objectId.load() << " persona=" << ai.bb.persona.name << " build=" << (ai.build.empty() ? "-" : ai.build)
				<< " weapon=" << weapon << " reaction=" << ai.bb.persona.reactionUs / 1000 << "ms strafe=" << ai.bb.persona.strafe << '\n';
		}

		/*-------- 대본 --------*/

		struct ScriptLine { string cmd; vector<string> args; };
		vector<ScriptLine> GScript;

		bool RunScript(const BotSessionRef& bot, uint64 nowUs, float deltaTime)
		{
			BotAiState& ai = bot->ai;
			if (ai.scriptIndex >= GScript.size())
			{
				SendMove(bot, nowUs, false);
				return true;
			}
			if (nowUs < ai.scriptWaitUntilUs)
			{
				if (ai.scriptMoving)
				{
					StepToward(bot, ai.scriptGoalX, ai.scriptGoalY, nowUs, deltaTime);
					SendMove(bot, nowUs, true);
					if (Dist(bot->x, bot->y, ai.scriptGoalX, ai.scriptGoalY) < 30.f)
					{
						ai.scriptMoving = false;
						ai.scriptWaitUntilUs = 0;
					}
				}
				else
					SendMove(bot, nowUs, false);
				return true;
			}
			ai.scriptMoving = false;

			const ScriptLine& line = GScript[ai.scriptIndex++];
			auto num = [&](size_t i, float def) { return (i < line.args.size()) ? static_cast<float>(::atof(line.args[i].c_str())) : def; };
			LOG_LINE << "[AI SCRIPT] char=" << bot->objectId.load() << " #" << ai.scriptIndex << " " << line.cmd << '\n';
			if (line.cmd == "wait" || line.cmd == "idle")
				ai.scriptWaitUntilUs = nowUs + static_cast<uint64>(num(0, 1000.f)) * 1000;
			else if (line.cmd == "goto")
			{
				ai.scriptGoalX = num(0, bot->x);
				ai.scriptGoalY = num(1, bot->y);
				ai.scriptMoving = true;
				ai.scriptWaitUntilUs = nowUs + 60'000'000;		// 도착하면 푼다
			}
			else if (line.cmd == "attack")
			{
				BotAI::World w = BuildWorld(bot, nowUs);
				const BotAI::Creature* t = BotAI::Brain::NearestThreat(w, 100000.f);
				if (t != nullptr)
				{
					Protocol::C_ATTACK pkt;
					pkt.set_target_id(t->id);
					bot->Send(ClientPacketHandler::MakeSendBuffer(pkt));
					GAiAttacks.fetch_add(1);
				}
			}
			else if (line.cmd == "skill")
			{
				Protocol::C_SKILL pkt;
				pkt.set_slot(static_cast<Protocol::EquipSlot>(static_cast<int32>(num(0, 1.f))));
				pkt.set_target_id(0);
				pkt.set_aim_x(num(1, bot->x + 300.f));
				pkt.set_aim_y(num(2, bot->y));
				bot->Send(ClientPacketHandler::MakeSendBuffer(pkt));
				GAiSkills.fetch_add(1);
			}
			else if (line.cmd == "open")
			{
				uint64 bagId = 0;
				float best = FLT_MAX;
				{
					lock_guard<mutex> guard(bot->itemLock);
					for (const auto& [id, pos] : bot->bags)
					{
						const float d = Dist(bot->x, bot->y, pos.first, pos.second);
						if (d < best) { best = d; bagId = id; }
					}
					bot->openBagId = bagId;
				}
				if (bagId != 0)
				{
					Protocol::C_BAG_OPEN pkt;
					pkt.set_bag_id(bagId);
					bot->Send(ClientPacketHandler::MakeSendBuffer(pkt));
					GAiLootOpens.fetch_add(1);
				}
			}
			else if (line.cmd == "escape")
			{
				Protocol::C_CHAT chat;
				chat.set_msg("/escape");
				bot->Send(ClientPacketHandler::MakeSendBuffer(chat));
			}
			else if (line.cmd == "say")
			{
				Protocol::C_CHAT chat;
				string msg;
				for (const string& a : line.args) msg += (msg.empty() ? "" : " ") + a;
				chat.set_msg(msg);
				bot->Send(ClientPacketHandler::MakeSendBuffer(chat));
			}
			else if (line.cmd == "loop")
				ai.scriptIndex = 0;
			return true;
		}
	}

	bool Init(OUT string& error)
	{
		const string dir = DataDir();
		string text;
		BotAI::BrainConfig cfg;
		if (ReadFile(dir + "\\bot_brain.csv", OUT text) == false)
		{
			error = "cannot read " + dir + "\\bot_brain.csv";
			return false;
		}
		if (cfg.Parse(text, OUT error) == false)
			return false;

		string skills, effects;
		if (ReadFile(dir + "\\skills.csv", OUT skills) == false || ReadFile(dir + "\\skill_effects.csv", OUT effects) == false)
		{
			error = "cannot read skills.csv / skill_effects.csv in " + dir;
			return false;
		}
		if (GSkills.Parse(skills, effects, OUT error) == false)
			return false;

		GTowers.clear();
		if (ReadFile(dir + "\\towers.csv", OUT text))
		{
			stringstream ss(text);
			string line;
			bool header = true;
			while (getline(ss, line))
			{
				if (line.empty() == false && line.back() == '\r') line.pop_back();
				if (header) { header = false; continue; }
				vector<string> cells;
				string cur;
				for (char ch : line) { if (ch == ',') { cells.push_back(cur); cur.clear(); } else cur.push_back(ch); }
				cells.push_back(cur);
				if (cells.size() < 4 || cells[1] != "0")
					continue;
				BotAI::Tower t;
				t.id = cells[0];
				t.x = static_cast<float>(::atof(cells[2].c_str()));
				t.y = static_cast<float>(::atof(cells[3].c_str()));
				GTowers.push_back(t);
			}
		}

		// 2026-10-20 - 무기 · 빌드 · 성격 · 둘러볼 곳. 성격 표가 없으면 옛 두뇌 그대로, 있는데 틀리면 시작하지 않는다
		GWeapons.clear();
		if (ReadFile(dir + "\\item_types.csv", OUT text))
		{
			for (const auto& row : ReadRows(text))
			{
				if (Num(row, "equip_slot") != 1.f)
					continue;
				WeaponInfo info;
				info.rangeCm = Num(row, "attack_range_cm");
				info.attackSpeed = (std::max)(0.2f, Num(row, "attack_speed"));
				auto kind = row.find("attack_kind");
				info.ranged = (kind != row.end() && kind->second == "projectile");
				GWeapons[static_cast<uint32>(Num(row, "id"))] = info;
			}
		}
		GLoadouts.clear();
		if (ReadFile(dir + "\\bot_loadouts.csv", OUT text))
		{
			for (const auto& row : ReadRows(text))
			{
				LoadoutRow l;
				l.code = row.count("code") ? row.at("code") : "";
				l.role = row.count("role") ? row.at("role") : "balanced";
				l.weapon = static_cast<uint32>(Num(row, "weapon_type"));
				l.shift = static_cast<uint32>(Num(row, "shift_skill"));
				l.q = static_cast<uint32>(Num(row, "q_skill"));
				GLoadouts.push_back(l);
			}
		}
		GRecoveryMs.clear();
		if (ReadFile(dir + "\\skill_recovery.csv", OUT text))
		{
			for (const auto& row : ReadRows(text))
				GRecoveryMs[static_cast<uint32>(Num(row, "skill_id"))] = static_cast<uint32>(Num(row, "recovery_ms"));
		}
		GPersonas.byRole.clear();
		if (ReadFile(dir + "\\bot_personas.csv", OUT text) && GPersonas.Parse(text, OUT error) == false)
			return false;
		GPois.clear();
		for (const BotAI::Tower& t : GTowers)
			GPois.push_back({ t.x, t.y, 2 });
		if (ReadFile(dir + "\\chest_spawns.csv", OUT text))
		{
			for (const auto& row : ReadRows(text))
				if (Num(row, "floor_id") == 0.f)
					GPois.push_back({ Num(row, "x"), Num(row, "y"), 0 });
		}
		if (ReadFile(dir + "\\houses.csv", OUT text))
		{
			for (const auto& row : ReadRows(text))
				if (Num(row, "floor_id") == 0.f)
					GPois.push_back({ (Num(row, "x0") + Num(row, "x1")) * 0.5f, (Num(row, "y0") + Num(row, "y1")) * 0.5f, 1 });
		}

		static BotAI::Brain brain(cfg);
		GBrain = &brain;
		LOG_LINE << "[AI] brain ready - skills=" << GSkills.entries.size() << " towers=" << GTowers.size() << " flee<" << cfg.fleeHpRatio
			<< " escape_left=" << cfg.escapeWhenLeftUs / 1'000'000 << "s loot_goal=" << cfg.lootGoalValue
			<< " weapons=" << GWeapons.size() << " builds=" << GLoadouts.size() << " personas=" << GPersonas.byRole.size() << " pois=" << GPois.size() << '\n';
		return true;
	}

	bool LoadScript(const string& path, OUT string& error)
	{
		GScript.clear();
		string text;
		if (ReadFile(path, OUT text) == false)
		{
			error = "cannot read " + path;
			return false;
		}
		stringstream ss(text);
		string line;
		while (getline(ss, line))
		{
			if (line.empty() == false && line.back() == '\r') line.pop_back();
			if (line.empty() || line[0] == '#')
				continue;
			stringstream ls(line);
			ScriptLine sl;
			ls >> sl.cmd;
			string a;
			while (ls >> a) sl.args.push_back(a);
			GScript.push_back(sl);
		}
		if (GScript.empty())
		{
			error = "script is empty";
			return false;
		}
		return true;
	}

	bool HasScript() { return GScript.empty() == false; }

	void Tick(const BotSessionRef& bot, uint64 nowUs, float deltaTime)
	{
		if (GBrain == nullptr)
			return;
		if (HasScript() && bot->botIndex == 1)
		{
			RunScript(bot, nowUs, deltaTime);
			return;
		}

		BotAiState& ai = bot->ai;
		if (ai.personaSet == false)
			AssignPersona(bot, nowUs);

		// 재입력 스킬의 둘째 누름 (찌르고 베기 · 피의 궤적 · 비창 ...) - 시전이 끝나고 잠깐 뒤 같은 칸을 한 번 더
		if (ai.recastAtUs != 0 && nowUs >= ai.recastAtUs)
		{
			ai.recastAtUs = 0;
			float tx = bot->x, ty = bot->y;
			bool seen = false;
			{
				lock_guard<mutex> guard(bot->itemLock);
				auto it = ai.creatures.find(ai.recastTargetId);
				if (it != ai.creatures.end()) { tx = it->second.x; ty = it->second.y; seen = true; }
			}
			if (seen)
			{
				Protocol::C_SKILL pkt;
				pkt.set_slot(static_cast<Protocol::EquipSlot>(ai.recastSlot));
				pkt.set_target_id(ai.recastTargetId);
				pkt.set_aim_x(tx);
				pkt.set_aim_y(ty);
				bot->Send(ClientPacketHandler::MakeSendBuffer(pkt));
				GAiSkills.fetch_add(1);
				FaceToward(bot, tx, ty);
			}
		}

		const BotAI::World world = BuildWorld(bot, nowUs);
		const BotAI::Action a = GBrain->Decide(world, ai.bb);
		GAiDecisions.fetch_add(1);
		if (ai.bb.lastBranch != a.branch)
		{
			ai.bb.lastBranch = a.branch;
			ai.lastReason = a.reason;
			GAiBranchChanges.fetch_add(1);
			if (GAiTrace.load())
				Trace(bot, a, world);
		}
		else if (ai.lastReason != a.reason)
		{
			// 같은 가지 안의 손놀림 바뀜 (평타 ↔ 옆걸음 ↔ 스킬) - 추적 모드에서만 남긴다. 이유 문자열은 두뇌의 상수라 주소로 견준다
			ai.lastReason = a.reason;
			if (GAiTrace.load())
				Trace(bot, a, world);
		}

		// 돌진 중엔 몸이 스스로 난다 (서버가 검증한다) - 기존 봇과 같은 흉내
		if (world.self.dashing)
		{
			// 시전 중엔 서서 기다리고, 시전이 끝나면 표의 속도로 난다. 벽 · 바다면 그 자리에서 멈춘다 (서버도 벽 앞에서 멈춘다)
			if (nowUs >= ai.dashStartUs)
			{
				const float speed = (ai.dashSpeedCms > 0.f) ? ai.dashSpeedCms : DASH_SPEED;
				const float nx = bot->x + bot->dashDirX * speed * deltaTime;
				const float ny = bot->y + bot->dashDirY * speed * deltaTime;
				if (TryStep(bot, nx, ny) == false)
					bot->dashUntilUs = 0;		// 벽 앞에서 멈춘다 (서버도 벽 앞에서 멈춘다)
			}
			SendMove(bot, nowUs, true);
			return;
		}

		switch (a.type)
		{
		case BotAI::ActionType::MoveTo:
			if (world.self.casting == false)
				StepToward(bot, a.x, a.y, nowUs, deltaTime, a.speedScale);
			SendMove(bot, nowUs, world.self.casting == false);
			break;

		case BotAI::ActionType::Attack:
		{
			// 대상을 바라본다 - 옆걸음 뒤에도 몸이 상대 쪽으로 돌아선다
			{
				lock_guard<mutex> guard(bot->itemLock);
				auto it = ai.creatures.find(a.targetId);
				if (it != ai.creatures.end())
					FaceToward(bot, it->second.x, it->second.y);
			}
			SendMove(bot, nowUs, false);
			if (nowUs >= bot->nextAttackAtUs)
			{
				Protocol::C_ATTACK pkt;
				pkt.set_target_id(a.targetId);
				bot->Send(ClientPacketHandler::MakeSendBuffer(pkt));
				// 무기 공속대로 (단검 1.3 · 망치 0.8 ...) + 사람 손 흔들림. 모르면 1 s
				uint64 interval = ATTACK_INTERVAL_US;
				if (auto weapon = GWeapons.find(world.self.weaponTypeId); weapon != GWeapons.end())
					interval = static_cast<uint64>(1'000'000.f / weapon->second.attackSpeed);
				bot->nextAttackAtUs = nowUs + interval + static_cast<uint64>(ai.bb.Random01() * static_cast<float>(ATTACK_JITTER_US));
				GAiAttacks.fetch_add(1);
			}
			break;
		}

		case BotAI::ActionType::UseSkill:
		{
			SendMove(bot, nowUs, false);
			Protocol::C_SKILL pkt;
			pkt.set_slot(static_cast<Protocol::EquipSlot>(a.slot));
			pkt.set_target_id(a.targetId);
			pkt.set_aim_x(a.x);
			pkt.set_aim_y(a.y);
			bot->Send(ClientPacketHandler::MakeSendBuffer(pkt));
			GAiSkills.fetch_add(1);
			FaceToward(bot, a.x, a.y);
			// 쿨다운 · 시전 - 서버가 거절해도 같은 칸을 바로 다시 누르지 않는다
			const BotAI::SkillSlot* used = nullptr;
			for (const BotAI::SkillSlot& s : world.skills)
				if (s.slot == a.slot) used = &s;
			{
				lock_guard<mutex> guard(bot->itemLock);
				ai.skillReadyAtUs[a.slot] = nowUs + (used ? (static_cast<uint64>(used->castMs) + used->cooldownMs) * 1000 : 3'000'000);
			}
			if (used != nullptr)
			{
				// 다음 스킬 · 다음 걸음은 시전 + 회복(동작 클립)이 끝난 뒤 - 서버가 그동안의 이동을 [MOVE BLOCKED] recovery 로 막고
				// 안티치트 move_while_locked 점수를 쌓는다 (실기동 2026-10-20: 20 봇 180 s 에 375 줄). 기본 공격만 버프면 스킬은 그 시간 내내
				uint64 recoveryUs = 0;
				if (auto rec = GRecoveryMs.find(used->skillId); rec != GRecoveryMs.end())
					recoveryUs = static_cast<uint64>(rec->second) * 1000;
				// 서버 후딜은 "치는 때"부터 - 돌진 · 도약은 표의 거리를 다 날아간 시간이 먼저 붙는다 (Room 후딜: TravelMs(dist) + recovery_ms)
				if (recoveryUs > 0 && used->dashSpeedCms > 0.f && used->dashDistCm > 0.f
					&& (used->dashMode == BotAI::DashMode::Aim || used->dashMode == BotAI::DashMode::Back || used->dashMode == BotAI::DashMode::ToTarget))
					recoveryUs += static_cast<uint64>(used->dashDistCm / used->dashSpeedCms * 1'000'000.f);
				bot->castUnitlUs = nowUs + static_cast<uint64>(used->castMs) * 1000 + recoveryUs + CAST_HOLD_EXTRA_US;
				uint64 lockUs = nowUs + static_cast<uint64>(used->castMs) * 1000 + recoveryUs + RECOVERY_SLACK_US;
				if (used->lockOthersMs > 0)
					lockUs = (std::max)(lockUs, nowUs + (static_cast<uint64>(used->castMs) + used->lockOthersMs) * 1000 + BASIC_ONLY_SLACK_US);
				{
					lock_guard<mutex> guard(bot->itemLock);
					ai.skillLockUntilUs = (std::max)(ai.skillLockUntilUs, lockUs);
				}
				// 이동 스킬 - 표의 거리 · 속도 그대로 몸을 날린다 (예전엔 모든 이동을 4 m @ 1200 으로 흉내 내 서버가 speed 로 거절했다, 실기동 2026-10-20)
				if (used->dashMode == BotAI::DashMode::Aim || used->dashMode == BotAI::DashMode::Back || used->dashMode == BotAI::DashMode::ToTarget)
				{
					float dx = a.x - bot->x;
					float dy = a.y - bot->y;
					const float len = ::sqrtf(dx * dx + dy * dy);
					float dist = used->dashDistCm;
					if (used->dashMode == BotAI::DashMode::ToTarget)
						dist = (std::min)(dist, (std::max)(0.f, len - DASH_STOP_SHORT_CM));		// 대상 앞에서 멈춘다
					dist *= DASH_DIST_SAFETY;		// 서버 속도 창은 치는 때의 거리로 열린다 - 끝 몇 걸음이 창 밖으로 나가 speed 거부 (실기동 30 봇: 1165 뒤 10 건)
					if (used->dashMode == BotAI::DashMode::Back) { dx = -dx; dy = -dy; }
					if (len > 1.f && dist > 1.f && used->dashSpeedCms > 0.f)
					{
						bot->dashDirX = dx / len;
						bot->dashDirY = dy / len;
						ai.dashSpeedCms = used->dashSpeedCms;
						// 시전이 끝나고 서버가 속도 창을 연 뒤에 날기 시작한다 (먼저 날면 reason=speed 로 거부된다, 실기동 2026-10-20)
						ai.dashStartUs = nowUs + static_cast<uint64>(used->castMs) * 1000 + DASH_START_DELAY_US;
						bot->dashUntilUs = ai.dashStartUs + static_cast<uint64>(dist / used->dashSpeedCms * 1'000'000.f);
					}
				}
				if (used->rangeBuffMs > 0)
				{
					lock_guard<mutex> guard(bot->itemLock);
					ai.rangeBuffCm = used->rangeBuffCm;
					ai.rangeBuffUntilUs = nowUs + (static_cast<uint64>(used->castMs) + used->rangeBuffMs) * 1000;
				}
				if (used->recast && a.targetId != 0)
				{
					ai.recastSlot = a.slot;
					ai.recastTargetId = a.targetId;
					ai.recastAtUs = bot->castUnitlUs + RECAST_MIN_US + static_cast<uint64>(ai.bb.Random01() * static_cast<float>(RECAST_MAX_US - RECAST_MIN_US));
				}
				if (used->movement && used->dashMode == BotAI::DashMode::None)
				{
					// 표에 이동 효과 행이 없는 칸 (손으로 만든 칸 · 옛 표) - 예전 흉내
					float dx = a.x - bot->x;
					float dy = a.y - bot->y;
					const float len = ::sqrtf(dx * dx + dy * dy);
					if (len > 1.f)
					{
						bot->dashDirX = dx / len;
						bot->dashDirY = dy / len;
						ai.dashSpeedCms = DASH_SPEED;
						ai.dashStartUs = nowUs;
						bot->dashUntilUs = nowUs + DASH_DURATION_US;
					}
				}
			}
			break;
		}

		case BotAI::ActionType::OpenLoot:
		{
			SendMove(bot, nowUs, false);
			{
				lock_guard<mutex> guard(bot->itemLock);
				ai.lootTried.insert(a.targetId);		// 한 번만 - 열기가 거절돼도 다시 매달리지 않는다
				bot->openBagId = a.targetId;
			}
			Protocol::C_BAG_OPEN pkt;
			pkt.set_bag_id(a.targetId);
			bot->Send(ClientPacketHandler::MakeSendBuffer(pkt));
			GAiLootOpens.fetch_add(1);
			break;
		}

		case BotAI::ActionType::Escape:
			// 구역 안에 서 있는다 - 서버 게이지가 채워 탈출시킨다. 구역 가장자리면 중심으로 조금 더
			if (world.self.inEscapeZone == false && world.self.casting == false)
				StepToward(bot, a.x, a.y, nowUs, deltaTime);
			SendMove(bot, nowUs, world.self.inEscapeZone == false && world.self.casting == false);
			break;

		case BotAI::ActionType::Idle:
		default:
			SendMove(bot, nowUs, false);
			break;
		}
	}
}
