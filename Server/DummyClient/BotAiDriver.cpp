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

		void SendMove(const BotSessionRef& bot, uint64 nowUs, bool running)
		{
			Protocol::C_MOVE pkt;
			Protocol::PosInfo* info = pkt.mutable_info();
			info->set_object_id(bot->objectId.load());
			info->set_x(bot->x);
			info->set_y(bot->y);
			info->set_z(bot->z);
			info->set_yaw(0.f);
			info->set_state(running ? Protocol::MOVE_STATE_RUN : Protocol::MOVE_STATE_IDLE);
			if (GNoClientMs.load() == false)
				pkt.set_client_ms(nowUs / 1000);
			bot->Send(ClientPacketHandler::MakeSendBuffer(pkt));
			++GSentThisWindow;
		}

		// 길을 따라 한 걸음. 목표가 바뀌었거나 오래됐으면 다시 찾는다.
		// 막힘: 서버가 벽(엄폐)에서 자리를 되돌려 1 s 넘게 제자리면 가던 방향의 반대로 1.5 m 물러선 뒤 길과 탐색 지점을 버린다 (실기동 2026-10-12: 엄폐 모서리에서 170 번 거부)
		void StepToward(const BotSessionRef& bot, float goalX, float goalY, uint64 nowUs, float deltaTime)
		{
			BotAiState& ai = bot->ai;
			if (nowUs < ai.detourUntilUs)
			{
				const float dx = ai.detourX - bot->x;
				const float dy = ai.detourY - bot->y;
				const float dist = ::sqrtf(dx * dx + dy * dy);
				const float step = MOVE_SPEED * deltaTime;
				const float ratio = (dist > 1.f) ? ((step < dist) ? (step / dist) : 1.f) : 0.f;
				bot->x += dx * ratio;
				bot->y += dy * ratio;
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

			float speed = MOVE_SPEED;
			if (nowUs < bot->slowUntilUs.load())
				speed = (std::max)(MOVE_SPEED * 0.5f, MOVE_SPEED * (1.f - bot->slowMagnitude.load()));
			const float tx = ai.path[ai.pathIndex].first;
			const float ty = ai.path[ai.pathIndex].second;
			const float dx = tx - bot->x;
			const float dy = ty - bot->y;
			const float dist = ::sqrtf(dx * dx + dy * dy);
			const float step = speed * deltaTime;
			const float ratio = (step < dist) ? (step / dist) : 1.f;
			bot->x += dx * ratio;
			bot->y += dy * ratio;
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
			w.walkable = &BotPath::Walkable;		// 탐색 · 도망 지점은 섬 안 · 벽 밖 (bounds 거부를 막는다)

			lock_guard<mutex> guard(bot->itemLock);
			const BotAiState& ai = bot->ai;
			w.self.hp = ai.hp;
			w.self.maxHp = ai.maxHp;
			w.self.runEndsAtUs = ai.runEndsAtUs;
			w.self.gaugePermille = ai.gaugePermille;
			w.self.inEscapeZone = ai.inZone;
			w.self.lastDamagedUs = ai.lastDamagedUs;
			w.self.weaponTypeId = ai.weaponTypeId;
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

		static BotAI::Brain brain(cfg);
		GBrain = &brain;
		LOG_LINE << "[AI] brain ready - skills=" << GSkills.entries.size() << " towers=" << GTowers.size() << " flee<" << cfg.fleeHpRatio
			<< " escape_left=" << cfg.escapeWhenLeftUs / 1'000'000 << "s loot_goal=" << cfg.lootGoalValue << '\n';
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
		const BotAI::World world = BuildWorld(bot, nowUs);
		const BotAI::Action a = GBrain->Decide(world, ai.bb);
		GAiDecisions.fetch_add(1);
		if (ai.bb.lastBranch != a.branch)
		{
			ai.bb.lastBranch = a.branch;
			GAiBranchChanges.fetch_add(1);
			if (GAiTrace.load())
				Trace(bot, a, world);
		}

		// 돌진 중엔 몸이 스스로 난다 (서버가 검증한다) - 기존 봇과 같은 흉내
		if (world.self.dashing)
		{
			const float step = DASH_SPEED * deltaTime;
			bot->x += bot->dashDirX * step;
			bot->y += bot->dashDirY * step;
			SendMove(bot, nowUs, true);
			return;
		}

		switch (a.type)
		{
		case BotAI::ActionType::MoveTo:
			if (world.self.casting == false)
				StepToward(bot, a.x, a.y, nowUs, deltaTime);
			SendMove(bot, nowUs, world.self.casting == false);
			break;

		case BotAI::ActionType::Attack:
			SendMove(bot, nowUs, false);
			if (nowUs >= bot->nextAttackAtUs)
			{
				Protocol::C_ATTACK pkt;
				pkt.set_target_id(a.targetId);
				bot->Send(ClientPacketHandler::MakeSendBuffer(pkt));
				bot->nextAttackAtUs = nowUs + ATTACK_INTERVAL_US;
				GAiAttacks.fetch_add(1);
			}
			break;

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
				bot->castUnitlUs = nowUs + static_cast<uint64>(used->castMs) * 1000 + CAST_HOLD_EXTRA_US;
				if (used->movement)
				{
					float dx = a.x - bot->x;
					float dy = a.y - bot->y;
					const float len = ::sqrtf(dx * dx + dy * dy);
					if (len > 1.f)
					{
						bot->dashDirX = dx / len;
						bot->dashDirY = dy / len;
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
			if (world.self.inEscapeZone == false)
				StepToward(bot, a.x, a.y, nowUs, deltaTime);
			SendMove(bot, nowUs, world.self.inEscapeZone == false);
			break;

		case BotAI::ActionType::Idle:
		default:
			SendMove(bot, nowUs, false);
			break;
		}
	}
}
