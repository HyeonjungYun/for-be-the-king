#pragma once
#include "BotSession.h"
#include "../BotAI/BotBrain.h"

/*------------------
	BotAiDriver
------------------*/

// -ai: 봇이 BotAI::Brain 으로 판단하고 패킷으로 실행한다 (2026-10-12 · design/quick-specs/bot-ai-2026-10-12.md).
// 감각 = 받은 패킷(ClientPacketHandler 가 BotSession.ai 에 적는다) · 손발 = C_MOVE · C_ATTACK · C_SKILL · C_BAG_OPEN.
// 두뇌는 패킷을 모른다 - 같은 두뇌를 나중에 서버 안 PVE AI 플레이어가 쓴다.
//   -ai          봇 전원이 두뇌로 논다 (기존 -items · -monsterpull · -tower 보다 우선)
//   -aitrace     가지가 바뀔 때마다 [AI] 한 줄 (char · 가지 · 행동 · 이유 · 체력 · 자리)
//   -aiscript F  1 번 봇은 두뇌 대신 대본 F 를 따른다 (한 줄에 명령 하나 - 아래 Script)

namespace BotAiDriver
{
	extern atomic<bool>		GAi;
	extern atomic<bool>		GAiTrace;

	// bot_brain.csv · skills.csv · skill_effects.csv · towers.csv (FBTK_DATA_DIR 또는 exe 기준 ..\..\Data). 실패하면 이유를 남기고 false
	bool	Init(OUT string& error);

	// 대본 (-aiscript). 명령: wait <ms> | goto <x> <y> | attack nearest | skill <slot> [x y] | open nearest | escape | idle <ms> | say <text> | loop
	bool	LoadScript(const string& path, OUT string& error);
	bool	HasScript();

	// 틱 스레드 - 입장 · 살아 있음 · 하드 CC 아님을 확인한 뒤 부른다
	void	Tick(const BotSessionRef& bot, uint64 nowUs, float deltaTime);

	// 10 초 창 통계 (DummyClient 가 보고 줄에 싣고 0 으로 돌린다)
	extern atomic<uint64>	GAiDecisions;
	extern atomic<uint64>	GAiBranchChanges;
	extern atomic<uint64>	GAiAttacks;
	extern atomic<uint64>	GAiSkills;
	extern atomic<uint64>	GAiLootOpens;
}
