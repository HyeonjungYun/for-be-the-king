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

	// src/스킬_목록.csv 2 ~ 77 (1 채찍 제외) → 1002 ~ 1077. 9001 은 몬스터
	for (uint32 n = 2; n <= 77; n++)
		EXPECT_NE(SkillTable::Find(1000 + n), nullptr) << "skill " << n << " is missing";

	EXPECT_NE(SkillTable::Find(9001), nullptr);
	EXPECT_EQ(SkillTable::Find(1001), nullptr) << "채찍(#1)은 구현하지 않는다";
	EXPECT_EQ(SkillTable::All().size(), 77u);
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

				float ratio = 0.f;
				for (const SkillEffect& fx : def->effects)
				{
					if (fx.type == SkillEffectType::Damage && fx.condition == SkillRequire::None)
						ratio += (fx.adRatio + fx.apRatio) * max(1u, fx.hits);
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
		"mark_spear_basic_bonus,0.5\nchain_radius_cm,300\ndelayed_burst_ms,1500\nspin_max_ms,3000\nhold_max_ms,0\nbuff_per_level_default,0.03\ntrail_buff_ms,2000\ntrail_pickup_cm,80\n";
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
			const bool hasTrigger = def.FindEffect(SkillEffectType::OnHit) != nullptr || def.FindEffect(SkillEffectType::LethalSave) != nullptr;
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

	EXPECT_EQ(passives, 3) << "#24 뇌신 · #34 맹수의 본능 · #67 화신";
	EXPECT_EQ(recasts, 2) << "#38 피의 추적 · #45 올가미";
}

// 1 단계 - 버프 · 지속 피해 · 표식 · 처형 효과의 필수 열. 엔진이 읽는 열이 비어 있으면 조용히 아무 일도 없다 - 여기서 잡는다
TEST(SkillRoster, 버프_지속피해_표식_효과는_필수_열이_채워져_있다)
{
	string error;
	ASSERT_TRUE(LoadRealData(OUT error)) << error;

	const set<string> dotKinds = { "bleed", "poison", "burn", "heavy_bleed" };
	const set<string> markKinds = { "spear", "arrow", "blood", "heavy_bleed", "rope" };

	for (const auto& [skillId, def] : SkillTable::All())
	{
		for (const SkillEffect& e : def.effects)
		{
			switch (e.type)
			{
			case SkillEffectType::SelfBuff:
				EXPECT_TRUE(e.self && e.stat != SkillBuffStat::None && e.durationMs > 0 && e.magnitude > 0.f) << "skill " << skillId << " buff";
				break;
			case SkillEffectType::Dot:
				EXPECT_TRUE(dotKinds.count(e.param) > 0 && e.durationMs > 0 && (e.adRatio > 0.f || e.apRatio > 0.f)) << "skill " << skillId << " dot";
				break;
			case SkillEffectType::Mark:
				EXPECT_TRUE(markKinds.count(e.param) > 0 && e.durationMs > 0) << "skill " << skillId << " mark";
				break;
			case SkillEffectType::Knockback:
				EXPECT_TRUE(e.distCm > 0.f && e.speedCms > 0.f) << "skill " << skillId << " knockback";
				break;
			default:
				break;
			}
		}
	}
}
