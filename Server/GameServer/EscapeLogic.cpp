#include "pch.h"
#include "EscapeLogic.h"
#include "CsvTable.h"
#include <cmath>

namespace
{
	template<typename T>
	bool InRange(T v, T lo, T hi, const char* name, OUT string& error)
	{
		if (v >= lo && v <= hi)
			return true;

		ostringstream os;
		os << "escape_config " << name << " must be " << lo << ".." << hi << " (got " << v << ")";
		error = os.str();
		return false;
	}

	bool ReadEnvUInt(const char* name, OUT uint64& out)
	{
		char* raw = nullptr;
		size_t length = 0;
		if (_dupenv_s(&raw, &length, name) != 0 || raw == nullptr)
			return false;

		out = ::strtoull(raw, nullptr, 10);
		::free(raw);
		return true;
	}
}

bool EscapeConfig::Parse(const string& csv, OUT string& error)
{
	CsvTable table;
	vector<int32> c;
	if (Csv::Parse(csv, "escape_config.csv", OUT table, OUT error) == false
		|| Csv::FindColumns(table, "escape_config.csv", { "key", "value" }, OUT c, OUT error) == false)
		return false;

	for (size_t r = 0; r < table.rows.size(); r++)
	{
		const string& key = table.rows[r][c[0]];
		const string& value = table.rows[r][c[1]];
		const string where = "escape_config.csv:" + to_string(table.lineNumbers[r]) + " " + key;

		uint32 u = 0;
		float f = 0.f;
		const bool isUInt = Csv::ParseUInt(value, OUT u);
		const bool isFloat = Csv::ParseFloat(value, OUT f);

		auto needUInt = [&]() { if (isUInt == false) error = where + " must be a non-negative integer"; return isUInt; };
		auto needFloat = [&]() { if (isFloat == false) error = where + " must be a number"; return isFloat; };

		if (key == "zone_enter_radius_cm") { if (needFloat() == false) return false; zoneEnterCm = f; }
		else if (key == "zone_leave_radius_cm") { if (needFloat() == false) return false; zoneLeaveCm = f; }
		else if (key == "gauge_fill_s") { if (needUInt() == false) return false; fillUs = static_cast<uint64>(u) * 1'000'000; }
		else if (key == "gauge_reverse_mult_x100") { if (needUInt() == false) return false; reverseMultX100 = u; }
		else if (key == "gauge_alert_radius_m") { if (needFloat() == false) return false; alertCm = f * 100.f; }
		else if (key == "gauge_visible_radius_m") { if (needFloat() == false) return false; visibleCm = f * 100.f; }
		else if (key == "gauge_heartbeat_ms") { if (needUInt() == false) return false; heartbeatUs = static_cast<uint64>(u) * 1000; }
		else if (key == "gauge_dt_clamp_ms") { if (needUInt() == false) return false; dtClampUs = static_cast<uint64>(u) * 1000; }
		else if (key == "run_limit_s") { if (needUInt() == false) return false; runLimitUs = static_cast<uint64>(u) * 1'000'000; }
		else if (key == "expiry_gold_kept_pct") { if (needUInt() == false) return false; expiryGoldKeptPct = u; }
		else if (key == "telemetry_min_run_s") { if (needUInt() == false) return false; telemetryMinRunUs = static_cast<uint64>(u) * 1'000'000; }
		else if (key == "spawn_invulnerability_ms") { if (needUInt() == false) return false; spawnInvulnerabilityUs = static_cast<uint64>(u) * 1000; }
		else if (key == "escape_debug_command")
		{
			if (value != "on" && value != "off")
			{
				error = where + " must be on or off";
				return false;
			}
			debugEscapeCommand = (value == "on");
		}
		// 클라 · 집계 도구가 읽는 값 - 서버는 형식만 받아 둔다
		else if (key == "run_warning_s" || key == "telemetry_grade_points" || key == "telemetry_alert_camp_alarm_pct")
		{
		}
		else
		{
			error = where + " is not a known key";
			return false;
		}
	}

	return true;
}

bool EscapeConfig::Validate(OUT string& error) const
{
	// 하한 800 → 500 (2026-10-17 사용자 결정: 구역 반경을 2/3 로 - 667 cm)
	return InRange(zoneEnterCm, 500.f, 1100.f, "zone_enter_radius_cm", error)
		&& InRange(zoneLeaveCm, zoneEnterCm, 1200.f, "zone_leave_radius_cm (enter .. tower_clear_radius 1200)", error)
		&& InRange(fillUs, 3ull * 1'000'000, 120ull * 1'000'000, "gauge_fill (us)", error)
		&& InRange(reverseMultX100, 100u, 400u, "gauge_reverse_mult_x100", error)
		&& InRange(visibleCm, 2500.f, 3000.f, "gauge_visible_radius (cm)", error)
		&& InRange(alertCm, visibleCm, 7000.f, "gauge_alert_radius (cm, visible .. 70 m)", error)
		&& InRange(heartbeatUs, 500'000ull, 2'000'000ull, "gauge_heartbeat (us)", error)
		&& InRange(dtClampUs, 100'000ull, 500'000ull, "gauge_dt_clamp (us)", error)
		&& InRange(runLimitUs, 30ull * 1'000'000, 3600ull * 1'000'000, "run_limit (us)", error)
		&& InRange(expiryGoldKeptPct, 0u, 100u, "expiry_gold_kept_pct", error)
		&& InRange(telemetryMinRunUs, 0ull, 300ull * 1'000'000, "telemetry_min_run (us)", error)
		&& InRange(spawnInvulnerabilityUs, 0ull, 5'000'000ull, "spawn_invulnerability (us, 0 .. 5 s)", error);
}

bool EscapeConfig::Load(const string& dataDir, OUT string& error)
{
	*this = EscapeConfig();

	string text;
	if (Csv::ReadFile(dataDir + "\\escape_config.csv", OUT text) && Parse(text, OUT error) == false)
		return false;

	// 파일의 제품 값은 안전 범위 안이어야 한다. 시험용 환경 변수는 그 뒤에 덮는다 - 30 초 런 · 짧은 게이지 (AC-ET-20 · 28)
	if (runLimitUs < 600ull * 1'000'000 || fillUs < 30ull * 1'000'000)
	{
		error = "escape_config run_limit_s must be >= 600 and gauge_fill_s >= 30 in the file (use FBTK_RUN_LIMIT_S / FBTK_ESCAPE_FILL_S for tests)";
		return false;
	}

	uint64 v = 0;
	if (ReadEnvUInt("FBTK_RUN_LIMIT_S", OUT v))
		runLimitUs = v * 1'000'000;
	if (ReadEnvUInt("FBTK_ESCAPE_FILL_S", OUT v))
		fillUs = v * 1'000'000;
	if (ReadEnvUInt("FBTK_DEBUG_ESCAPE", OUT v))
		debugEscapeCommand = (v != 0);

	return Validate(OUT error);
}

EscapeConfig& EscapeConfig::Get()
{
	static EscapeConfig s_config;
	return s_config;
}

bool EscapeLogic::InZone(bool wasIn, float distCm, const EscapeConfig& cfg)
{
	if (std::isfinite(distCm) == false)
		return false;

	return wasIn ? (distCm <= cfg.zoneLeaveCm) : (distCm <= cfg.zoneEnterCm);
}

EscapeLogic::Audience EscapeLogic::AudienceFor(bool inZone, float distCm, const EscapeConfig& cfg)
{
	if (inZone)
		return Audience::Full;

	if (std::isfinite(distCm) == false)
		return Audience::None;

	if (distCm <= cfg.visibleCm)
		return Audience::Full;

	if (distCm <= cfg.alertCm)
		return Audience::Alert;

	return Audience::None;
}

EscapeLogic::StepResult EscapeLogic::Step(Gauge& gauge, int32 occupants, uint64 dtUs, const EscapeConfig& cfg)
{
	StepResult r;
	r.before = gauge.state;

	const uint64 dt = min(dtUs, cfg.dtClampUs);

	if (occupants > 0)
	{
		gauge.progressUs += dt;
		gauge.state = Protocol::ESCAPE_GAUGE_FILLING;

		if (gauge.progressUs >= cfg.fillUs)
		{
			// 완충 - 점유자 전원 탈출, 쿨다운 없이 바로 0 (Core Rule 9)
			gauge.progressUs = 0;
			gauge.state = Protocol::ESCAPE_GAUGE_IDLE;
			r.completed = true;
			r.after = Protocol::ESCAPE_GAUGE_COMPLETED;
			return r;
		}
	}
	else if (gauge.progressUs > 0)
	{
		const uint64 drain = dt * cfg.reverseMultX100 / 100;
		gauge.progressUs = (drain >= gauge.progressUs) ? 0 : gauge.progressUs - drain;
		gauge.state = (gauge.progressUs == 0) ? Protocol::ESCAPE_GAUGE_IDLE : Protocol::ESCAPE_GAUGE_DRAINING;
	}
	else
	{
		gauge.state = Protocol::ESCAPE_GAUGE_IDLE;
	}

	r.after = gauge.state;
	return r;
}

uint32 EscapeLogic::Permille(const Gauge& gauge, const EscapeConfig& cfg)
{
	if (cfg.fillUs == 0)
		return 0;

	return static_cast<uint32>(min<uint64>(1000, gauge.progressUs * 1000 / cfg.fillUs));
}

float EscapeLogic::RatePerSecond(Protocol::EscapeGaugeState state, const EscapeConfig& cfg)
{
	const float fillS = static_cast<float>(cfg.fillUs) / 1'000'000.f;
	if (fillS <= 0.f)
		return 0.f;

	switch (state)
	{
	case Protocol::ESCAPE_GAUGE_FILLING:	return 1.f / fillS;
	case Protocol::ESCAPE_GAUGE_DRAINING:	return -(cfg.reverseMultX100 / 100.f) / fillS;
	default:								return 0.f;
	}
}

uint64 EscapeLogic::GoldKept(uint64 gold, uint32 pct)
{
	pct = min(pct, 100u);
	return (gold / 100) * pct + (gold % 100) * pct / 100;
}

uint64 EscapeLogic::RemainingUs(uint64 deadlineUs, uint64 nowUs)
{
	return (deadlineUs > nowUs) ? deadlineUs - nowUs : 0;
}

bool EscapeLogic::Expired(uint64 deadlineUs, uint64 nowUs)
{
	return deadlineUs != 0 && nowUs >= deadlineUs;
}
