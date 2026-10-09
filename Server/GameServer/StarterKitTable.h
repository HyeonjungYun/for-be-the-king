#pragma once
#include <functional>

struct ItemEntry;

/*--------------------
	StarterKitTable
--------------------*/

// Server/Data/starter_kit.csv — 기본 장비 (design/quick-specs/account-login-mvp-2026-10-09.md Core Rule 8 · ADR-0011 Decision 4).
// 착용 · 소지 · 창고가 모두 빈 캐릭터가 입장하면 이 표대로 런 중 생성(isNew) 아이템을 입힌다 — DB 행은 런 종료 커밋이 만든다.
// 봇 재지급(RegearBot, FBTK_BOT_REGEAR)도 같은 표를 쓴다. reset_bots.sql 이 처음에 입히는 3점과 같아야 봇 측정이 예전과 비교된다.
// ItemTable 다음에 읽는다 - 종류가 그 부위에 맞는지, 스킬이 그 종류의 풀에 있는지 검사한다.

struct StarterKitPiece
{
	Protocol::EquipSlot	slot = Protocol::SLOT_NONE;
	uint32				itemTypeId = 0;
	uint32				skillId = 0;			// 0 = 스킬 없음
	uint32				skillIdSecondary = 0;	// 무기의 Q 키. 무기가 아니면 0
};

class StarterKitTable
{
public:
	// 실패하면 이유를 error 에 남기고 false. 표는 빈 채로 둔다.
	static bool		Load(const string& dataDir, OUT string& error);
	static bool		Parse(const string& csv, OUT string& error);

	// 비어 있는 부위에만 입힌다 (이미 입은 부위는 그대로). equipped 는 Player::equipped - 부위 번호 - 1 이 칸이다.
	// 채운 개수를 돌려준다. 인스턴스 id 는 nextInstanceId 가 준다 (게임 서버 메모리 카운터 - ADR-0004)
	static int32	EquipInto(ItemEntry* equipped, int32 slotCount, const function<uint64()>& nextInstanceId);

	static const vector<StarterKitPiece>& All() { return s_pieces; }

private:
	static vector<StarterKitPiece>	s_pieces;
};
