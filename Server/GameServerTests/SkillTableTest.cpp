#include "pch.h"
#include "SkillTable.h"
#include "ItemTable.h"

// 🔴 이 파일은 실제 Server/Data 를 읽는다 - 스킬 로스터(skills.csv · skill_effects.csv · skill_rules.csv · skill_pools.csv)의 린트다.
//    수치를 고치면 여기서 먼저 걸린다. 부팅도 같은 검사를 하지만, 부팅은 서버를 띄워야 알 수 있다 (SK-32 는 부팅에 없다).
namespace
{
	bool LoadRealData(OUT string& error)
	{
		const string dir = ItemTable::DefaultDataDir();
		return SkillTable::Load(dir, OUT error) && ItemTable::Load(dir, OUT error);
	}

	// 어느 풀이 이 스킬을 담는가
	vector<string> PoolsOf(uint32 skillId)
	{
		vector<string> out;
		for (const auto& [typeId, def] : ItemTable::All())
		{
			const vector<SkillPoolEntry>* pool = ItemTable::FindPool(def.skillPoolId);
			if (pool == nullptr)
				continue;
			for (const SkillPoolEntry& e : *pool)
			{
				if (e.skillId == skillId && find(out.begin(), out.end(), def.skillPoolId) == out.end())
					out.push_back(def.skillPoolId);
			}
		}
		return out;
	}
}

TEST(SkillRoster, 실제_데이터가_읽히고_76개_전부_있다)
{
	string error;
	ASSERT_TRUE(LoadRealData(OUT error)) << error;

	// src/스킬_목록.csv 게임 ID 1002 ~ 1189 (1001 채찍 제외 · 1155 는 목록에서 빠졌다). 9001 은 몬스터
	for (uint32 n = 2; n <= 189; n++)
	{
		if (n == 155)
			continue;
		EXPECT_NE(SkillTable::Find(1000 + n), nullptr) << "skill " << n << " is missing";
	}

	EXPECT_NE(SkillTable::Find(9001), nullptr);
	EXPECT_NE(SkillTable::Find(9002), nullptr);		// 멧돼지 돌진 (monster-ai.md § L, 2026-10-17)
	EXPECT_NE(SkillTable::Find(9003), nullptr);		// 허깨비 화살
	EXPECT_EQ(SkillTable::Find(1001), nullptr) << "채찍(#1)은 구현하지 않는다";
	EXPECT_EQ(SkillTable::Find(1155), nullptr) << "1155 는 스킬 목록에 없다";
	// 76 + 몬스터 9001 + 모자 27 (#78 ~ #104) + 모자 1 · 갑옷 9 (#105 ~ #114, 2026-10-10 오후) + 2차 로스터 23 (1115 ~ 1137, 2026-10-11)
	// + 3차 로스터 51 (1138 ~ 1189 - 1155, 2026-10-11 밤) + 몬스터 9002 · 9003 (2026-10-17)
	EXPECT_EQ(SkillTable::All().size(), 190u);
}

// 🔴 갑옷 로스터 #106 ~ #114 - "망치 / 검을 들고 있을 때만" 은 skills.csv requires_weapon 열 (Room::MeetsWeaponRequirement 가 본다)
TEST(SkillRoster, 갑옷_스킬은_무기_조건을_가진다)
{
	string error;
	ASSERT_TRUE(LoadRealData(OUT error)) << error;

	for (uint32 id = 1106; id <= 1111; id++)
	{
		const SkillDef* def = SkillTable::Find(id);
		ASSERT_NE(def, nullptr) << id;
		EXPECT_EQ(def->requiresWeapon, "hammer") << id;
	}
	for (uint32 id = 1112; id <= 1114; id++)
	{
		const SkillDef* def = SkillTable::Find(id);
		ASSERT_NE(def, nullptr) << id;
		EXPECT_EQ(def->requiresWeapon, "sword") << id;
	}
	EXPECT_EQ(SkillTable::Find(1113)->requiresWeaponTag, "ghost") << "백귀야행은 원혼 스킬이 있는 검";
	EXPECT_TRUE(SkillTable::Find(1113)->HasHardCc()) << "백귀야행 기절 - 무기 조건부라 갑옷 풀에서 허용";

	// 무기 조건은 갑옷 · 모자 풀에만 - 무기 풀 스킬은 이미 그 무기를 들고 있고, 조건 없는 스킬은 어느 무기로도 쓴다.
	// 🔸 2026-10-11 사용자 결정 - 오버로드(1073) · 니힐(1074) 를 스태프 풀에서 모자 풀로 옮기며 "스태프를 들고 있을 때만"
	for (const auto& [id, def] : SkillTable::All())
	{
		if (def.requiresWeapon.empty() == false)
		{
			const vector<string> pools = PoolsOf(id);
			EXPECT_TRUE(pools == vector<string>{ "armor" } || pools == vector<string>{ "helmet" })
				<< "skill " << id << " has requires_weapon=" << def.requiresWeapon;
		}
	}
	for (uint32 id : { 1073u, 1074u })
	{
		EXPECT_EQ(SkillTable::Find(id)->requiresWeapon, "staff") << id;
		EXPECT_EQ(PoolsOf(id), vector<string>{ "helmet" }) << id;
	}

	// 2차 로스터 (2026-10-11) - 갑옷 스킬은 전부 "○○을 들고 있을 때만", 바위 날리기(무기) · 천리안(모자)은 조건 없음
	const unordered_map<string, vector<uint32>> byWeapon = {
		{ "sword",	{ 1115, 1117, 1118, 1119 } },
		{ "hammer",	{ 1116, 1122 } },
		{ "dagger",	{ 1120, 1121, 1124, 1125, 1126, 1127, 1128, 1129, 1189 } },
		{ "bow",	{ 1130, 1131, 1133, 1134, 1135, 1136, 1137 } },
		// 3차 로스터 (2026-10-11) - 창 갑옷 10 · 스태프 갑옷 9
		{ "spear",	{ 1138, 1139, 1140, 1141, 1142, 1143, 1144, 1145, 1146, 1147 } },
		{ "staff",	{ 1149, 1150, 1151, 1152, 1153, 1154, 1156, 1157, 1158 } },
	};
	for (const auto& [weapon, ids] : byWeapon)
	{
		for (uint32 id : ids)
		{
			ASSERT_NE(SkillTable::Find(id), nullptr) << id;
			EXPECT_EQ(SkillTable::Find(id)->requiresWeapon, weapon) << id;
			EXPECT_TRUE(SkillTable::Find(id)->HasTag(weapon)) << id;
		}
	}
	EXPECT_TRUE(SkillTable::Find(1123)->requiresWeapon.empty());
	EXPECT_TRUE(SkillTable::Find(1132)->requiresWeapon.empty());
	EXPECT_EQ(PoolsOf(1123), vector<string>{ "hammer" });
	EXPECT_EQ(PoolsOf(1132), vector<string>{ "helmet" });

	// 3차 로스터 - 글라키에스는 스태프 무기 스킬(조건 없음), 신발 30 은 신발 풀에만 · 조건 없음
	EXPECT_TRUE(SkillTable::Find(1148)->requiresWeapon.empty());
	EXPECT_EQ(PoolsOf(1148), vector<string>{ "staff" });
	for (uint32 id = 1159; id <= 1188; id++)
	{
		ASSERT_NE(SkillTable::Find(id), nullptr) << id;
		EXPECT_TRUE(SkillTable::Find(id)->requiresWeapon.empty()) << id;
		EXPECT_EQ(PoolsOf(id), vector<string>{ "boots" }) << id;
	}

	// 건곤일척(damage_mod tag:hammer) 이 보는 태그 - 망치 풀 스킬 전부 + 갑옷 망치 스킬
	for (uint32 id : { 1005u, 1006u, 1026u, 1028u, 1030u, 1031u, 1032u, 1033u, 1034u, 1106u, 1107u, 1108u, 1109u, 1110u, 1116u, 1122u, 1123u })
		EXPECT_TRUE(SkillTable::Find(id)->HasTag("hammer")) << id;
	EXPECT_FALSE(SkillTable::Find(1005)->HasTag("sword")) << "머리 부수기는 망치 스킬 (모자 단계에서 잘못 붙은 sword 태그)";
	EXPECT_FALSE(SkillTable::Find(1006)->HasTag("sword")) << "흉갑 부수기는 망치 스킬";
}

// 🔴 유령 신랑의 저주(#2)만 벽을 지나는 이동이다 - ignores_walls 열은 2026-10-10 오후까지 읽기만 하고 쓰는 곳이 없었다
//    (Room::ApplyMovement 가 창을 열고 HandleMove 벽 검사를 건너뛴다 · 클라 TickDash 는 스윕 없이). 다른 이동 스킬에 번지면 안 된다
TEST(SkillRoster, 벽을_지나는_이동은_유령_신랑의_저주뿐이다)
{
	string error;
	ASSERT_TRUE(LoadRealData(OUT error)) << error;

	const SkillDef* curse = SkillTable::Find(1002);
	ASSERT_NE(curse, nullptr);
	const SkillEffect* move = curse->FindEffect(SkillEffectType::Movement);
	ASSERT_NE(move, nullptr);
	EXPECT_TRUE(move->ignoresWalls) << "1002 move effect must carry ignores_walls=1";

	// 블링크(1075)는 서버가 옮기는 이동이라 열이 1 이어도 C_MOVE 창을 열지 않는다 (BlinkCaster) - 여기서 제외.
	// to_anchor(비창 · 포르타 재입력, 2026-10-11)도 서버가 옮기는 순간이동이다 (TeleportToAnchor)
	for (const auto& [id, def] : SkillTable::All())
	{
		for (const SkillEffect& e : def.effects)
		{
			if (e.ignoresWalls && e.param != "blink" && e.param != "to_anchor")
				EXPECT_EQ(id, 1002u) << "skill " << id << " effect param=" << e.param << " has ignores_walls=1";
		}
	}
}

// 🔴 하드 CC 는 무기 부위 전용 - 캐릭터당 1개가 AC-5(4인 체인 CC ≤ 5초) 예산의 전제다 (ItemTable::Parse 가 부팅에서도 막는다)
TEST(SkillRoster, 하드CC_스킬은_무기_풀에만_있다)
{
	string error;
	ASSERT_TRUE(LoadRealData(OUT error)) << error;

	const set<string> weaponPools = { "sword", "hammer", "spear", "dagger", "bow", "knuckle", "staff" };
	int32 hardCcCount = 0;

	for (const auto& [skillId, def] : SkillTable::All())
	{
		if (def.HasHardCc() == false)
			continue;
		++hardCcCount;

		// 🔴 2026-10-10 갑옷 로스터 - requires_weapon 이 있는 스킬(백귀야행 #113)은 무기에 묶인 셈이라 갑옷 풀에도 허용 (ItemTable::Parse 와 같은 규칙).
		//    AC-5 예산은 "무기 1 + 무기 조건부 갑옷 1" 까지로 넓어졌다 - quick-spec § 갑옷 로스터
		if (def.requiresWeapon.empty() == false)
			continue;

		for (const string& pool : PoolsOf(skillId))
			EXPECT_TRUE(weaponPools.count(pool) > 0) << "skill " << skillId << " (hard CC) is in non-weapon pool " << pool;
	}

	EXPECT_GT(hardCcCount, 0);
}

// skill-system.md § S1 — 하드 CC 유형별 상한. AC-5 예산은 전부 이 천장으로 계산했다 (Parse 도 막지만 수치를 여기 적어 둔다)
TEST(SkillRoster, AC5_하드CC_지속시간은_유형별_상한을_넘지_않는다)
{
	string error;
	ASSERT_TRUE(LoadRealData(OUT error)) << error;

	const unordered_map<Protocol::CcType, uint32> capMs = {
		{ Protocol::CC_TYPE_STUN,	1250 },
		{ Protocol::CC_TYPE_LAUNCH,	1000 },
		{ Protocol::CC_TYPE_ROOT,	1750 },
	};

	uint32 worstIncapacitationMs = 0;

	for (const auto& [skillId, def] : SkillTable::All())
	{
		uint32 incapacitationMs = 0;

		for (const SkillEffect& e : def.effects)
		{
			if (e.type != SkillEffectType::HardCc)
				continue;

			auto it = capMs.find(e.ccType);
			ASSERT_NE(it, capMs.end()) << "skill " << skillId;
			EXPECT_LE(e.durationMs, it->second) << "skill " << skillId;

			if (e.ccType != Protocol::CC_TYPE_ROOT)
				incapacitationMs = max(incapacitationMs, e.durationMs);
		}

		worstIncapacitationMs = max(worstIncapacitationMs, incapacitationMs);
	}

	EXPECT_LE(worstIncapacitationMs * 4, 5000u) << "가장 긴 하드 CC 로 4인 릴레이하면 5초를 넘는다";
}

// 무기 풀마다 하드 CC 가 아닌 스킬이 둘 이상 - E2 재추첨(Shift 가 하드 CC 면 Q 는 비CC 풀에서)이 늘 가능해야 한다
TEST(SkillRoster, 무기_풀마다_비CC_스킬이_둘_이상이다)
{
	string error;
	ASSERT_TRUE(LoadRealData(OUT error)) << error;

	for (const char* poolId : { "sword", "hammer", "spear", "dagger", "bow", "knuckle", "staff" })
	{
		const vector<SkillPoolEntry>* pool = ItemTable::FindPool(poolId);
		ASSERT_NE(pool, nullptr) << poolId;

		int32 nonCc = 0;
		for (const SkillPoolEntry& e : *pool)
		{
			const SkillDef* def = SkillTable::Find(e.skillId);
			ASSERT_NE(def, nullptr);
			if (def->HasHardCc() == false)
				nonCc++;
		}
		EXPECT_GE(nonCc, 2) << poolId;
		EXPECT_GE(pool->size(), 4u) << poolId << " 는 Shift · Q 를 다르게 뽑을 만큼 커야 한다";
	}
}

// SK-32 — 즉발기 버스트 상한. 슬롯마다 cast_ms = 0 인 스킬의 (ad + ap) 최댓값을 더해 10.0 을 넘지 않는다 (S5)
TEST(SkillRoster, SK32_즉발기_계수_합은_10을_넘지_않는다)
{
	string error;
	ASSERT_TRUE(LoadRealData(OUT error)) << error;

	// 키 → 그 키에 올 수 있는 풀. 무기 키 둘은 무기 풀 전부, 나머지는 그 부위 풀
	const vector<vector<string>> keyPools =
	{
		{ "sword", "hammer", "spear", "dagger", "bow", "knuckle", "staff" },	// Shift
		{ "sword", "hammer", "spear", "dagger", "bow", "knuckle", "staff" },	// Q
		{ "helmet" }, { "armor" }, { "boots" }, { "trinket" },
	};

	float total = 0.f;
	for (const vector<string>& pools : keyPools)
	{
		float best = 0.f;
		for (const string& poolId : pools)
		{
			const vector<SkillPoolEntry>* pool = ItemTable::FindPool(poolId);
			if (pool == nullptr)
				continue;

			for (const SkillPoolEntry& e : *pool)
			{
				const SkillDef* def = SkillTable::Find(e.skillId);
				if (def == nullptr || def->castMs != 0)
					continue;

				// 연사(volley)는 피해 효과가 한 발 몫이다 - 기본 발 수만큼 곱한다 (그림자 퍼붓기 6 발, 2026-10-11)
				const SkillEffect* volley = def->FindEffect(SkillEffectType::Volley);
				const uint32 shots = (volley != nullptr) ? max(1u, volley->hits) : 1u;

				float ratio = 0.f;
				for (const SkillEffect& fx : def->effects)
				{
					if (fx.type == SkillEffectType::Damage && fx.condition == SkillRequire::None)
						ratio += (fx.adRatio + fx.apRatio) * max(1u, fx.hits) * shots;
				}
				best = max(best, ratio);
			}
		}
		total += best;
	}

	EXPECT_LE(total, 10.f) << "instant burst ratio sum";
}

// 모든 스킬에 효과가 있고, 활성 스킬은 조준 · 모양이 맞물린다 (Parse 가 검사하지만 데이터 변경 때 여기서 먼저 보인다)
TEST(SkillRoster, 모든_스킬에_효과가_하나_이상_있다)
{
	string error;
	ASSERT_TRUE(LoadRealData(OUT error)) << error;

	for (const auto& [skillId, def] : SkillTable::All())
	{
		EXPECT_FALSE(def.effects.empty()) << "skill " << skillId;
		EXPECT_EQ(def.effectId, skillId);
		EXPECT_EQ(def.nameKey, "skill." + to_string(skillId));
	}
}

// 테스트 픽스처(Init)는 예전 네 스킬 그대로다 - 아이템 · 롤러 · 장비 테스트의 전제
TEST(SkillTable, 테스트_픽스처는_네_스킬을_싣는다)
{
	SkillTable::Init();
	EXPECT_EQ(SkillTable::All().size(), 4u);

	const SkillDef* stun = SkillTable::Find(1001);
	ASSERT_NE(stun, nullptr);
	EXPECT_TRUE(stun->HasHardCc());
	EXPECT_EQ(stun->allowedSlot, Protocol::SLOT_WEAPON_PRIMARY);

	const SkillDef* dash = SkillTable::Find(2001);
	ASSERT_NE(dash, nullptr);
	EXPECT_TRUE(dash->HasMovement());
	EXPECT_EQ(dash->FindEffect(SkillEffectType::Movement)->distCm, 400.f);
}

// 로더가 막아야 하는 것 - 상한 넘는 기절 · 조준 없는 캐스트 · 넉백 운동학 · 모르는 효과
TEST(SkillTable, 잘못된_데이터는_거부한다)
{
	const string header = "skill_id,name_key,power,kind,cast_ms,cooldown_ms,can_move,telegraph_ms,shape,aim,range_cm,width_cm,radius_cm,angle_deg,allowed_slot,projectile_speed_cms,pierce\n";
	const string eheader = "skill_id,seq,effect,who,requires,ad_ratio,ap_ratio,hits,interval_ms,cc_type,duration_ms,magnitude,dist_cm,speed_cms,ignores_walls,stat,per_level,radius_cm,param\n";
	const string rules = "key,value\ndot_tick_ms,500\ndot_max_stacks,5\nwall_stun_ms,750\nrecast_window_ms,2000\nexecute_hp_pct,15\nblind_view_scale,0.5\n"
		"pull_stop_cm,100\npull_speed_cms,1600\ndecoy_hp,1\nprojectile_radius_cm,40\nbasic_projectile_speed_cms,2400\nconfront_range_cm,400\n"
		"mark_spear_basic_bonus,0.5\nchain_radius_cm,300\ndelayed_burst_ms,1500\nspin_max_ms,3000\nhold_max_ms,0\nbuff_per_level_default,0.03\ntrail_buff_ms,2000\ntrail_pickup_cm,80\ntaunt_speed_cms,150\ntaunt_stop_cm,100\nlong_range_cm,500\nbasic_dash_speed_cms,1200\n";
	string error;

	// 기절 1300 > 1250
	EXPECT_FALSE(SkillTable::Parse(header + "1,skill.1,ad,active,300,8000,0,0,single,target,400,0,0,0,0,0,0\n",
		eheader + "1,1,hard_cc,target,none,0,0,0,0,stun,1300,0,0,0,0,none,0,0,none\n", rules, OUT error));
	EXPECT_NE(error.find("S1 cap"), string::npos) << error;

	// 조준 없는 캐스트 (R12b)
	EXPECT_FALSE(SkillTable::Parse(header + "1,skill.1,ad,active,300,8000,0,0,none,none,0,0,0,0,0,0,0\n",
		eheader + "1,1,buff,self,none,0,0,0,0,none,5000,0.1,0,0,0,attack_power,0,0,none\n", rules, OUT error));
	EXPECT_NE(error.find("R12b"), string::npos) << error;

	// 넉백 300 cm 를 450 cm/s 로 - 0.667 s, dash_duration 상한 위반 (S2)
	EXPECT_FALSE(SkillTable::Parse(header + "1,skill.1,ad,active,300,8000,0,0,single,target,400,0,0,0,0,0,0\n",
		eheader + "1,1,knockback,target,none,0,0,0,0,knockback,0,0,300,450,0,none,0,0,none\n", rules, OUT error));
	EXPECT_NE(error.find("0.4"), string::npos) << error;

	// 모르는 효과 이름
	EXPECT_FALSE(SkillTable::Parse(header + "1,skill.1,ad,active,300,8000,0,0,single,target,400,0,0,0,0,0,0\n",
		eheader + "1,1,teleport,target,none,0,0,0,0,none,0,0,0,0,0,none,0,0,none\n", rules, OUT error));

	// 이동 스킬인데 즉발 (cast 0)
	EXPECT_FALSE(SkillTable::Parse(header + "1,skill.1,ad,active,0,8000,0,0,circle_self,direction,400,0,0,0,0,0,0\n",
		eheader + "1,1,move,self,none,0,0,0,0,none,0,0,400,1200,0,none,0,0,dash\n", rules, OUT error));
	EXPECT_NE(error.find("cast_ms must be >= 60"), string::npos) << error;

	// 규칙 키 누락
	EXPECT_FALSE(SkillTable::Parse(header + "1,skill.1,ad,active,300,8000,0,0,single,target,400,0,0,0,0,0,0\n",
		eheader + "1,1,damage,target,none,1,0,1,0,none,0,0,0,0,0,none,0,0,none\n", "key,value\ndot_tick_ms,500\n", OUT error));
	EXPECT_NE(error.find("skill_rules.csv"), string::npos) << error;

	EXPECT_TRUE(SkillTable::All().empty());
}

// 1 단계 - 종류(kind)와 효과가 맞물리는가. 패시브는 평타 적중 · 치명상 효과로만 발동하고, 재입력은 두 번째 입력의 효과가 있어야 한다
TEST(SkillRoster, 패시브는_on_hit_또는_lethal_save_를_가지고_재입력은_recast_효과를_가진다)
{
	string error;
	ASSERT_TRUE(LoadRealData(OUT error)) << error;

	int32 passives = 0;
	int32 recasts = 0;

	for (const auto& [skillId, def] : SkillTable::All())
	{
		if (def.kind == SkillKind::Passive)
		{
			++passives;
			// 패시브가 발동하는 길: 평타 적중 · 치명상 · 면역 · 피해 배율 · 첫 타 경감 · 트리거 조건(on_dot / on_dash / on_skill)
			// + 3차 로스터 (2026-10-11): 장착 스탯 · CC 저항 · 트리거(on_stance / idle_combat / on_damaged / on_kill / on_move_end / low_hp)
			bool hasTrigger = def.FindEffect(SkillEffectType::OnHit) != nullptr || def.FindEffect(SkillEffectType::LethalSave) != nullptr
				|| def.FindEffect(SkillEffectType::Immune) != nullptr || def.FindEffect(SkillEffectType::DamageMod) != nullptr
				|| def.FindEffect(SkillEffectType::DamageReduceOnce) != nullptr || def.FindEffect(SkillEffectType::Vision) != nullptr
				|| def.FindEffect(SkillEffectType::PassiveStat) != nullptr || def.FindEffect(SkillEffectType::CcResist) != nullptr;
			for (const SkillEffect& e : def.effects)
			{
				hasTrigger |= (e.condition == SkillRequire::OnDot || e.condition == SkillRequire::OnDashAtMe || e.condition == SkillRequire::OnSkill
					|| e.condition == SkillRequire::OnStance || e.condition == SkillRequire::IdleCombat || e.condition == SkillRequire::OnDamaged
					|| e.condition == SkillRequire::OnKill || e.condition == SkillRequire::OnMoveEnd || e.condition == SkillRequire::LowHp);
			}
			EXPECT_TRUE(hasTrigger) << "passive skill " << skillId << " has no on_hit / lethal_save effect";
			EXPECT_EQ(def.castMs, 0u) << "passive skill " << skillId;
		}

		if (def.kind == SkillKind::Recast)
		{
			++recasts;
			bool hasRecast = false;
			for (const SkillEffect& e : def.effects)
				hasRecast |= (e.condition == SkillRequire::Recast);
			EXPECT_TRUE(hasRecast) << "recast skill " << skillId << " has no requires=recast effect";
		}

		// requires=recast 는 kind recast 에만
		for (const SkillEffect& e : def.effects)
		{
			if (e.condition == SkillRequire::Recast)
				EXPECT_EQ(def.kind, SkillKind::Recast) << "skill " << skillId << " has a recast effect but kind is not recast";
		}
	}

	// + 3차 로스터 10: 추혼 1143 · 플럭스 1151 · 신발 1177 ~ 1184
	EXPECT_EQ(passives, 21) << "#24 뇌신 · #34 맹수의 본능 · #67 화신 + 모자 #83 #88 #89 #90 #94 #96 #97 + 천리안 1132 + 3차 10";
	// 낚아채기 #45 는 2026-10-11 스킬 개선에서 재입력이 아니게 됐다 - 맞자마자 끌려온다. 3차 로스터: 비창 · 포르타 · 이단 질주
	EXPECT_EQ(recasts, 5) << "#38 피의 추적 · 찌르고 베기 1118 · 비창 1138 · 포르타 1150 · 이단 질주 1159";
}

// 3차 로스터 (게임 ID 1138 ~ 1189, 2026-10-11 밤) - 데이터가 엔진의 새 경로를 실제로 탄다 (짝이 없으면 엔진은 조용히 아무 일도 안 한다)
TEST(SkillRoster, 3차_로스터_데이터가_새_경로를_탄다)
{
	string error;
	ASSERT_TRUE(LoadRealData(OUT error)) << error;

	auto def = [](uint32 id) { return SkillTable::Find(id); };
	auto moveParam = [&](uint32 id, bool recast) -> string
		{
			for (const SkillEffect& e : def(id)->effects)
			{
				if (e.type == SkillEffectType::Movement && (e.condition == SkillRequire::Recast) == recast)
					return e.param;
			}
			return "";
		};

	// 비창 - 던진 창이 멈춘 자리 · 재입력 순간이동 · 창 5 s. 포르타 - 찍은 자리 · 10 s. 이단 질주는 기본 창 (2 s)
	ASSERT_NE(def(1138)->FindEffect(SkillEffectType::Anchor), nullptr);
	EXPECT_EQ(def(1138)->FindEffect(SkillEffectType::Anchor)->param, "projectile");
	EXPECT_EQ(moveParam(1138, true), "to_anchor");
	EXPECT_EQ(SkillTable::RecastWindowMs(*def(1138)), 5000u);
	EXPECT_EQ(def(1150)->FindEffect(SkillEffectType::Anchor)->param, "point");
	EXPECT_EQ(moveParam(1150, true), "to_anchor");
	EXPECT_EQ(SkillTable::RecastWindowMs(*def(1150)), 10000u);
	EXPECT_EQ(SkillTable::RecastWindowMs(*def(1159)), SkillTable::Rules().recastWindowMs);
	EXPECT_EQ(moveParam(1159, true), "dash");

	// 벽 차기 - 벽에 붙어 있을 때만 · 서버가 벽 반대쪽으로. 회귀 - 3 s 전. 활주 - 길고 느리다 (지속 상한 밖)
	EXPECT_EQ(def(1163)->FindEffect(SkillEffectType::Movement)->condition, SkillRequire::NearWall);
	EXPECT_EQ(moveParam(1163, false), "off_wall");
	EXPECT_EQ(def(1167)->FindEffect(SkillEffectType::Movement)->durationMs, 3000u);
	EXPECT_EQ(moveParam(1167, false), "rewind");
	const SkillEffect* slide = def(1170)->FindEffect(SkillEffectType::Movement);
	EXPECT_EQ(slide->param, "slide");
	EXPECT_GT(slide->distCm / slide->speedCms, 1.f) << "활주는 1 초 넘게 미끄러진다";

	// 전력 질주 - 유지형 · 이속 버프. 모래주머니 - 장착 중 이속 -10 %. 날개 - 이동 거리
	EXPECT_EQ(def(1169)->kind, SkillKind::Hold);
	EXPECT_EQ(def(1169)->FindEffect(SkillEffectType::Hold)->param, "sprint");
	EXPECT_EQ(def(1169)->FindEffect(SkillEffectType::SelfBuff)->stat, SkillBuffStat::MoveSpeed);
	const SkillEffect* sandbag = def(1174)->FindEffect(SkillEffectType::PassiveStat);
	ASSERT_NE(sandbag, nullptr);
	EXPECT_EQ(sandbag->stat, SkillBuffStat::MoveSpeed);
	EXPECT_LT(sandbag->magnitude, 0.f);
	EXPECT_EQ(def(1184)->FindEffect(SkillEffectType::PassiveStat)->stat, SkillBuffStat::MoveDist);

	// 시길룸 - 예고 + 속성마다 다른 효과 (불 · 번개 · 얼음 조건)
	EXPECT_GT(def(1156)->telegraphMs, 0u);
	set<string> elements;
	for (const SkillEffect& e : def(1156)->effects)
	{
		if (e.condition == SkillRequire::ElementIs)
			elements.insert(e.conditionParam);
	}
	EXPECT_EQ(elements, (set<string>{ "fire", "lightning", "frost" }));

	// 추혼 - 점지 표식 대상에게만 · 창 조건. 착지 충격 · 플럭스 - 반경이 있어야 맞는다
	EXPECT_EQ(def(1143)->FindEffect(SkillEffectType::OnHit)->condition, SkillRequire::MarkSpear);
	EXPECT_GT(def(1182)->effects[0].radiusCm, 0.f);
	EXPECT_GT(def(1151)->effects[0].radiusCm, 0.f);

	// 바쿠움 - 지점으로 끌기 (하드 CC 지만 스태프 조건부라 갑옷 풀 허용)
	EXPECT_EQ(def(1154)->FindEffect(SkillEffectType::Pull)->param, "to_point");
	EXPECT_TRUE(def(1154)->HasHardCc());
	EXPECT_EQ(def(1154)->requiresWeapon, "staff");

	// 글라키에스 - 얼음 속성. 스쿠툼 - 깨지면 터지는 보호막
	EXPECT_EQ(def(1148)->FindEffect(SkillEffectType::Stance)->param, "frost");
	EXPECT_EQ(def(1149)->FindEffect(SkillEffectType::Shield)->param, "burst");
	EXPECT_GT(def(1149)->FindEffect(SkillEffectType::Shield)->apRatio, 0.f);
}

// 2차 로스터 (2026-10-11) - 새 효과가 서로 맞물리는가. 엔진은 짝이 없으면 조용히 아무 일도 하지 않는다 - 여기서 잡는다
TEST(SkillRoster, 평타_모드와_연사_효과는_짝이_맞는다)
{
	string error;
	ASSERT_TRUE(LoadRealData(OUT error)) << error;

	for (const auto& [skillId, def] : SkillTable::All())
	{
		bool hasProcBuff = false;
		bool hasOnBasic = false;
		bool basicOnly = false;
		int32 otherBuffs = 0;

		for (const SkillEffect& e : def.effects)
		{
			if (e.type == SkillEffectType::SelfBuff && e.stat == SkillBuffStat::BasicProc)
				hasProcBuff = true;
			if (e.condition == SkillRequire::OnBasic)
			{
				hasOnBasic = true;
				EXPECT_FALSE(e.self) << "skill " << skillId << " on_basic effects hit the struck enemy (who=target)";
			}
			if (e.type == SkillEffectType::SelfBuff && e.stat == SkillBuffStat::BasicOnly)
				basicOnly = true;
			else if (e.type == SkillEffectType::SelfBuff)
				otherBuffs++;

			// 그림자 밟기 - 등 뒤로 가려면 고른 대상이 있어야 한다
			if (e.type == SkillEffectType::Movement && e.param == "behind")
				EXPECT_EQ(def.aimType, SkillAimType::Target) << "skill " << skillId << " move behind needs aim target";

			// 연사 - 투사체 제원은 스킬 줄의 것. 한 발씩(interval) 은 바라보는 방향을 따른다
			if (e.type == SkillEffectType::Volley)
				EXPECT_EQ(def.shape, SkillShape::Projectile) << "skill " << skillId;
		}

		// on_basic 효과는 basic_proc 버프가 켜져야 나간다 - 둘은 한 스킬 안에 함께
		EXPECT_EQ(hasProcBuff, hasOnBasic) << "skill " << skillId << " basic_proc buff and on_basic effects come together";

		// basic_only 는 그동안 평타를 바꾸는 버프와 함께여야 뜻이 있다 (통아 · 육량전 · 은장도)
		if (basicOnly)
			EXPECT_GE(otherBuffs, 1) << "skill " << skillId << " basic_only without a basic-attack buff";
	}

	// 은신 · 시야 스킬이 FoW 효과를 실제로 가진다 (1 단계의 reveal 자리표시가 남아 있으면 안 된다)
	EXPECT_NE(SkillTable::Find(1032)->FindEffect(SkillEffectType::VisionArea), nullptr) << "맹수의 탐색";
	EXPECT_EQ(SkillTable::Find(1032)->FindEffect(SkillEffectType::Reveal), nullptr) << "맹수의 탐색";
	EXPECT_NE(SkillTable::Find(1074)->FindEffect(SkillEffectType::Stealth), nullptr) << "니힐 - 기척을 지운다";
	EXPECT_NE(SkillTable::Find(1124)->FindEffect(SkillEffectType::Stealth), nullptr) << "그림자 감추기";
	EXPECT_EQ(SkillTable::Find(1124)->FindEffect(SkillEffectType::Stealth)->param, "break_on_damage");
	EXPECT_NE(SkillTable::Find(1079)->FindEffect(SkillEffectType::Shield), nullptr) << "전사의 다짐 - 방어막";
	EXPECT_GT(SkillTable::Find(1087)->FindEffect(SkillEffectType::Reveal)->durationMs, 0u) << "외로운 외침 - 공개 시간";
	EXPECT_EQ(SkillTable::Find(1017)->FindEffect(SkillEffectType::Damage), nullptr) << "점지 - 표식만";
}

// 1 단계 - 버프 · 지속 피해 · 표식 · 처형 효과의 필수 열. 엔진이 읽는 열이 비어 있으면 조용히 아무 일도 없다 - 여기서 잡는다
TEST(SkillRoster, 버프_지속피해_표식_효과는_필수_열이_채워져_있다)
{
	string error;
	ASSERT_TRUE(LoadRealData(OUT error)) << error;

	const set<string> dotKinds = { "bleed", "poison", "burn", "heavy_bleed", "frost" };
	const set<string> markKinds = { "spear", "arrow", "blood", "heavy_bleed", "rope", "devotion" };

	for (const auto& [skillId, def] : SkillTable::All())
	{
		for (const SkillEffect& e : def.effects)
		{
			switch (e.type)
			{
			case SkillEffectType::SelfBuff:
				EXPECT_TRUE(e.self && e.stat != SkillBuffStat::None && e.durationMs > 0 && e.magnitude != 0.f) << "skill " << skillId << " buff";
				break;
			case SkillEffectType::Dot:
				EXPECT_TRUE(dotKinds.count(e.param) > 0 && e.durationMs > 0 && (e.adRatio > 0.f || e.apRatio > 0.f)) << "skill " << skillId << " dot";
				break;
			case SkillEffectType::Mark:
				EXPECT_TRUE(markKinds.count(e.param) > 0 && e.durationMs > 0) << "skill " << skillId << " mark";
				break;
			case SkillEffectType::Knockback:
				// param far (저 멀리 1109) - 거리는 시전자의 시야 반경이라 dist_cm 이 비어 있다. 속도와 벽 충돌 계수는 있어야 한다
				if (e.param.find("far") != string::npos)
					EXPECT_TRUE(e.speedCms > 0.f && e.adRatio > 0.f) << "skill " << skillId << " far knockback";
				else
					EXPECT_TRUE(e.distCm > 0.f && e.speedCms > 0.f) << "skill " << skillId << " knockback";
				break;
			default:
				break;
			}
		}
	}
}

// 스킬 개선 (2026-10-11 저녁, 사용자 플레이 피드백) - 데이터가 엔진의 새 경로를 실제로 탄다.
// 엔진은 짝이 맞지 않으면 예전 경로로 조용히 돌아간다 (예: 열화 장판에 dist_cm 이 없으면 큰 원 3 개) - 여기서 잡는다
// 2026-10-15 사용자 - 맹수의 어금니 · 맹수의 앞발은 고른 적에게 도약하는 단일기, 피해는 착지 때 (move leap_target)
TEST(SkillRoster, 맹수의_어금니와_앞발은_고른_적에게_도약하는_단일기다)
{
	string error;
	ASSERT_TRUE(LoadRealData(OUT error)) << error;

	for (uint32 id : { 1030u, 1031u })
	{
		const SkillDef* def = SkillTable::Find(id);
		ASSERT_NE(def, nullptr) << id;
		EXPECT_EQ(def->shape, SkillShape::Single) << id;
		EXPECT_EQ(def->aimType, SkillAimType::Target) << id;
		const SkillEffect* move = def->FindEffect(SkillEffectType::Movement);
		ASSERT_NE(move, nullptr) << id;
		EXPECT_EQ(move->param, "leap_target") << id;
		EXPECT_EQ(def->FindEffect(SkillEffectType::FallOnHit), nullptr) << id << " - 도약 중 맞아도 떨어지지 않는다";
	}
	// 어금니는 착지하고 telegraph_ms 뒤에 친다 (애니메이션이 끝나기 0.1 초 전) - 앞발은 착지 때
	EXPECT_GT(SkillTable::Find(1030)->telegraphMs, 0u);
	EXPECT_EQ(SkillTable::Find(1031)->telegraphMs, 0u);
}

// 2026-10-15 사용자 - 후딜: 스킬이 나간 뒤 애니메이션이 끝날 때까지 이동 · 평타 · 다른 스킬이 막힌다. 버프 · 순수 이동기는 없다
TEST(SkillRoster, 후딜_표는_버프와_이동기만_비우고_나머지는_채운다)
{
	string error;
	ASSERT_TRUE(LoadRealData(OUT error)) << error;

	EXPECT_GT(SkillTable::Find(1004)->recoveryMs, 0u) << "휘두르기 - 클립이 있는 스킬";
	EXPECT_GT(SkillTable::Find(1022)->recoveryMs, 0u) << "클립 없는 스킬은 기본값";
	EXPECT_EQ(SkillTable::Find(1002)->recoveryMs, 0u) << "순수 이동기";
	uint32 locked = 0;
	for (const auto& [id, def] : SkillTable::All())
	{
		if (def.kind == SkillKind::Passive || def.kind == SkillKind::Hold)
			EXPECT_EQ(def.recoveryMs, 0u) << id;
		locked += def.recoveryMs > 0 ? 1 : 0;
	}
	EXPECT_GT(locked, 50u);

	// 모르는 스킬 · 숫자 아님은 거부한다
	EXPECT_FALSE(SkillTable::ParseRecovery("skill_id,recovery_ms\n999999,300\n", OUT error));
	EXPECT_NE(error.find("unknown skill"), string::npos) << error;
	EXPECT_FALSE(SkillTable::ParseRecovery("skill_id,recovery_ms\n1004,-1\n", OUT error));
	ASSERT_TRUE(LoadRealData(OUT error)) << error;		// 다음 테스트를 위해 되돌린다
}

TEST(SkillTable, 이동_param_은_아는_것만_받는다)
{
	const string header = "skill_id,name_key,power,kind,cast_ms,cooldown_ms,can_move,telegraph_ms,shape,aim,range_cm,width_cm,radius_cm,angle_deg,allowed_slot,projectile_speed_cms,pierce\n";
	const string eheader = "skill_id,seq,effect,who,requires,ad_ratio,ap_ratio,hits,interval_ms,cc_type,duration_ms,magnitude,dist_cm,speed_cms,ignores_walls,stat,per_level,radius_cm,param\n";
	const string rules = "key,value\ndot_tick_ms,500\ndot_max_stacks,5\nwall_stun_ms,750\nrecast_window_ms,2000\nexecute_hp_pct,15\nblind_view_scale,0.5\n"
		"pull_stop_cm,100\npull_speed_cms,1600\ndecoy_hp,1\nprojectile_radius_cm,40\nbasic_projectile_speed_cms,2400\nconfront_range_cm,400\n"
		"mark_spear_basic_bonus,0.5\nchain_radius_cm,300\ndelayed_burst_ms,1500\nspin_max_ms,3000\nhold_max_ms,0\nbuff_per_level_default,0.03\ntrail_buff_ms,2000\ntrail_pickup_cm,80\ntaunt_speed_cms,150\ntaunt_stop_cm,100\nlong_range_cm,500\nbasic_dash_speed_cms,1200\n";
	const string skill = header + "1,skill.1,ad,active,60,8000,0,0,single,target,400,0,0,0,0,0,0\n";
	string error;

	EXPECT_TRUE(SkillTable::Parse(skill, eheader + "1,1,move,self,none,0,0,0,0,none,0,0,400,1200,0,none,0,0,leap_target\n"
		"1,2,damage,target,none,1,0,1,0,none,0,0,0,0,0,none,0,0,none\n", rules, OUT error)) << error;
	EXPECT_FALSE(SkillTable::Parse(skill, eheader + "1,1,move,self,none,0,0,0,0,none,0,0,400,1200,0,none,0,0,leap_to\n", rules, OUT error));
	EXPECT_NE(error.find("leap_target"), string::npos) << error;
}

TEST(SkillRoster, 스킬개선_2026_10_11_데이터가_새_경로를_탄다)
{
	string error;
	ASSERT_TRUE(LoadRealData(OUT error)) << error;

	auto def = [](uint32 id) { return SkillTable::Find(id); };

	// 오버로드 · 니힐 - 속성 없이 쓰면 "효과가 끝날 때" 기절
	for (uint32 id : { 1073u, 1074u })
	{
		const SkillEffect* selfCc = def(id)->FindEffect(SkillEffectType::SelfCc);
		ASSERT_NE(selfCc, nullptr) << id;
		EXPECT_EQ(selfCc->param, "at_end") << id;
		EXPECT_EQ(selfCc->condition, SkillRequire::NoElement) << id;
	}

	// 포착 · 약속의 한 발 - 논타겟팅 투사체, 확신의 표식(arrow)으로 유도
	for (uint32 id : { 1055u, 1057u })
	{
		EXPECT_EQ(def(id)->shape, SkillShape::Projectile) << id;
		EXPECT_EQ(def(id)->aimType, SkillAimType::Direction) << id;
		EXPECT_TRUE(def(id)->HasTag("homing:arrow")) << id;
	}
	EXPECT_NE(def(1055)->FindEffect(SkillEffectType::Volley), nullptr) << "포착 - 여러 발 (그림자 던지기처럼)";
	EXPECT_EQ(def(1052)->shape, SkillShape::Projectile) << "비장의 한 발 - 논타겟팅 일직선";

	// 저 멀리 - 대상 지정, 평타 사거리, 시야 반경만큼 날리는 넉백
	EXPECT_EQ(def(1109)->aimType, SkillAimType::Target);
	EXPECT_FLOAT_EQ(def(1109)->rangeCm, 190.f) << "망치 평타 사거리 (item_types.csv 102)";
	ASSERT_NE(def(1109)->FindEffect(SkillEffectType::Knockback), nullptr);
	EXPECT_EQ(def(1109)->FindEffect(SkillEffectType::Knockback)->param, "far");

	// 건곤일척 - 피해 증가는 제자리 한 번 (param ;still)
	ASSERT_NE(def(1111)->FindEffect(SkillEffectType::DamageMod), nullptr);
	EXPECT_EQ(def(1111)->FindEffect(SkillEffectType::DamageMod)->param, "tag:hammer;still");

	// 열화 - 지나간 길에만 작은 원 (zone dist_cm = 칠하는 간격)
	const SkillEffect* firePath = def(1063)->FindEffect(SkillEffectType::Zone);
	ASSERT_NE(firePath, nullptr);
	EXPECT_GT(firePath->distCm, 0.f);
	EXPECT_LT(firePath->radiusCm, 100.f) << "작은 원";
	EXPECT_TRUE(def(1063)->HasMovement());

	// 네불라 아르카나 - 시전자 앞 부채꼴 장판
	EXPECT_EQ(def(1069)->shape, SkillShape::Cone);
	EXPECT_NE(def(1069)->FindEffect(SkillEffectType::Zone), nullptr);

	// 즉발 · 조준 없음
	EXPECT_EQ(def(1113)->castMs, 0u) << "백귀야행 즉발";
	EXPECT_EQ(def(1118)->castMs, 0u) << "찌르고 베기 즉발";
	EXPECT_EQ(def(1118)->aimType, SkillAimType::Facing) << "찌르고 베기 - 누르자마자 바라보는 방향";
	EXPECT_EQ(def(1108)->castMs, 1500u) << "질풍노도 - 평타 시전(300 ms)의 5 배";

	// 대상 지정으로 바뀐 돌진 (기습 단검 · 일도양단) - 대상 바로 앞까지
	for (uint32 id : { 1008u, 1035u })
	{
		EXPECT_EQ(def(id)->aimType, SkillAimType::Target) << id;
		ASSERT_NE(def(id)->FindEffect(SkillEffectType::Movement), nullptr) << id;
		EXPECT_EQ(def(id)->FindEffect(SkillEffectType::Movement)->param, "to_target") << id;
	}

	// 낙뢰 - 범위 안 점지 표식이 남은 적 전부 (논타겟팅)
	EXPECT_EQ(def(1022)->shape, SkillShape::CircleSelf);
	EXPECT_EQ(def(1022)->FindEffect(SkillEffectType::Damage)->condition, SkillRequire::MarkSpear);

	// 염두 - 패시브지만 쿨다운이 클라에 보인다 (skills.csv cooldown_ms)
	EXPECT_GT(def(1090)->cooldownMs, 0u);

	// 낚아채기 - 재입력이 아니다, 던지는 동안 움직인다, 맞으면 바로 끌려온다
	EXPECT_EQ(def(1045)->kind, SkillKind::Active);
	EXPECT_TRUE(def(1045)->canMoveWhileCasting);
	ASSERT_NE(def(1045)->FindEffect(SkillEffectType::Pull), nullptr);
	EXPECT_EQ(def(1045)->FindEffect(SkillEffectType::Pull)->condition, SkillRequire::None);
}
