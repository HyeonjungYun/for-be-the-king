#include "pch.h"
#include "SkillTable.h"
#include "CsvTable.h"

unordered_map<uint32, SkillDef> SkillTable::s_defs;
SkillRules SkillTable::s_rules;

namespace
{
	// skill-system.md § S1 · S2 — 스킬이 넘을 수 없는 천장. AC-5(4인 체인 CC ≤ 5 s)는 이 값으로 계산했다
	constexpr uint32	STUN_CAP_MS			= 1250;
	constexpr uint32	LAUNCH_CAP_MS		= 1000;
	constexpr uint32	ROOT_CAP_MS			= 1750;
	constexpr float		KNOCKBACK_DIST_CAP	= 300.f;
	constexpr float		KNOCKBACK_SPEED_MIN	= 450.f;
	constexpr float		KNOCKBACK_SPEED_MAX	= 1360.f;
	constexpr float		DASH_DURATION_MAX_S	= 0.4f;		// movement-camera.md dash_duration 상한 - speed >= dist / 0.4
	constexpr float		RATIO_CAP			= 3.f;		// S3 ad_ratio · ap_ratio [0, 3.0]
	constexpr float		SLOW_CAP			= 0.4f;		// soft_cc_slow_cap
	constexpr uint32	SLOW_DURATION_CAP_MS = 3000;
	constexpr uint32	MOVE_CAST_MIN_MS	= 60;		// 이동 스킬은 Casting 을 거쳐야 한다 (design-review 2026-08-20 결정 1)

	template<typename T>
	bool ParseEnum(const string& text, const vector<pair<const char*, T>>& names, OUT T& out)
	{
		for (const auto& [name, value] : names)
		{
			if (text == name)
			{
				out = value;
				return true;
			}
		}
		return false;
	}

	const vector<pair<const char*, SkillKind>> KIND_NAMES =
	{
		{ "active", SkillKind::Active }, { "passive", SkillKind::Passive }, { "hold", SkillKind::Hold }, { "recast", SkillKind::Recast },
	};

	const vector<pair<const char*, SkillShape>> SHAPE_NAMES =
	{
		{ "none", SkillShape::None }, { "single", SkillShape::Single }, { "line", SkillShape::Line }, { "cone", SkillShape::Cone },
		{ "circle_self", SkillShape::CircleSelf }, { "circle_point", SkillShape::CirclePoint }, { "projectile", SkillShape::Projectile },
	};

	const vector<pair<const char*, SkillAimType>> AIM_NAMES =
	{
		{ "none", SkillAimType::None }, { "self_area", SkillAimType::SelfArea }, { "target", SkillAimType::Target },
		{ "direction", SkillAimType::Direction }, { "point", SkillAimType::Point },
	};

	const vector<pair<const char*, SkillPower>> POWER_NAMES =
	{
		{ "none", SkillPower::None }, { "ad", SkillPower::Ad }, { "ap", SkillPower::Ap }, { "hybrid", SkillPower::Hybrid },
	};

	const vector<pair<const char*, SkillEffectType>> EFFECT_NAMES =
	{
		{ "damage", SkillEffectType::Damage }, { "hard_cc", SkillEffectType::HardCc }, { "soft_cc", SkillEffectType::SoftCc },
		{ "knockback", SkillEffectType::Knockback }, { "pull", SkillEffectType::Pull }, { "move", SkillEffectType::Movement },
		{ "buff", SkillEffectType::SelfBuff }, { "dot", SkillEffectType::Dot }, { "mark", SkillEffectType::Mark },
		{ "execute", SkillEffectType::Execute }, { "zone", SkillEffectType::Zone }, { "chain", SkillEffectType::Chain },
		{ "delayed", SkillEffectType::Delayed }, { "decoy", SkillEffectType::Decoy }, { "invuln", SkillEffectType::Invuln },
		{ "counter", SkillEffectType::Counter }, { "parry", SkillEffectType::Parry }, { "hold", SkillEffectType::Hold },
		{ "stance", SkillEffectType::Stance }, { "on_hit", SkillEffectType::OnHit }, { "lethal_save", SkillEffectType::LethalSave },
		{ "reveal", SkillEffectType::Reveal }, { "trail", SkillEffectType::Trail }, { "self_cc", SkillEffectType::SelfCc },
		{ "petrify_window", SkillEffectType::PetrifyWindow }, { "fall_on_hit", SkillEffectType::FallOnHit }, { "throw", SkillEffectType::Throw },
	};

	const vector<pair<const char*, SkillRequire>> REQUIRE_NAMES =
	{
		{ "none", SkillRequire::None }, { "mark:spear", SkillRequire::MarkSpear }, { "mark:arrow", SkillRequire::MarkArrow },
		{ "mark:blood", SkillRequire::MarkBlood }, { "mark:heavy_bleed", SkillRequire::MarkHeavyBleed }, { "mark:rope", SkillRequire::MarkRope },
		{ "stunned", SkillRequire::Stunned }, { "element", SkillRequire::Element }, { "no_element", SkillRequire::NoElement },
		{ "hit", SkillRequire::Hit }, { "no_kill", SkillRequire::NoKill }, { "recast", SkillRequire::Recast }, { "wall", SkillRequire::Wall },
	};

	const vector<pair<const char*, Protocol::CcType>> CC_NAMES =
	{
		{ "none", Protocol::CC_TYPE_NONE }, { "stun", Protocol::CC_TYPE_STUN }, { "root", Protocol::CC_TYPE_ROOT },
		{ "knockback", Protocol::CC_TYPE_KNOCKBACK }, { "launch", Protocol::CC_TYPE_LAUNCH }, { "slow", Protocol::CC_TYPE_SLOW },
		{ "silence", Protocol::CC_TYPE_SILENCE }, { "blind", Protocol::CC_TYPE_BLIND },
	};

	const vector<pair<const char*, SkillBuffStat>> STAT_NAMES =
	{
		{ "none", SkillBuffStat::None }, { "attack_power", SkillBuffStat::AttackPower }, { "magic_power", SkillBuffStat::MagicPower },
		{ "crit_chance", SkillBuffStat::CritChance }, { "attack_speed", SkillBuffStat::AttackSpeed }, { "move_speed", SkillBuffStat::MoveSpeed },
		{ "armor", SkillBuffStat::Armor }, { "magic_resist", SkillBuffStat::MagicResist }, { "damage_pct", SkillBuffStat::DamagePct },
	};

	bool ParseBool(const string& s, OUT bool& out)
	{
		if (s == "0" || s == "1")
		{
			out = (s == "1");
			return true;
		}
		return false;
	}

	// 한 스킬의 효과 목록 전체가 하드 CC 를 포함하는가 - 풀 규칙(무기 전용)과 E2 재추첨이 본다
	bool IsHardCcEffect(const SkillEffect& e)
	{
		switch (e.type)
		{
		case SkillEffectType::Knockback:
		case SkillEffectType::Pull:
		case SkillEffectType::PetrifyWindow:
		case SkillEffectType::Throw:
			return true;
		case SkillEffectType::HardCc:
			return e.ccType == Protocol::CC_TYPE_STUN || e.ccType == Protocol::CC_TYPE_KNOCKBACK
				|| e.ccType == Protocol::CC_TYPE_LAUNCH || e.ccType == Protocol::CC_TYPE_ROOT;
		case SkillEffectType::Hold:
			return e.ccType == Protocol::CC_TYPE_ROOT;		// 빈쿨룸
		default:
			return false;
		}
	}

	// 스킬 한 줄의 모양 · 조준 · 수치가 서로 맞는지
	bool ValidateDef(const SkillDef& def, OUT string& error)
	{
		const string where = "skills.csv skill " + to_string(def.skillId);

		// R12b - 조준 없는 스킬에 캐스트가 있으면 키를 계속 눌러야 한다. 유지형 · 패시브는 캐스트가 없다
		if (def.aimType == SkillAimType::None && def.castMs > 0)
		{
			error = where + " has aim none with cast_ms > 0 (skill-system.md R12b)";
			return false;
		}

		if ((def.kind == SkillKind::Passive || def.kind == SkillKind::Hold) && def.castMs != 0)
		{
			error = where + " passive / hold skills cannot have cast_ms";
			return false;
		}

		switch (def.shape)
		{
		case SkillShape::None:
			if (def.aimType != SkillAimType::None)
			{
				error = where + " shape none needs aim none";
				return false;
			}
			break;
		case SkillShape::Single:
			if (def.rangeCm <= 0.f || (def.aimType != SkillAimType::Target && def.aimType != SkillAimType::None))
			{
				error = where + " single needs range_cm > 0 and aim target (or none = nearest in front)";
				return false;
			}
			break;
		case SkillShape::Line:
			if (def.rangeCm <= 0.f || def.widthCm <= 0.f || def.aimType != SkillAimType::Direction)
			{
				error = where + " line needs range_cm, width_cm > 0 and aim direction";
				return false;
			}
			break;
		case SkillShape::Cone:
			if (def.radiusCm <= 0.f || def.angleDeg <= 0.f || def.angleDeg > 360.f || def.aimType != SkillAimType::Direction)
			{
				error = where + " cone needs radius_cm > 0, angle_deg in (0, 360] and aim direction";
				return false;
			}
			break;
		case SkillShape::CircleSelf:
			// radius 0 = 아무도 맞히지 않는 이동 스킬 (R12b) - direction · point 조준 허용
			if (def.aimType == SkillAimType::Target)
			{
				error = where + " circle_self cannot aim at a target";
				return false;
			}
			if (def.radiusCm <= 0.f && def.HasMovement() == false)
			{
				error = where + " circle_self with radius 0 must be a movement skill";
				return false;
			}
			break;
		case SkillShape::CirclePoint:
			if (def.rangeCm <= 0.f || def.radiusCm <= 0.f || def.aimType != SkillAimType::Point)
			{
				error = where + " circle_point needs range_cm, radius_cm > 0 and aim point";
				return false;
			}
			break;
		case SkillShape::Projectile:
			if (def.rangeCm <= 0.f || def.widthCm <= 0.f || def.projectileSpeedCms <= 0.f
				|| (def.aimType != SkillAimType::Direction && def.aimType != SkillAimType::Target))
			{
				error = where + " projectile needs range_cm, width_cm, projectile_speed_cms > 0 and aim direction / target";
				return false;
			}
			break;
		}

		if (def.shape != SkillShape::Projectile && (def.projectileSpeedCms != 0.f || def.pierce))
		{
			error = where + " projectile_speed_cms / pierce belong to projectile skills only";
			return false;
		}

		for (const SkillEffect& e : def.effects)
		{
			const string ew = where + " effect " + SkillTable::EffectName(e.type);

			if (e.adRatio < 0.f || e.adRatio > RATIO_CAP || e.apRatio < 0.f || e.apRatio > RATIO_CAP)
			{
				error = ew + " ad_ratio / ap_ratio must be in [0, 3.0] (S3)";
				return false;
			}

			switch (e.type)
			{
			case SkillEffectType::Damage:
				if (e.adRatio <= 0.f && e.apRatio <= 0.f)
				{
					error = ew + " needs ad_ratio or ap_ratio";
					return false;
				}
				if (e.hits == 0 || (e.hits > 1 && e.intervalMs == 0))
				{
					error = ew + " hits must be >= 1 and multi-hit needs interval_ms";
					return false;
				}
				break;

			case SkillEffectType::HardCc:
			{
				uint32 cap = 0;
				switch (e.ccType)
				{
				case Protocol::CC_TYPE_STUN:	cap = STUN_CAP_MS; break;
				case Protocol::CC_TYPE_LAUNCH:	cap = LAUNCH_CAP_MS; break;
				case Protocol::CC_TYPE_ROOT:	cap = ROOT_CAP_MS; break;
				default:
					error = ew + " cc_type must be stun / launch / root (knockback is its own effect)";
					return false;
				}
				if (e.durationMs == 0 || e.durationMs > cap)
				{
					error = ew + " duration_ms must be 1.." + to_string(cap) + " (skill-system.md S1 cap)";
					return false;
				}
				break;
			}

			case SkillEffectType::SoftCc:
				if (e.ccType == Protocol::CC_TYPE_SLOW)
				{
					if (e.magnitude <= 0.f || e.magnitude > SLOW_CAP || e.durationMs == 0 || e.durationMs > SLOW_DURATION_CAP_MS)
					{
						error = ew + " slow needs magnitude (0, 0.4] and duration_ms 1..3000";
						return false;
					}
				}
				else if (e.ccType == Protocol::CC_TYPE_BLIND || e.ccType == Protocol::CC_TYPE_SILENCE)
				{
					if (e.durationMs == 0)
					{
						error = ew + " needs duration_ms";
						return false;
					}
				}
				else
				{
					error = ew + " cc_type must be slow / blind / silence";
					return false;
				}
				break;

			case SkillEffectType::Knockback:
				if (e.distCm <= 0.f || e.distCm > KNOCKBACK_DIST_CAP || e.speedCms < KNOCKBACK_SPEED_MIN || e.speedCms > KNOCKBACK_SPEED_MAX)
				{
					error = ew + " needs dist_cm (0, 300] and speed_cms [450, 1360] (S2)";
					return false;
				}
				if (e.speedCms < e.distCm / DASH_DURATION_MAX_S)
				{
					error = ew + " speed_cms must be >= dist_cm / 0.4 (S2 schema constraint)";
					return false;
				}
				break;

			case SkillEffectType::Movement:
				if (e.self == false || e.distCm <= 0.f)
				{
					error = ew + " must be who=self with dist_cm > 0";
					return false;
				}
				// blink 는 즉시 (speed 0). 나머지는 dash_kinematics 지속 상한 안
				if (e.param != "blink" && (e.speedCms <= 0.f || e.speedCms < e.distCm / DASH_DURATION_MAX_S))
				{
					error = ew + " speed_cms must be >= dist_cm / 0.4 (dash_duration cap)";
					return false;
				}
				if (e.param != "dash" && e.param != "leap" && e.param != "back" && e.param != "forward" && e.param != "blink")
				{
					error = ew + " param must be dash / leap / back / forward / blink";
					return false;
				}
				break;

			case SkillEffectType::SelfBuff:
				if (e.self == false || e.stat == SkillBuffStat::None || e.durationMs == 0 || e.magnitude <= 0.f)
				{
					error = ew + " needs who=self, stat, duration_ms and magnitude";
					return false;
				}
				break;

			case SkillEffectType::Dot:
				if (e.apRatio <= 0.f && e.adRatio <= 0.f)
				{
					error = ew + " needs a per-tick ratio";
					return false;
				}
				if (e.durationMs == 0 || e.param.empty())
				{
					error = ew + " needs duration_ms and param (bleed / poison / burn / heavy_bleed)";
					return false;
				}
				break;

			case SkillEffectType::Mark:
				if (e.durationMs == 0 || e.param.empty())
				{
					error = ew + " needs duration_ms and param";
					return false;
				}
				break;

			case SkillEffectType::Execute:
				if (e.magnitude <= 0.f || e.magnitude >= 100.f)
				{
					error = ew + " magnitude is the max-hp % threshold, (0, 100)";
					return false;
				}
				break;

			case SkillEffectType::Zone:
				if (e.radiusCm <= 0.f || e.durationMs == 0 || e.param.empty())
				{
					error = ew + " needs radius_cm, duration_ms and param";
					return false;
				}
				break;

			case SkillEffectType::Chain:
				if (e.hits == 0 || e.radiusCm <= 0.f || e.apRatio <= 0.f)
				{
					error = ew + " needs hits (jumps), radius_cm and ap_ratio";
					return false;
				}
				break;

			case SkillEffectType::Pull:
				if (e.speedCms <= 0.f)
				{
					error = ew + " needs speed_cms";
					return false;
				}
				break;

			case SkillEffectType::LethalSave:
				if (e.durationMs == 0 || e.distCm <= 0.f || e.speedCms <= 0.f)
				{
					error = ew + " needs duration_ms (cooldown), dist_cm and speed_cms";
					return false;
				}
				break;

			case SkillEffectType::SelfCc:
			case SkillEffectType::PetrifyWindow:
				if (e.ccType != Protocol::CC_TYPE_STUN || e.durationMs == 0)
				{
					error = ew + " needs cc_type stun and duration_ms";
					return false;
				}
				break;

			default:
				break;
			}
		}

		// 이동 스킬은 Casting 을 거친다 - cast_ms 0 이면 취소 경로가 없다
		if (def.HasMovement() && def.kind == SkillKind::Active && def.castMs < MOVE_CAST_MIN_MS)
		{
			error = where + " has a movement effect - cast_ms must be >= " + to_string(MOVE_CAST_MIN_MS);
			return false;
		}

		return true;
	}

	bool ReadRule(const unordered_map<string, string>& kv, const char* key, OUT uint32& out, OUT string& error)
	{
		auto it = kv.find(key);
		if (it == kv.end() || Csv::ParseUInt(it->second, OUT out) == false)
		{
			error = string("skill_rules.csv is missing or has a bad '") + key + "'";
			return false;
		}
		return true;
	}

	bool ReadRule(const unordered_map<string, string>& kv, const char* key, OUT float& out, OUT string& error)
	{
		auto it = kv.find(key);
		if (it == kv.end() || Csv::ParseFloat(it->second, OUT out) == false)
		{
			error = string("skill_rules.csv is missing or has a bad '") + key + "'";
			return false;
		}
		return true;
	}
}

bool SkillDef::HasHardCc() const
{
	for (const SkillEffect& e : effects)
	{
		if (IsHardCcEffect(e))
			return true;
	}
	return false;
}

bool SkillDef::HasMovement() const
{
	for (const SkillEffect& e : effects)
	{
		// recast 의 두 번째 이동은 첫 입력의 캐스트와 무관하다
		if (e.type == SkillEffectType::Movement && e.condition != SkillRequire::Recast)
			return true;
	}
	return false;
}

const SkillEffect* SkillDef::FindEffect(SkillEffectType type) const
{
	for (const SkillEffect& e : effects)
	{
		if (e.type == type)
			return &e;
	}
	return nullptr;
}

const char* SkillTable::EffectName(SkillEffectType type)
{
	for (const auto& [name, value] : EFFECT_NAMES)
	{
		if (value == type)
			return name;
	}
	return "?";
}

const char* SkillTable::BuffStatName(SkillBuffStat stat)
{
	for (const auto& [name, value] : STAT_NAMES)
	{
		if (value == stat)
			return name;
	}
	return "?";
}

bool SkillTable::Load(const string& dataDir, OUT string& error)
{
	const char* names[3] = { "skills.csv", "skill_effects.csv", "skill_rules.csv" };
	string texts[3];

	for (int32 i = 0; i < 3; i++)
	{
		if (Csv::ReadFile(dataDir + "\\" + names[i], OUT texts[i]) == false)
		{
			error = string("cannot open ") + names[i];
			s_defs.clear();
			return false;
		}
	}

	return Parse(texts[0], texts[1], texts[2], OUT error);
}

bool SkillTable::Parse(const string& skillsCsv, const string& effectsCsv, const string& rulesCsv, OUT string& error)
{
	s_defs.clear();
	s_rules = SkillRules{};

	unordered_map<uint32, SkillDef> defs;
	SkillRules rules;

	// ── skill_rules.csv ──
	{
		CsvTable table;
		vector<int32> c;
		if (Csv::Parse(rulesCsv, "skill_rules.csv", OUT table, OUT error) == false
			|| Csv::FindColumns(table, "skill_rules.csv", { "key", "value" }, OUT c, OUT error) == false)
			return false;

		unordered_map<string, string> kv;
		for (const vector<string>& row : table.rows)
			kv[row[c[0]]] = row[c[1]];

		if (ReadRule(kv, "dot_tick_ms", OUT rules.dotTickMs, OUT error) == false
			|| ReadRule(kv, "dot_max_stacks", OUT rules.dotMaxStacks, OUT error) == false
			|| ReadRule(kv, "wall_stun_ms", OUT rules.wallStunMs, OUT error) == false
			|| ReadRule(kv, "recast_window_ms", OUT rules.recastWindowMs, OUT error) == false
			|| ReadRule(kv, "execute_hp_pct", OUT rules.executeHpPct, OUT error) == false
			|| ReadRule(kv, "blind_view_scale", OUT rules.blindViewScale, OUT error) == false
			|| ReadRule(kv, "pull_stop_cm", OUT rules.pullStopCm, OUT error) == false
			|| ReadRule(kv, "pull_speed_cms", OUT rules.pullSpeedCms, OUT error) == false
			|| ReadRule(kv, "projectile_radius_cm", OUT rules.projectileRadiusCm, OUT error) == false
			|| ReadRule(kv, "basic_projectile_speed_cms", OUT rules.basicProjectileSpeedCms, OUT error) == false
			|| ReadRule(kv, "confront_range_cm", OUT rules.confrontRangeCm, OUT error) == false
			|| ReadRule(kv, "mark_spear_basic_bonus", OUT rules.markSpearBasicBonus, OUT error) == false
			|| ReadRule(kv, "chain_radius_cm", OUT rules.chainRadiusCm, OUT error) == false
			|| ReadRule(kv, "delayed_burst_ms", OUT rules.delayedBurstMs, OUT error) == false
			|| ReadRule(kv, "spin_max_ms", OUT rules.spinMaxMs, OUT error) == false
			|| ReadRule(kv, "hold_max_ms", OUT rules.holdMaxMs, OUT error) == false
			|| ReadRule(kv, "buff_per_level_default", OUT rules.buffPerLevelDefault, OUT error) == false)
			return false;

		uint32 decoyHp = 0;
		if (ReadRule(kv, "decoy_hp", OUT decoyHp, OUT error) == false)
			return false;
		rules.decoyHp = static_cast<int32>(decoyHp);

		if (rules.dotTickMs == 0 || rules.dotMaxStacks == 0 || rules.wallStunMs > STUN_CAP_MS || rules.recastWindowMs == 0
			|| rules.executeHpPct <= 0.f || rules.executeHpPct >= 100.f || rules.blindViewScale <= 0.f || rules.blindViewScale > 1.f
			|| rules.pullSpeedCms <= 0.f || rules.projectileRadiusCm <= 0.f || rules.basicProjectileSpeedCms <= 0.f || rules.confrontRangeCm <= 0.f)
		{
			error = "skill_rules.csv has a value out of range";
			return false;
		}
	}

	// ── skills.csv ──
	{
		CsvTable table;
		vector<int32> c;
		if (Csv::Parse(skillsCsv, "skills.csv", OUT table, OUT error) == false
			|| Csv::FindColumns(table, "skills.csv", { "skill_id", "name_key", "power", "kind", "cast_ms", "cooldown_ms", "can_move", "telegraph_ms",
				"shape", "aim", "range_cm", "width_cm", "radius_cm", "angle_deg", "allowed_slot", "projectile_speed_cms", "pierce" }, OUT c, OUT error) == false)
			return false;

		for (size_t r = 0; r < table.rows.size(); r++)
		{
			const vector<string>& row = table.rows[r];
			const string where = "skills.csv:" + to_string(table.lineNumbers[r]);

			SkillDef def;
			uint32 allowed = 0;

			if (Csv::ParseUInt(row[c[0]], OUT def.skillId) == false || def.skillId == 0)
			{
				error = where + " skill_id must be a positive number";
				return false;
			}
			def.effectId = def.skillId;
			def.nameKey = row[c[1]];

			if (def.nameKey.empty())
			{
				error = where + " name_key is required";
				return false;
			}

			if (ParseEnum(row[c[2]], POWER_NAMES, OUT def.power) == false
				|| ParseEnum(row[c[3]], KIND_NAMES, OUT def.kind) == false
				|| ParseEnum(row[c[8]], SHAPE_NAMES, OUT def.shape) == false
				|| ParseEnum(row[c[9]], AIM_NAMES, OUT def.aimType) == false)
			{
				error = where + " unknown power / kind / shape / aim";
				return false;
			}

			if (Csv::ParseUInt(row[c[4]], OUT def.castMs) == false || Csv::ParseUInt(row[c[5]], OUT def.cooldownMs) == false
				|| ParseBool(row[c[6]], OUT def.canMoveWhileCasting) == false || Csv::ParseUInt(row[c[7]], OUT def.telegraphMs) == false
				|| Csv::ParseFloat(row[c[10]], OUT def.rangeCm) == false || Csv::ParseFloat(row[c[11]], OUT def.widthCm) == false
				|| Csv::ParseFloat(row[c[12]], OUT def.radiusCm) == false || Csv::ParseFloat(row[c[13]], OUT def.angleDeg) == false
				|| Csv::ParseUInt(row[c[14]], OUT allowed) == false || Csv::ParseFloat(row[c[15]], OUT def.projectileSpeedCms) == false
				|| ParseBool(row[c[16]], OUT def.pierce) == false)
			{
				error = where + " numeric column is not a number (can_move / pierce are 0 or 1)";
				return false;
			}

			if (def.rangeCm < 0.f || def.widthCm < 0.f || def.radiusCm < 0.f || def.angleDeg < 0.f || def.projectileSpeedCms < 0.f)
			{
				error = where + " distances must be >= 0";
				return false;
			}

			if (allowed > Protocol::SLOT_WEAPON_SECONDARY)
			{
				error = where + " allowed_slot must be 0 (any key), 1 (Shift) or 2 (Q)";
				return false;
			}
			def.allowedSlot = static_cast<Protocol::EquipSlot>(allowed);

			if (defs.emplace(def.skillId, def).second == false)
			{
				error = where + " duplicate skill_id " + to_string(def.skillId);
				return false;
			}
		}
	}

	if (defs.empty())
	{
		error = "skills.csv has no rows";
		return false;
	}

	// ── skill_effects.csv ──
	{
		CsvTable table;
		vector<int32> c;
		if (Csv::Parse(effectsCsv, "skill_effects.csv", OUT table, OUT error) == false
			|| Csv::FindColumns(table, "skill_effects.csv", { "skill_id", "seq", "effect", "who", "requires", "ad_ratio", "ap_ratio", "hits", "interval_ms",
				"cc_type", "duration_ms", "magnitude", "dist_cm", "speed_cms", "ignores_walls", "stat", "per_level", "radius_cm", "param" }, OUT c, OUT error) == false)
			return false;

		unordered_map<uint32, uint32> lastSeq;

		for (size_t r = 0; r < table.rows.size(); r++)
		{
			const vector<string>& row = table.rows[r];
			const string where = "skill_effects.csv:" + to_string(table.lineNumbers[r]);

			uint32 skillId = 0;
			uint32 seq = 0;
			if (Csv::ParseUInt(row[c[0]], OUT skillId) == false || Csv::ParseUInt(row[c[1]], OUT seq) == false)
			{
				error = where + " skill_id and seq must be numbers";
				return false;
			}

			auto defIt = defs.find(skillId);
			if (defIt == defs.end())
			{
				error = where + " skill " + to_string(skillId) + " is not in skills.csv";
				return false;
			}

			// seq 는 1 부터 빈틈없이 - 적용 순서가 곧 의미다 (이동 → 피해 → CC)
			if (seq != lastSeq[skillId] + 1)
			{
				error = where + " seq must be " + to_string(lastSeq[skillId] + 1);
				return false;
			}
			lastSeq[skillId] = seq;

			SkillEffect e;
			const string& who = row[c[3]];
			if (who == "self")
				e.self = true;
			else if (who != "target")
			{
				error = where + " who must be self or target";
				return false;
			}

			if (ParseEnum(row[c[2]], EFFECT_NAMES, OUT e.type) == false
				|| ParseEnum(row[c[4]], REQUIRE_NAMES, OUT e.condition) == false
				|| ParseEnum(row[c[9]], CC_NAMES, OUT e.ccType) == false
				|| ParseEnum(row[c[15]], STAT_NAMES, OUT e.stat) == false)
			{
				error = where + " unknown effect / requires / cc_type / stat";
				return false;
			}

			if (Csv::ParseFloat(row[c[5]], OUT e.adRatio) == false || Csv::ParseFloat(row[c[6]], OUT e.apRatio) == false
				|| Csv::ParseUInt(row[c[7]], OUT e.hits) == false || Csv::ParseUInt(row[c[8]], OUT e.intervalMs) == false
				|| Csv::ParseUInt(row[c[10]], OUT e.durationMs) == false || Csv::ParseFloat(row[c[11]], OUT e.magnitude) == false
				|| Csv::ParseFloat(row[c[12]], OUT e.distCm) == false || Csv::ParseFloat(row[c[13]], OUT e.speedCms) == false
				|| ParseBool(row[c[14]], OUT e.ignoresWalls) == false || Csv::ParseFloat(row[c[16]], OUT e.perLevel) == false
				|| Csv::ParseFloat(row[c[17]], OUT e.radiusCm) == false)
			{
				error = where + " numeric column is not a number (ignores_walls is 0 or 1)";
				return false;
			}

			if (e.distCm < 0.f || e.speedCms < 0.f || e.radiusCm < 0.f || e.magnitude < 0.f || e.perLevel < 0.f)
			{
				error = where + " values must be >= 0";
				return false;
			}

			e.param = row[c[18]];
			if (e.param == "none")
				e.param.clear();

			// 피해 효과가 hits 0 으로 들어오면 "한 번" 으로 - 다른 효과는 hits 를 쓰지 않는다
			if (e.type == SkillEffectType::Damage && e.hits == 0)
				e.hits = 1;

			defIt->second.effects.push_back(e);
		}
	}

	for (const auto& [skillId, def] : defs)
	{
		if (ValidateDef(def, OUT error) == false)
			return false;
	}

	s_defs = move(defs);
	s_rules = rules;
	return true;
}

const SkillDef* SkillTable::Find(uint32 skillId)
{
	auto it = s_defs.find(skillId);
	if (it == s_defs.end())
		return nullptr;

	return &it->second;
}

// 🔴 테스트 픽스처 - 2026-10-10 로스터 이전의 네 스킬. 실제 데이터는 Load 가 읽는다
void SkillTable::Init()
{
	const string skills =
		"skill_id,name_key,power,kind,cast_ms,cooldown_ms,can_move,telegraph_ms,shape,aim,range_cm,width_cm,radius_cm,angle_deg,allowed_slot,projectile_speed_cms,pierce\n"
		"1001,skill.1001,ad,active,300,8000,0,0,single,target,400,0,0,0,1,0,0\n"
		"2001,skill.2001,none,active,60,6000,0,0,circle_self,direction,400,0,0,0,0,0,0\n"
		"3001,skill.3001,ad,active,0,5000,0,1000,circle_self,self_area,250,0,250,0,0,0,0\n"
		"9001,skill.9001,ad,active,600,8000,0,1450,circle_self,self_area,400,0,400,0,0,0,0\n";

	const string effects =
		"skill_id,seq,effect,who,requires,ad_ratio,ap_ratio,hits,interval_ms,cc_type,duration_ms,magnitude,dist_cm,speed_cms,ignores_walls,stat,per_level,radius_cm,param\n"
		"1001,1,damage,target,none,2.0,0,1,0,none,0,0,0,0,0,none,0,0,none\n"
		"1001,2,hard_cc,target,none,0,0,0,0,stun,1000,0,0,0,0,none,0,0,none\n"
		"2001,1,move,self,none,0,0,0,0,none,0,0,400,1200,0,none,0,0,dash\n"
		"3001,1,damage,target,none,1.2,0,1,0,none,0,0,0,0,0,none,0,0,none\n"
		"3001,2,soft_cc,target,none,0,0,0,0,slow,1500,0.3,0,0,0,none,0,0,none\n"
		"9001,1,damage,target,none,2.0,0,1,0,none,0,0,0,0,0,none,0,0,none\n";

	const string rules =
		"key,value\n"
		"dot_tick_ms,500\ndot_max_stacks,5\nwall_stun_ms,750\nrecast_window_ms,2000\nexecute_hp_pct,15\nblind_view_scale,0.5\n"
		"pull_stop_cm,100\npull_speed_cms,1600\ndecoy_hp,1\nprojectile_radius_cm,40\nbasic_projectile_speed_cms,2400\nconfront_range_cm,400\n"
		"mark_spear_basic_bonus,0.5\nchain_radius_cm,300\ndelayed_burst_ms,1500\nspin_max_ms,3000\nhold_max_ms,0\nbuff_per_level_default,0.03\n";

	string error;
	if (Parse(skills, effects, rules, OUT error) == false)
		LOG_LINE << "[SKILL] test fixture is broken - " << error << '\n';
}
