#include "pch.h"
#include "StarterKitTable.h"
#include "ItemTable.h"
#include "SkillTable.h"
#include "Player.h"

// 기본 장비 (account-login-mvp-2026-10-09.md Core Rule 8 · ADR-0011 Decision 4 · AC-AL-06 의 서버 쪽).
// 🔴 파일을 읽지 않는다 - 문자열 픽스처를 Parse 에 넣는다. 실제 Server/Data/starter_kit.csv 는 부팅이 검사한다.
namespace
{
	const char* HEADER = "slot,item_type_id,skill_id\n";

	void LoadItems()
	{
		SkillTable::Init();

		string error;
		const string types = string("id,name_key,equip_slot,archetype,damage_type,grade_mode,attack_range_cm,skill_pool_id\n")
			+ "101,item.101,1,heavy_weapon,physical,rolled,175,weapon\n"
			+ "401,item.401,4,armor_heavy,none,rolled,0,armor\n"
			+ "501,item.501,5,boots_sprint,none,rolled,0,boots\n";
		const string pools = string("pool_id,skill_id,weight\n")
			+ "weapon,1001,1\n"
			+ "armor,3001,1\n"
			+ "boots,2001,1\n";

		ASSERT_TRUE(ItemTable::Parse(types, pools, OUT error)) << error;
	}

	string ValidKit()
	{
		return string(HEADER) + "1,101,1001\n" + "4,401,3001\n" + "5,501,2001\n";
	}

	bool ParseKit(const string& csv, OUT string& error)
	{
		LoadItems();
		return StarterKitTable::Parse(csv, OUT error);
	}
}

TEST(StarterKit, 올바른_표는_세_부위를_싣는다)
{
	string error;
	ASSERT_TRUE(ParseKit(ValidKit(), OUT error)) << error;

	ASSERT_EQ(StarterKitTable::All().size(), 3u);
	EXPECT_EQ(StarterKitTable::All()[0].slot, Protocol::SLOT_WEAPON_PRIMARY);
	EXPECT_EQ(StarterKitTable::All()[1].itemTypeId, 401u);
	EXPECT_EQ(StarterKitTable::All()[2].skillId, 2001u);
}

TEST(StarterKit, 그_부위에_입을_수_없는_종류는_거부한다)
{
	string error;
	EXPECT_FALSE(ParseKit(string(HEADER) + "4,101,1001\n", OUT error));		// 무기를 갑옷 칸에
	EXPECT_NE(error.find("cannot be worn"), string::npos) << error;
	EXPECT_TRUE(StarterKitTable::All().empty());
}

TEST(StarterKit, 모르는_종류는_거부한다)
{
	string error;
	EXPECT_FALSE(ParseKit(string(HEADER) + "1,999,1001\n", OUT error));
	EXPECT_NE(error.find("unknown item_type_id"), string::npos) << error;
}

TEST(StarterKit, 종류의_풀에_없는_스킬은_거부한다)
{
	string error;
	EXPECT_FALSE(ParseKit(string(HEADER) + "1,101,3001\n", OUT error));		// 3001 은 갑옷 풀
	EXPECT_NE(error.find("not in pool"), string::npos) << error;
}

TEST(StarterKit, 스킬_0_은_스킬_없는_장비로_받는다)
{
	string error;
	ASSERT_TRUE(ParseKit(string(HEADER) + "1,101,0\n", OUT error)) << error;
	EXPECT_EQ(StarterKitTable::All()[0].skillId, 0u);
}

TEST(StarterKit, 같은_부위가_두_번이면_거부한다)
{
	string error;
	EXPECT_FALSE(ParseKit(string(HEADER) + "1,101,1001\n" + "1,101,1001\n", OUT error));
	EXPECT_NE(error.find("duplicate slot"), string::npos) << error;
}

TEST(StarterKit, 범위_밖_부위는_거부한다)
{
	string error;
	EXPECT_FALSE(ParseKit(string(HEADER) + "0,101,1001\n", OUT error));
	EXPECT_FALSE(ParseKit(string(HEADER) + "7,101,1001\n", OUT error));
}

// 빈 표면 "맨몸으로 들여보낸다" 가 되어 규칙이 조용히 꺼진다
TEST(StarterKit, 빈_표는_기동을_거부한다)
{
	string error;
	EXPECT_FALSE(ParseKit(HEADER, OUT error));
	EXPECT_NE(error.find("no pieces"), string::npos) << error;
}

TEST(StarterKit, 맨몸에는_세_부위를_런_중_생성_일반_등급으로_입힌다)
{
	string error;
	ASSERT_TRUE(ParseKit(ValidKit(), OUT error)) << error;

	ItemEntry equipped[SKILL_SLOT_COUNT];
	uint64 nextId = 900100;

	const int32 filled = StarterKitTable::EquipInto(equipped, SKILL_SLOT_COUNT, [&nextId] { return nextId++; });

	EXPECT_EQ(filled, 3);

	const ItemEntry& weapon = equipped[Protocol::SLOT_WEAPON_PRIMARY - 1];
	EXPECT_EQ(weapon.itemTypeId, 101u);
	EXPECT_EQ(weapon.instanceId, 900100u);
	EXPECT_EQ(weapon.grade, Protocol::ITEM_GRADE_COMMON);
	EXPECT_EQ(weapon.skillIdPrimary, 1001u);
	EXPECT_TRUE(weapon.isNew);		// DB 행은 런 종료 커밋이 만든다

	EXPECT_EQ(equipped[Protocol::SLOT_ARMOR - 1].instanceId, 900101u);
	EXPECT_EQ(equipped[Protocol::SLOT_BOOTS - 1].instanceId, 900102u);
	EXPECT_EQ(equipped[Protocol::SLOT_HELMET - 1].itemTypeId, 0u);		// 표에 없는 부위는 비워 둔다
	EXPECT_EQ(nextId, 900103u);
}

// 봇 재지급(RegearBot)이 이 경로를 쓴다 - 이미 입은 부위는 건드리지 않는다
TEST(StarterKit, 이미_입은_부위는_그대로_두고_빈_부위만_채운다)
{
	string error;
	ASSERT_TRUE(ParseKit(ValidKit(), OUT error)) << error;

	ItemEntry equipped[SKILL_SLOT_COUNT];
	equipped[Protocol::SLOT_WEAPON_PRIMARY - 1].instanceId = 555;
	equipped[Protocol::SLOT_WEAPON_PRIMARY - 1].itemTypeId = 101;
	equipped[Protocol::SLOT_WEAPON_PRIMARY - 1].grade = Protocol::ITEM_GRADE_RARE;

	uint64 nextId = 1;
	EXPECT_EQ(StarterKitTable::EquipInto(equipped, SKILL_SLOT_COUNT, [&nextId] { return nextId++; }), 2);

	EXPECT_EQ(equipped[Protocol::SLOT_WEAPON_PRIMARY - 1].instanceId, 555u);
	EXPECT_EQ(equipped[Protocol::SLOT_WEAPON_PRIMARY - 1].grade, Protocol::ITEM_GRADE_RARE);
	EXPECT_FALSE(equipped[Protocol::SLOT_WEAPON_PRIMARY - 1].isNew);
	EXPECT_EQ(equipped[Protocol::SLOT_ARMOR - 1].instanceId, 1u);
}
