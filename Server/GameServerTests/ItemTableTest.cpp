#include "pch.h"
#include "ItemTable.h"
#include "SkillTable.h"

// 🔴 테스트는 파일을 읽지 않는다 - 문자열 픽스처를 Parse 에 넣는다.
//    실제 Server/Data 의 검증은 부팅이 한다 (ItemTable::Load 실패 = 기동 거부).
namespace
{
	const char* TYPES_HEADER = "id,name_key,equip_slot,archetype,damage_type,grade_mode,attack_range_cm,skill_pool_id\n";
	const char* POOLS_HEADER = "pool_id,skill_id,weight\n";

	string ValidTypes()
	{
		return string(TYPES_HEADER)
			+ "101,item.101,1,heavy_weapon,physical,rolled,175,weapon\n"
			+ "103,item.103,1,heavy_weapon,magic,rolled,175,weapon\n"
			+ "301,item.301,3,helmet_scout,none,rolled,0,helmet\n"
			+ "401,item.401,4,armor_heavy,none,rolled,0,armor\n"
			+ "501,item.501,5,boots_sprint,none,rolled,0,boots\n"
			+ "601,item.601,6,trinket,none,rolled,0,trinket\n";
	}

	string ValidPools()
	{
		return string(POOLS_HEADER)
			+ "weapon,1001,1\n"
			+ "armor,3001,1\n"
			+ "boots,2001,1\n"
			+ "trinket,2001,1\n"
			+ "trinket,3001,1\n";
	}

	bool ParseWith(const string& types, const string& pools, OUT string& error)
	{
		SkillTable::Init();
		return ItemTable::Parse(types, pools, OUT error);
	}
}

TEST(ItemTable, 올바른_데이터는_종류와_풀을_모두_싣는다)
{
	string error;
	ASSERT_TRUE(ParseWith(ValidTypes(), ValidPools(), OUT error)) << error;

	EXPECT_EQ(ItemTable::All().size(), 6u);

	ASSERT_NE(ItemTable::Find(103), nullptr);
	EXPECT_EQ(ItemTable::Find(103)->equipSlot, Protocol::SLOT_WEAPON_PRIMARY);
	EXPECT_EQ(ItemTable::Find(103)->damage, ItemDamage::Magic);

	ASSERT_NE(ItemTable::FindPool("trinket"), nullptr);
	EXPECT_EQ(ItemTable::FindPool("trinket")->size(), 2u);

	// 행이 없는 풀 - 투구는 스킬 없이 나온다 (EB-15). 오류가 아니다
	EXPECT_EQ(ItemTable::FindPool("helmet"), nullptr);
}

TEST(ItemTable, AC_CH_05_종류의_부위와_다른_슬롯에는_착용할_수_없다)
{
	string error;
	ASSERT_TRUE(ParseWith(ValidTypes(), ValidPools(), OUT error)) << error;

	EXPECT_TRUE(ItemTable::CanEquip(101, Protocol::SLOT_WEAPON_PRIMARY));
	EXPECT_TRUE(ItemTable::CanEquip(601, Protocol::SLOT_TRINKET));

	EXPECT_FALSE(ItemTable::CanEquip(101, Protocol::SLOT_WEAPON_SECONDARY)) << "무기는 주무기 칸에만";
	EXPECT_FALSE(ItemTable::CanEquip(401, Protocol::SLOT_HELMET));
	EXPECT_FALSE(ItemTable::CanEquip(999, Protocol::SLOT_WEAPON_PRIMARY)) << "표에 없는 종류";
}

TEST(ItemTable, id_체계와_부위가_다르면_실패하고_표를_비운다)
{
	string error;
	ASSERT_TRUE(ParseWith(ValidTypes(), ValidPools(), OUT error)) << error;

	const string types = ValidTypes() + "105,item.105,3,heavy_weapon,physical,rolled,175,weapon\n";
	EXPECT_FALSE(ParseWith(types, ValidPools(), OUT error));
	EXPECT_NE(error.find("105"), string::npos) << error;

	EXPECT_TRUE(ItemTable::All().empty()) << "실패했는데 이전 표가 남았다";
}

TEST(ItemTable, AC_CH_08_하드CC_스킬이_무기_아닌_풀에_있으면_실패한다)
{
	string error;
	EXPECT_FALSE(ParseWith(ValidTypes(), ValidPools() + "armor,1001,1\n", OUT error));
	EXPECT_NE(error.find("1001"), string::npos) << error;
}

TEST(ItemTable, AC_CH_08_SkillTable에_없는_스킬이면_실패한다)
{
	string error;
	EXPECT_FALSE(ParseWith(ValidTypes(), ValidPools() + "boots,9999,1\n", OUT error));
	EXPECT_NE(error.find("9999"), string::npos) << error;
}

TEST(ItemTable, 가중치_0은_실패한다)
{
	string error;
	EXPECT_FALSE(ParseWith(ValidTypes(), ValidPools() + "armor,2001,0\n", OUT error));
	EXPECT_NE(error.find("weight"), string::npos) << error;
}

TEST(ItemTable, 어떤_종류도_쓰지_않는_풀은_오타로_보고_실패한다)
{
	string error;
	EXPECT_FALSE(ParseWith(ValidTypes(), ValidPools() + "armr,2001,1\n", OUT error));
	EXPECT_NE(error.find("armr"), string::npos) << error;
}

TEST(ItemTable, 장신구_풀에는_방어구_풀에_있는_스킬만_들어간다)
{
	// 2001 이 신발 풀에 없다 - 장신구가 방어구 어디에도 없는 스킬을 갖게 된다
	const string pools = string(POOLS_HEADER)
		+ "weapon,1001,1\n"
		+ "armor,3001,1\n"
		+ "trinket,2001,1\n";

	string error;
	EXPECT_FALSE(ParseWith(ValidTypes(), pools, OUT error));
	EXPECT_NE(error.find("trinket"), string::npos) << error;
}

TEST(ItemTable, 무기만_물리_마법을_가진다)
{
	string error;
	const string types = ValidTypes() + "402,item.402,4,armor_light,magic,rolled,0,armor\n";
	EXPECT_FALSE(ParseWith(types, ValidPools(), OUT error));
	EXPECT_NE(error.find("damage_type"), string::npos) << error;
}
