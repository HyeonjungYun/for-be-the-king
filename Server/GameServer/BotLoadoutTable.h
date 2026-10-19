#pragma once
#include <functional>

struct ItemEntry;

/*--------------------
	BotLoadoutTable
--------------------*/

// Server/Data/bot_loadouts.csv — 봇이 입고 들어오는 스킬 셋 (2026-10-20 · design/quick-specs/bot-loadouts-2026-10-20.md).
// 행 하나 = design/quick-specs/skill-loadouts-2026-10-12.md 의 빌드 하나 (무기 7 × 5 = 35). 무기 · 모자 · 갑옷 · 신발 · 장신구와 키별 스킬.
// FBTK_BOT_LOADOUTS=1 일 때만 쓴다 - bot_ 캐릭터가 비어 있는 부위로 입장하면 무작위 빌드 하나로 그 부위를 채운다 (런 중 생성 isNew).
// 무기가 남아 있으면 그 무기의 빌드 중에서 고른다 - 갑옷 스킬의 requires_weapon 이 맞는 짝으로 들어오게.
// ItemTable 다음에 읽는다 - 종류가 그 부위에 맞는지, 스킬이 그 종류의 풀에 있는지 기본 장비와 같은 검사를 한다.

struct BotLoadoutPiece
{
	Protocol::EquipSlot	slot = Protocol::SLOT_NONE;
	uint32				itemTypeId = 0;		// 0 = 이 부위는 비워 둔다
	uint32				skillId = 0;
	uint32				skillIdSecondary = 0;	// 무기의 Q 키
};

struct BotLoadout
{
	uint32					buildId = 0;
	string					code;			// "S-1" - 설계안의 빌드 번호
	string					role;			// duel · pack · runner · balanced (봇 성격 - DummyClient 가 같은 표를 읽는다)
	uint32					weight = 1;
	vector<BotLoadoutPiece>	pieces;

	uint32		WeaponTypeId() const;
};

class BotLoadoutTable
{
public:
	// 파일이 없으면 빈 표로 true (선택 기능). 있는데 틀리면 이유를 error 에 남기고 false
	static bool		Load(const string& dataDir, OUT string& error);
	static bool		Parse(const string& csv, OUT string& error);

	// 무게대로 하나. weaponTypeId != 0 이면 그 무기의 빌드 중에서만 (없으면 nullptr). roll 은 0 이상 아무 수 (테스트가 고정한다)
	static const BotLoadout*	Pick(uint32 weaponTypeId, uint64 roll);

	// 비어 있는 부위만 그 빌드로 채운다 (이미 입은 부위는 그대로). 채운 개수
	static int32	EquipInto(const BotLoadout& loadout, ItemEntry* equipped, int32 slotCount, const function<uint64()>& nextInstanceId);

	static const vector<BotLoadout>& All() { return s_loadouts; }

private:
	static vector<BotLoadout>	s_loadouts;
};
