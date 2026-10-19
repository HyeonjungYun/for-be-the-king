#include "pch.h"
#include "BotLoadoutTable.h"
#include "ItemTable.h"
#include "SkillTable.h"
#include "Player.h"

// 봇 스킬 셋 (design/quick-specs/bot-loadouts-2026-10-20.md AC-BL-01~05 의 서버 쪽).
// 🔴 파일을 읽지 않는다 - 문자열 픽스처를 Parse 에 넣는다. 실제 Server/Data/bot_loadouts.csv 는 부팅이 검사한다.
namespace
{
	const char* HEADER = "build_id,code,role,weapon_type,shift_skill,q_skill,helmet_type,e_skill,armor_type,r_skill,boots_type,z_skill,trinket_type,x_skill,weight\n";

	void LoadItems()
	{
		// 픽스처 스킬은 실제 로스터 id (1007 · 1046 ...) - 코드 픽스처(Init)에는 없다
		string error;
		ASSERT_TRUE(SkillTable::Load(ItemTable::DefaultDataDir(), OUT error)) << error;
		const string types = string("id,name_key,equip_slot,archetype,damage_type,grade_mode,attack_range_cm,skill_pool_id\n")
			+ "101,item.101,1,weapon_sword,physical,rolled,175,sword\n"
			+ "105,item.105,1,weapon_bow,physical,rolled,900,bow\n"
			+ "302,item.302,3,helmet_guard,none,rolled,0,helmet\n"
			+ "401,item.401,4,armor_heavy,none,rolled,0,armor\n"
			+ "501,item.501,5,boots_sprint,none,rolled,0,boots\n"
			+ "601,item.601,6,trinket,none,rolled,0,trinket\n";
		const string pools = string("pool_id,skill_id,weight\n")
			+ "sword,1007,1\n" + "sword,1015,1\n"
			+ "bow,1046,1\n" + "bow,1047,1\n"
			+ "helmet,1094,1\n" + "helmet,1002,1\n"		// 장신구 스킬은 다른 부위 풀에도 있어야 한다 (ItemTable 규칙)
			+ "armor,1118,1\n"
			+ "boots,1165,1\n"
			+ "trinket,1002,1\n";

		ASSERT_TRUE(ItemTable::Parse(types, pools, OUT error)) << error;
	}

	string SwordRow(int id = 1, int weight = 1)
	{
		return to_string(id) + ",S-2,duel,101,1007,1015,302,1094,401,1118,501,1165,601,1002," + to_string(weight) + "\n";
	}

	string BowRow(int id = 2, int weight = 1)
	{
		return to_string(id) + ",B-5,runner,105,1047,1046,0,0,0,0,501,1165,601,1002," + to_string(weight) + "\n";
	}

	bool ParseTable(const string& csv, OUT string& error)
	{
		LoadItems();
		return BotLoadoutTable::Parse(csv, OUT error);
	}
}

TEST(BotLoadout, 올바른_표는_빌드마다_부위를_싣는다)
{
	string error;
	ASSERT_TRUE(ParseTable(string(HEADER) + SwordRow() + BowRow(), OUT error)) << error;

	ASSERT_EQ(BotLoadoutTable::All().size(), 2u);
	const BotLoadout& sword = BotLoadoutTable::All()[0];
	EXPECT_EQ(sword.code, "S-2");
	EXPECT_EQ(sword.role, "duel");
	EXPECT_EQ(sword.pieces.size(), 5u);
	EXPECT_EQ(sword.WeaponTypeId(), 101u);
	EXPECT_EQ(sword.pieces[0].skillIdSecondary, 1015u);
	// 종류 0 인 부위(모자 · 갑옷)는 싣지 않는다 - 비워 둔다
	EXPECT_EQ(BotLoadoutTable::All()[1].pieces.size(), 3u);
}

TEST(BotLoadout, 종류의_풀에_없는_스킬은_거부한다)
{
	string error;
	// 1046 은 활 풀 - 검에 붙일 수 없다
	EXPECT_FALSE(ParseTable(string(HEADER) + "1,S-x,duel,101,1007,1046,0,0,0,0,0,0,0,0,1\n", OUT error));
	EXPECT_NE(error.find("not in pool"), string::npos) << error;
	EXPECT_TRUE(BotLoadoutTable::All().empty());
}

TEST(BotLoadout, 부위에_맞지_않는_종류_모르는_성격_같은_번호는_거부한다)
{
	string error;
	EXPECT_FALSE(ParseTable(string(HEADER) + "1,S-x,duel,101,1007,0,401,0,0,0,0,0,0,0,1\n", OUT error));		// 갑옷을 모자 칸에
	EXPECT_NE(error.find("cannot be worn"), string::npos) << error;
	EXPECT_FALSE(ParseTable(string(HEADER) + "1,S-x,berserk,101,1007,0,0,0,0,0,0,0,0,0,1\n", OUT error));
	EXPECT_NE(error.find("role"), string::npos) << error;
	EXPECT_FALSE(ParseTable(string(HEADER) + SwordRow(1) + BowRow(1), OUT error));
	EXPECT_NE(error.find("unique"), string::npos) << error;
	EXPECT_FALSE(ParseTable(string(HEADER) + "1,S-x,duel,0,0,0,0,0,0,0,0,0,0,0,1\n", OUT error));
	EXPECT_NE(error.find("weapon_type"), string::npos) << error;
	EXPECT_FALSE(ParseTable(string(HEADER) + SwordRow(1, 0), OUT error));
	EXPECT_NE(error.find("weight"), string::npos) << error;
}

TEST(BotLoadout, 무게대로_고르고_무기가_있으면_그_무기의_빌드만)
{
	string error;
	ASSERT_TRUE(ParseTable(string(HEADER) + SwordRow(1, 1) + BowRow(2, 3), OUT error)) << error;

	// 무게 합 4 - roll 0 은 검, 1..3 은 활, 4 는 다시 검 (나머지)
	EXPECT_EQ(BotLoadoutTable::Pick(0, 0)->code, "S-2");
	EXPECT_EQ(BotLoadoutTable::Pick(0, 1)->code, "B-5");
	EXPECT_EQ(BotLoadoutTable::Pick(0, 3)->code, "B-5");
	EXPECT_EQ(BotLoadoutTable::Pick(0, 4)->code, "S-2");

	// 활을 들고 있으면 roll 과 무관하게 활 빌드
	for (uint64 roll = 0; roll < 10; roll++)
		EXPECT_EQ(BotLoadoutTable::Pick(105, roll)->code, "B-5");

	// 표에 없는 무기 - nullptr (호출자가 아무 빌드로 다시 고른다)
	EXPECT_EQ(BotLoadoutTable::Pick(104, 0), nullptr);
}

TEST(BotLoadout, 비어_있는_부위만_채우고_입은_부위는_그대로다)
{
	string error;
	ASSERT_TRUE(ParseTable(string(HEADER) + SwordRow(), OUT error)) << error;

	ItemEntry equipped[SKILL_SLOT_COUNT];
	equipped[Protocol::SLOT_BOOTS - 1].instanceId = 77;
	equipped[Protocol::SLOT_BOOTS - 1].itemTypeId = 502;		// 이미 신은 신발

	uint64 next = 1000;
	const int32 filled = BotLoadoutTable::EquipInto(BotLoadoutTable::All()[0], equipped, SKILL_SLOT_COUNT, [&] { return next++; });

	EXPECT_EQ(filled, 4);
	const ItemEntry& weapon = equipped[Protocol::SLOT_WEAPON_PRIMARY - 1];
	EXPECT_EQ(weapon.itemTypeId, 101u);
	EXPECT_EQ(weapon.skillIdPrimary, 1007u);
	EXPECT_EQ(weapon.skillIdSecondary, 1015u);
	EXPECT_EQ(weapon.grade, Protocol::ITEM_GRADE_COMMON);
	EXPECT_TRUE(weapon.isNew);		// DB 행 없음 - 런 종료 커밋이 INSERT
	EXPECT_FALSE(weapon.isTest);
	EXPECT_EQ(equipped[Protocol::SLOT_HELMET - 1].skillIdPrimary, 1094u);
	EXPECT_EQ(equipped[Protocol::SLOT_TRINKET - 1].skillIdPrimary, 1002u);
	EXPECT_EQ(equipped[Protocol::SLOT_BOOTS - 1].instanceId, 77u);		// 그대로
	EXPECT_EQ(equipped[Protocol::SLOT_BOOTS - 1].itemTypeId, 502u);
	EXPECT_EQ(equipped[Protocol::SLOT_WEAPON_SECONDARY - 1].itemTypeId, 0u);	// Q 는 무기의 둘째 스킬 - 칸 2 에 아이템을 만들지 않는다
	EXPECT_EQ(next, 1004u);
}

TEST(BotLoadout, 실제_데이터_표의_빌드는_35개이고_무기마다_다섯이다)
{
	// 실제 Server/Data 를 읽는다 - 부팅 검사와 같은 경로 (ItemTable 다음). 파일 I/O 예외: 표 자체가 검사 대상
	string error;
	const string dir = ItemTable::DefaultDataDir();
	ASSERT_TRUE(SkillTable::Load(dir, OUT error)) << error;
	ASSERT_TRUE(ItemTable::Load(dir, OUT error)) << error;
	ASSERT_TRUE(BotLoadoutTable::Load(dir, OUT error)) << error;
	ASSERT_EQ(BotLoadoutTable::All().size(), 35u);
	map<uint32, int32> perWeapon;
	for (const BotLoadout& l : BotLoadoutTable::All())
		perWeapon[l.WeaponTypeId()]++;
	EXPECT_EQ(perWeapon.size(), 7u);
	for (const auto& [weapon, n] : perWeapon)
		EXPECT_EQ(n, 5) << "weapon " << weapon;
}
