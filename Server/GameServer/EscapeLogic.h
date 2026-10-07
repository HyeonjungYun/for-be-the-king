#pragma once

/*----------------
	EscapeLogic
----------------*/

// 탈출 타워 게이지 · 구역 판정 · 골드 분할 · 설정 (design/gdd/escape-timer.md).
// Room 을 모르는 순수 로직 - 경과 시간 · 점유자 수 · 거리를 인자로 받는다 (EscapeLogicTest.cpp, AC-ET-01 ~).

struct EscapeConfig
{
	float		zoneEnterCm = 1000.f;			// R_enter - 경계 포함 들어옴
	float		zoneLeaveCm = 1100.f;			// R_leave - 이 거리를 넘어야 나감 (이력 1 m)
	uint64		fillUs = 60'000'000;			// T_fill - 인원 무관 60 초
	uint32		reverseMultX100 = 200;			// M_rev × 100 - 아무도 없으면 2 배로 역행
	float		visibleCm = 2500.f;				// R_vis - 진행도 전체가 보이는 거리
	float		alertCm = 4000.f;				// R_alert - 시작 / 끝 알림만
	uint64		heartbeatUs = 1'000'000;		// 채움 · 역행 중 보정 주기
	uint64		dtClampUs = 250'000;			// 한 틱에 반영하는 경과 상한 - 스톨 뒤 게이지가 튀지 않게
	uint64		runLimitUs = 1'800'000'000;		// 체류 30 분
	uint32		expiryGoldKeptPct = 30;
	bool		debugEscapeCommand = false;		// /escape 즉시 탈출 - 꺼져 있어야 P5 가 산다
	uint64		telemetryMinRunUs = 60'000'000;

	// escape_config.csv (key,value). 모르는 키 · 범위 밖 값은 error 를 남기고 false. 없는 키는 기본값
	bool		Parse(const string& csv, OUT string& error);

	// 범위 · 관계 검사 (§ Tuning Knobs 안전 범위, Core Rule 17)
	bool		Validate(OUT string& error) const;

	// 파일이 없으면 기본값 그대로 true. 그다음 FBTK_RUN_LIMIT_S · FBTK_ESCAPE_FILL_S · FBTK_DEBUG_ESCAPE 가 우선한다 (AC-ET-28)
	bool		Load(const string& dataDir, OUT string& error);

	static EscapeConfig& Get();
};

namespace EscapeLogic
{
	// F2 이력 판정 - 들어올 때는 R_enter 이하, 이미 안이면 R_leave 초과여야 나간다. NaN · Inf 는 밖
	bool		InZone(bool wasIn, float distCm, const EscapeConfig& cfg);

	// F3 받는 사람 - 2 = 진행도 전체(구역 안 · R_vis 이내), 1 = 경보만(R_alert 이내), 0 = 없음
	enum class Audience : uint8 { None = 0, Alert = 1, Full = 2 };
	Audience	AudienceFor(bool inZone, float distCm, const EscapeConfig& cfg);

	struct Gauge
	{
		uint64						progressUs = 0;
		Protocol::EscapeGaugeState	state = Protocol::ESCAPE_GAUGE_IDLE;
	};

	struct StepResult
	{
		Protocol::EscapeGaugeState	before = Protocol::ESCAPE_GAUGE_IDLE;
		Protocol::EscapeGaugeState	after = Protocol::ESCAPE_GAUGE_IDLE;
		bool						completed = false;	// 이 틱에 완충 - 점유자 전원 탈출, 게이지 0
	};

	// F1 한 걸음. dtUs 는 dtClampUs 로 자른다. 점유자가 있으면 채우고, 없으면 2 배로 역행, 0 에서 멈춘다
	StepResult	Step(Gauge& gauge, int32 occupants, uint64 dtUs, const EscapeConfig& cfg);

	uint32		Permille(const Gauge& gauge, const EscapeConfig& cfg);
	float		RatePerSecond(Protocol::EscapeGaugeState state, const EscapeConfig& cfg);

	// F5 - 만료 때 계정에 남는 골드. ⌊g · pct / 100⌋ 와 같고 g 가 uint64 전체 범위여도 넘치지 않는다
	uint64		GoldKept(uint64 gold, uint32 pct);

	// F4 - 남은 시간 (0 에서 멈춘다) · 만료 여부 (마감 시각 자체가 만료)
	uint64		RemainingUs(uint64 deadlineUs, uint64 nowUs);
	bool		Expired(uint64 deadlineUs, uint64 nowUs);
}
