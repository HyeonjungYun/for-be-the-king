#include "pch.h"
#include "EquipStats.h"
#include "ItemTable.h"
#include "SkillTable.h"

// 장비 스탯 서버 합산 (2026-10-07) - item-equipment-system.md § 스탯 합산 규칙 표 · C3-b · D2-b, combat-system.md Core Rule 2
// 🔴 테스트는 파일을 읽지 않는다 - 실제 Server/Data 의 검증은 부팅이 한다 (ItemTable::Load 실패 = 기동 거부).
namespace
{
	const char* TYPES =
		"id,name_key,equip_slot,archetype,damage_type,grade_mode,attack_range_cm,skill_pool_id\n"
		"101,item.101,1,heavy_weapon,physical,rolled,175,weapon\n"
		"102,item.102,1,light_weapon,physical,rolled,175,weapon\n"
		"103,item.103,1,heavy_weapon,magic,rolled,175,weapon\n"
		"301,item.301,3,helmet_scout,none,rolled,0,helmet\n"
		"302,item.302,3,helmet_guard,none,rolled,0,helmet\n"
		"401,item.401,4,armor_heavy,none,rolled,0,armor\n"
		"501,item.501,5,boots_sprint,none,rolled,0,boots\n"
		"502,item.502,5,boots_endurance,none,rolled,0,boots\n"
		"601,item.601,6,trinket,none,rolled,0,trinket\n";

	const char* POOLS =
		"pool_id,skill_id,weight\n"
		"weapon,1001,1\n"
		"armor,3001,1\n"
		"boots,2001,1\n"
		"trinket,2001,1\n";

	// 실제 Server/Data/item_type_stats.csv 와 같은 수치 (2026-10-07)
	const char* STATS =
		"id,stat,base_stat,flat_floor\n"
		"101,attack_power,4.7059,0\n"
		"102,attack_power,1.4706,0\n"
		"102,crit_chance,0.0735,0\n"
		"102,percent_as_bonus,0.1029,0\n"
		"103,magic_power,4.7059,0\n"
		"301,crit_chance,0.0441,0\n"
		"301,crit_multiplier_bonus,0.2206,0\n"
		"302,resistance,0.1471,0\n"
		"401,max_hp,50,0\n"
		"401,armor,6,0\n"
		"401,magic_resist,6,0\n"
		"501,percent_ms_bonus,0.0441,0\n"
		"502,max_hp,8.8235,0\n"
		"502,resistance,0.0441,0\n";

	const char* GRADES =
		"grade,multiplier\n"
		"1,1.0\n"
		"2,1.3\n"
		"3,1.7\n"
		"4,2.2\n";

	bool Load(const string& stats = STATS, const string& grades = GRADES, string* outError = nullptr);
}

// 강화 레벨 항 (item-enhance-2026-10-12 AC-EN-01 · item-equipment-system.md § D2): 레벨 16 일반 = 레벨 0 전설, 레벨 0 은 예전 그대로
TEST(EquipStats, 레벨_16_일반은_레벨_0_전설과_같고_레벨_0_은_그대로다_AC_EN_01)
{
	ASSERT_TRUE(Load());
	const vector<WornItem> commonMax = { { 101, Protocol::ITEM_GRADE_COMMON, ItemLevel::LEVEL_MAX }, { 401, Protocol::ITEM_GRADE_COMMON, ItemLevel::LEVEL_MAX } };
	const vector<WornItem> legendary0 = { { 101, Protocol::ITEM_GRADE_LEGENDARY, 0 }, { 401, Protocol::ITEM_GRADE_LEGENDARY, 0 } };
	const vector<WornItem> common0 = { { 101, Protocol::ITEM_GRADE_COMMON, 0 }, { 401, Protocol::ITEM_GRADE_COMMON, 0 } };
	const vector<WornItem> commonNoLevel = { { 101, Protocol::ITEM_GRADE_COMMON }, { 401, Protocol::ITEM_GRADE_COMMON } };

	const EquipStatTotals a = EquipStats::Compute(commonMax, 500);
	const EquipStatTotals b = EquipStats::Compute(legendary0, 500);
	EXPECT_NEAR(a.attackPower, b.attackPower, 1);
	EXPECT_NEAR(a.maxHp, b.maxHp, 1);
	EXPECT_NEAR(a.armor, b.armor, 1);
	EXPECT_NEAR(a.magicResist, b.magicResist, 1);

	const EquipStatTotals c = EquipStats::Compute(common0, 500);
	const EquipStatTotals d = EquipStats::Compute(commonNoLevel, 500);
	EXPECT_EQ(c.attackPower, d.attackPower);
	EXPECT_EQ(c.maxHp, d.maxHp);
	EXPECT_LT(c.attackPower, a.attackPower);

	// 레벨은 16 에서 멈춘다 - 그 위는 16 과 같다
	const vector<WornItem> over = { { 101, Protocol::ITEM_GRADE_COMMON, 99 } };
	const vector<WornItem> at = { { 101, Protocol::ITEM_GRADE_COMMON, 16 } };
	EXPECT_EQ(EquipStats::Compute(over, 500).attackPower, EquipStats::Compute(at, 500).attackPower);
	EXPECT_TRUE(ItemLevel::CanEnhance(15));
	EXPECT_FALSE(ItemLevel::CanEnhance(16));
}

namespace
{
	bool Load(const string& stats, const string& grades, string* outError)
	{
		SkillTable::Init();

		string error;
		if (ItemTable::Parse(TYPES, POOLS, OUT error) == false)
		{
			ADD_FAILURE() << "fixture types/pools: " << error;
			return false;
		}

		const bool ok = ItemTable::ParseStats(stats, grades, OUT error);
		if (outError != nullptr)
			*outError = error;
		return ok;
	}

	EquipStatTotals Wear(std::initializer_list<WornItem> items, int32 nakedMaxHp = BaseStats::MAX_HP)
	{
		return EquipStats::Compute(vector<WornItem>(items), nakedMaxHp);
	}

	constexpr Protocol::ItemGrade COMMON = Protocol::ITEM_GRADE_COMMON;
	constexpr Protocol::ItemGrade RARE = Protocol::ITEM_GRADE_RARE;
	constexpr Protocol::ItemGrade LEGENDARY = Protocol::ITEM_GRADE_LEGENDARY;
}

TEST(EquipStats, 아무것도_입지_않으면_맨몸_기준값이다)
{
	ASSERT_TRUE(Load());

	const EquipStatTotals t = Wear({});
	EXPECT_EQ(t.maxHp, 500);
	EXPECT_EQ(t.attackPower, 10);
	EXPECT_EQ(t.magicPower, 0);
	EXPECT_EQ(t.armor, 0);
	EXPECT_EQ(t.magicResist, 0);
	EXPECT_FLOAT_EQ(t.critChance, 0.f);
	EXPECT_FLOAT_EQ(t.critMultiplier, 1.75f);
	EXPECT_FLOAT_EQ(t.resistance, 0.f);
	EXPECT_FLOAT_EQ(t.attackRange, 150.f) << "AC-IE-38 맨몸 사거리";
	EXPECT_EQ(t.autoAttackType, Protocol::DAMAGE_TYPE_PHYSICAL);
}

TEST(EquipStats, 물리_무기는_맨몸_공격력_위에_델타를_더하고_사거리를_공급한다)
{
	ASSERT_TRUE(Load());

	// 10 + 4.7059 × 1.0 = 14.7 → 15 · 10 + 4.7059 × 2.2 = 20.35 → 20
	EXPECT_EQ(Wear({ { 101, COMMON } }).attackPower, 15);
	EXPECT_EQ(Wear({ { 101, LEGENDARY } }).attackPower, 20);
	EXPECT_FLOAT_EQ(Wear({ { 101, COMMON } }).attackRange, 175.f);
	EXPECT_EQ(Wear({ { 101, COMMON } }).magicPower, 0);
}

TEST(EquipStats, AC_IE_04_마법_무기는_오프셋_10을_얹어_물리와_공격_스탯_총합이_같다)
{
	ASSERT_TRUE(Load());

	for (Protocol::ItemGrade grade : { COMMON, RARE, Protocol::ITEM_GRADE_EPIC, LEGENDARY })
	{
		const EquipStatTotals physical = Wear({ { 101, grade } });
		const EquipStatTotals magic = Wear({ { 103, grade } });

		EXPECT_EQ(physical.attackPower, magic.magicPower) << "grade " << grade;
		EXPECT_EQ(magic.attackPower, 10) << "D8 - 마법 무기 착용자에게도 맨주먹 10 은 남는다";
		EXPECT_EQ(magic.autoAttackType, Protocol::DAMAGE_TYPE_MAGIC) << "무기가 평타 타입을 정한다";
	}

	// 벗으면 오프셋도 사라진다
	EXPECT_EQ(Wear({}).magicPower, 0);
}

TEST(EquipStats, 여러_부위의_가산_스탯을_더한다)
{
	ASSERT_TRUE(Load());

	// 중갑 희귀 max_hp 50 × 1.3 = 65 · 지구형 신발 일반 8.8235 → 합 73.8 → 74
	const EquipStatTotals t = Wear({ { 401, RARE }, { 502, COMMON } });
	EXPECT_EQ(t.maxHp, 574);
	EXPECT_EQ(t.armor, 8) << "6 × 1.3 = 7.8 → 8";
	EXPECT_EQ(t.magicResist, 8);

	// 방비형 투구 0.1471 + 지구형 신발 0.0441 = 0.1912
	EXPECT_NEAR(Wear({ { 302, COMMON }, { 502, COMMON } }).resistance, 0.1912f, 1e-4f);

	// 정찰형 투구 크리 배율 보너스 0.2206 → 1.9706
	EXPECT_NEAR(Wear({ { 301, COMMON } }).critMultiplier, 1.9706f, 1e-4f);

	// 맨몸 바닥은 characters.max_hp 다
	EXPECT_EQ(Wear({ { 401, COMMON } }, 600).maxHp, 650);
}

TEST(EquipStats, 확률_스탯은_클램프되고_공속_이속은_아이템당_상한이_있다)
{
	const string synthetic =
		"id,stat,base_stat,flat_floor\n"
		"102,crit_chance,0.6,0\n"
		"102,percent_as_bonus,0.5,0\n"
		"301,crit_chance,0.6,0\n"
		"301,crit_multiplier_bonus,2.0,0\n"
		"302,resistance,0.7,0\n"
		"501,percent_ms_bonus,0.4,0\n"
		"502,resistance,0.7,0\n";
	ASSERT_TRUE(Load(synthetic));

	const EquipStatTotals t = Wear({ { 102, COMMON }, { 301, COMMON }, { 501, COMMON } });
	EXPECT_FLOAT_EQ(t.critChance, 1.f) << "crit_chance [0, 1.0]";
	EXPECT_FLOAT_EQ(t.critMultiplier, 3.f) << "crit_multiplier [1.0, 3.0]";
	EXPECT_FLOAT_EQ(t.percentAsBonus, 0.35f) << "percent_as_bonus 아이템당 0.35";
	EXPECT_FLOAT_EQ(t.percentMsBonus, 0.15f) << "percent_ms_bonus 아이템당 0.15";

	// AC-IE-36 - 두 부위 저항 합 1.4 → 1.0
	EXPECT_FLOAT_EQ(Wear({ { 302, COMMON }, { 502, COMMON } }).resistance, 1.f);
}

TEST(EquipStats, flat_floor_는_등급과_무관하게_앞에_붙는다)
{
	const string withFloor =
		"id,stat,base_stat,flat_floor\n"
		"102,crit_chance,0.0441,0.1\n";
	ASSERT_TRUE(Load(withFloor));

	// D2-b: flat_floor + base_stat × GM
	EXPECT_NEAR(Wear({ { 102, COMMON } }).critChance, 0.1441f, 1e-5f);
	EXPECT_NEAR(Wear({ { 102, LEGENDARY } }).critChance, 0.1f + 0.0441f * 2.2f, 1e-5f);
}

TEST(EquipStats, 표에_없는_종류는_스탯_0으로_건너뛴다)
{
	ASSERT_TRUE(Load());

	const EquipStatTotals t = Wear({ { 999, LEGENDARY }, { 401, COMMON } });
	EXPECT_EQ(t.maxHp, 550);
	EXPECT_EQ(t.attackPower, 10);
}

TEST(EquipStats, 최대_체력이_바뀌면_비율을_유지하고_벗었다_입어도_체력을_벌지_못한다)
{
	// 100 / 558 → 508: 100 × 508 / 558 = 91.04 → 91
	EXPECT_EQ(EquipStats::RescaleHp(100, 558, 508), 91);

	// 다시 입으면 91 × 558 / 508 = 99.95 → 99 - 원래 100 보다 많아지지 않는다
	EXPECT_EQ(EquipStats::RescaleHp(91, 508, 558), 99);

	int32 hp = 237;
	for (int32 i = 0; i < 50; i++)
		hp = EquipStats::RescaleHp(EquipStats::RescaleHp(hp, 565, 500), 500, 565);
	EXPECT_LE(hp, 237) << "벗고 입기를 반복해도 체력이 늘지 않는다";

	EXPECT_EQ(EquipStats::RescaleHp(565, 565, 500), 500) << "만피는 만피";
	EXPECT_EQ(EquipStats::RescaleHp(500, 500, 565), 565);
	EXPECT_EQ(EquipStats::RescaleHp(0, 565, 500), 0) << "죽은 몸은 그대로";
	EXPECT_EQ(EquipStats::RescaleHp(1, 565, 500), 1) << "장비를 벗어서 죽지 않는다";
	EXPECT_EQ(EquipStats::RescaleHp(300, 500, 500), 300);
}

TEST(EquipStats, 마법_평타는_magic_power_로_때리고_magic_resist_로_막힌다)
{
	Creature attacker;
	attacker.attackPower = 10;
	attacker.magicPower = 15;
	attacker.critChance = 0.f;

	Creature target;
	target.armor = 0;
	target.magicResist = 100;		// 50 % 완화

	bool isCrit = true;
	EXPECT_EQ(attacker.ComputeDamageTo(target, Protocol::DAMAGE_TYPE_PHYSICAL, OUT isCrit), 10);
	EXPECT_FALSE(isCrit);
	EXPECT_EQ(attacker.ComputeDamageTo(target, Protocol::DAMAGE_TYPE_MAGIC, OUT isCrit), 7) << "15 × (1 - 100/200) = 7.5 → 7";
}

TEST(ItemTableStats, 잘못된_스탯_데이터는_부팅을_거부한다)
{
	struct Case { const char* name; string stats; string grades; const char* expect; };
	const string H = "id,stat,base_stat,flat_floor\n";

	const Case cases[] =
	{
		{ "모르는 스탯",				H + "101,attack_powr,1,0\n",			GRADES, "attack_powr" },
		{ "표에 없는 종류",			H + "777,armor,1,0\n",					GRADES, "777" },
		{ "마법 무기에 attack_power",	H + "103,attack_power,1,0\n",			GRADES, "magic weapons" },
		{ "방어구에 magic_power",		H + "401,magic_power,1,0\n",			GRADES, "magic weapons" },
		{ "같은 스탯 두 번",			H + "401,armor,1,0\n401,armor,2,0\n",	GRADES, "twice" },
		{ "음수 페널티",				H + "401,armor,-1,0\n",					GRADES, ">= 0" },
		{ "등급 빠짐",				STATS, "grade,multiplier\n1,1.0\n2,1.3\n3,1.7\n",			"grade 4" },
		{ "등급 두 번",				STATS, string(GRADES) + "2,1.4\n",							"twice" },
		{ "배율 0",					STATS, "grade,multiplier\n1,0\n2,1.3\n3,1.7\n4,2.2\n",		"multiplier" },
	};

	for (const Case& c : cases)
	{
		string error;
		EXPECT_FALSE(Load(c.stats, c.grades, &error)) << c.name;
		EXPECT_NE(error.find(c.expect), string::npos) << c.name << ": " << error;
		EXPECT_FLOAT_EQ(ItemTable::GradeMultiplier(LEGENDARY), 1.f) << c.name << " - 실패하면 배율이 남지 않는다";
	}

	// 올바른 데이터로 돌아오면 다시 실린다
	ASSERT_TRUE(Load());
	EXPECT_FLOAT_EQ(ItemTable::GradeMultiplier(LEGENDARY), 2.2f);
	EXPECT_FLOAT_EQ(ItemTable::GradeMultiplier(Protocol::ITEM_GRADE_NONE), 1.f);
	EXPECT_EQ(ItemTable::StatLineCount(), 14u);
}

TEST(ItemTableStats, 사거리는_무기만_공급한다)
{
	SkillTable::Init();

	const string types = string(TYPES) + "303,item.303,3,helmet_scout,none,rolled,50,helmet\n";
	string error;
	EXPECT_FALSE(ItemTable::Parse(types, POOLS, OUT error));
	EXPECT_NE(error.find("attack_range_cm"), string::npos) << error;
}
