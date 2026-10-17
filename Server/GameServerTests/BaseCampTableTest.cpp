#include "pch.h"
#include "BaseCampTable.h"
#include "WallTable.h"
#include "ItemTable.h"
#include "CsvTable.h"

// 베이스캠프 맵 (2026-10-17) - design/quick-specs/basecamp-map-2026-10-17.md AC-BC-01 ~ 04

namespace
{
	const char* VALID =
		"kind,id,x,y,yaw,half_x,half_y\n"
		"area,camp,-24000,0,0,1500,1500\n"
		"spawn,player,-25000,0,0,0,0\n"
		"dummy,90,-24300,900,180,0,0\n"
		"npc,stash,-23300,-1000,180,70,70\n"
		"npc,enhance,-23300,-200,180,70,70\n"
		"npc,gate,-22750,700,180,70,70\n"
		"prop,anvil,-23300,150,0,60,40\n";

	string Without(const string& csv, const string& line)
	{
		string out = csv;
		const size_t at = out.find(line);
		if (at != string::npos)
			out.erase(at, line.size() + 1);
		return out;
	}
}

// AC-BC-01 배치를 읽고, 바닥 네모 밖으로 못 나가고, NPC · 장식은 길을 막는다
TEST(BaseCampTable, 배치_읽기와_벽)
{
	string error;
	ASSERT_TRUE(BaseCampTable::Parse(VALID, OUT error)) << error;
	EXPECT_TRUE(BaseCampTable::Loaded());
	EXPECT_EQ(BaseCampTable::Npcs().size(), 3u);
	EXPECT_EQ(BaseCampTable::DummyTypeId(), 90u);
	EXPECT_EQ(BaseCampTable::Walls().Count(), 4);		// NPC 3 + 모루

	const WallMap& w = BaseCampTable::Walls();
	EXPECT_TRUE(w.InsideFloor(-25000.f, 0.f));			// 서는 곳
	EXPECT_FALSE(w.InsideFloor(-26000.f, 0.f));			// 바닥 남쪽 밖 (바다)
	EXPECT_FALSE(w.InsideFloor(0.f, 0.f));				// 섬 한가운데는 캠프가 아니다
	EXPECT_TRUE(w.Overlaps(-23300.f, -1000.f, 0.f));	// 창고지기 몸
	EXPECT_TRUE(w.SweepBlocked(-23600.f, -1000.f, -23000.f, -1000.f, 40.f));	// 창고지기를 뚫고 지나갈 수 없다
}

// AC-BC-02 NPC 셋 중 하나라도 없거나 겹치면 부팅 거부
TEST(BaseCampTable, NPC_셋이_다_있어야_한다)
{
	string error;
	EXPECT_FALSE(BaseCampTable::Parse(Without(VALID, "npc,gate,-22750,700,180,70,70"), OUT error));
	EXPECT_FALSE(BaseCampTable::Loaded());
	EXPECT_FALSE(BaseCampTable::Parse(string(VALID) + "npc,stash,-24000,-500,0,70,70\n", OUT error));
	EXPECT_FALSE(BaseCampTable::Parse(string(VALID) + "npc,merchant,-24000,-500,0,70,70\n", OUT error));
}

// AC-BC-03 서는 곳 · 허수아비는 바닥 안이고 장애물과 겹치지 않는다
TEST(BaseCampTable, 서는_곳_검사)
{
	string error;
	string outside = VALID;
	outside.replace(outside.find("spawn,player,-25000"), 19, "spawn,player,-25700");
	EXPECT_FALSE(BaseCampTable::Parse(outside, OUT error));

	string blocked = VALID;
	blocked.replace(blocked.find("spawn,player,-25000,0"), 21, "spawn,player,-23300,-1000");
	EXPECT_FALSE(BaseCampTable::Parse(blocked, OUT error));

	EXPECT_FALSE(BaseCampTable::Parse(string(VALID) + "shop,x,0,0,0,0,0\n", OUT error));
}

// AC-BC-04 실제 데이터 - 캠프가 섬(층 0 해안선)과 겹치지 않는다
TEST(BaseCampTable, 실제_데이터는_섬_밖)
{
	string error;
	string text;
	ASSERT_TRUE(Csv::ReadFile(ItemTable::DefaultDataDir() + "\\basecamp.csv", OUT text));
	ASSERT_TRUE(BaseCampTable::Parse(text, OUT error)) << error;

	ASSERT_TRUE(EntryPointTable::Load(ItemTable::DefaultDataDir(), 4, OUT error)) << error;
	ASSERT_TRUE(WallTable::Load(ItemTable::DefaultDataDir(), 4, EntryPointTable::All(), OUT error)) << error;
	EXPECT_TRUE(WallTable::Get(0).HasOutline());
	EXPECT_TRUE(BaseCampTable::CheckClearOf(WallTable::Get(0), OUT error)) << error;

	// 섬 한가운데에 둔 캠프는 거절
	string onIsland = text;
	onIsland.replace(onIsland.find("area,camp,-24000"), 16, "area,camp,0     ");
	string ignored;
	if (BaseCampTable::Parse(onIsland, OUT ignored))
		EXPECT_FALSE(BaseCampTable::CheckClearOf(WallTable::Get(0), OUT error));
}
