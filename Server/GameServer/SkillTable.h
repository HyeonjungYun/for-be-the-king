#pragma once
#include "Enum.pb.h"

/*-----------------
	SkillTable
------------------*/

// Server/Data/skills.csv + skill_effects.csv + skill_rules.csv — 스킬 로스터 (src/스킬_목록.csv 76개 + 몬스터 9000번대).
// 클라 S1SkillTable 이 같은 세 파일을 읽는다 — 사거리 · 시전 · 쿨다운 · 이동 거리가 어긋나면 서버 값이 진실이다.
// 수치는 전부 여기(데이터)에 있고 코드에는 없다 (사용자 결정 2026-10-10 "모든 수치는 일괄적으로 정하고 나중에 수치만 조정").
// 판정 · 효과 의미는 design/quick-specs/skill-roster-2026-10-10.md.

enum class SkillKind : uint8
{
	Active = 0,		// 키를 누르면 조준 → 발동
	Passive,		// 키를 차지하지만 누르면 아무 일 없다. 효과는 조건(평타 적중 · 치명상)에 발동
	Hold,			// 누르면 시작, 같은 키를 다시 누르면 끝 (빈쿨룸 · 망치 회오리 · 정좌)
	Recast			// 발동 뒤 recast_window_ms 안에 다시 누르면 requires=recast 효과가 나간다
};

enum class SkillShape : uint8
{
	None = 0,		// 대상이 없다 (자기 버프 · 패시브)
	Single,
	Line,
	Cone,
	CircleSelf,
	CirclePoint,
	Projectile		// 날아가며 피할 수 있다. 0 단계에서는 Line 처럼 즉시 판정 (2 단계에서 실제 투사체)
};

enum class SkillAimType : uint8
{
	None = 0,
	SelfArea,
	Target,
	Direction,
	Point
};

enum class SkillPower : uint8
{
	None = 0,		// 피해 없음 - 분류 표시만
	Ad,
	Ap,
	Hybrid
};

enum class SkillEffectType : uint8
{
	Damage = 0,
	HardCc,
	SoftCc,
	Knockback,
	Pull,			// 하드 CC - 벽에 막히고 시전자 바로 앞(pull_stop_cm)까지
	Movement,		// param: dash · leap · back · forward · blink
	SelfBuff,		// stat 을 magnitude + per_level × 아이템 강화 레벨 만큼, duration 동안. 같은 버프는 갱신
	Dot,			// param: bleed · poison · burn · heavy_bleed. 같은 종류도 중첩 (rules dot_max_stacks)
	Mark,			// param: spear · arrow · blood · heavy_bleed · rope. 본인 것만 · 갱신
	Execute,		// magnitude = 최대 체력 % 이하면 즉시 처치 (몬스터 · 보스 제외)
	Zone,			// param: poison · fire_path · element_path · mist · arrows. 겹치면 피해 중첩
	Chain,			// hits = 최대 옮겨 가는 횟수, radius_cm = 옮겨 가는 거리
	Delayed,		// duration 뒤 터진다 - 피해 + radius 안으로 퍼지는 dot
	Decoy,			// duration 동안 시전자 복제 (magnitude = 체력)
	Invuln,
	Counter,		// duration 동안 평타 · 대상 지정 · 직선을 막고 ad_ratio 로 반격
	Parry,			// duration 동안 전방 투사체 · 근접을 받아넘긴다
	Hold,			// param: spin(피해 틱 + 넉백, duration = 상한) · chains(범위 속박 + 자기 속박, 상한 없음)
	Stance,			// param: fire · lightning · clear - 스태프 속성. 지속 없음 · 교체
	OnHit,			// 패시브 - 평타 적중마다 추가 피해 (ap_ratio) param 속성
	LethalSave,		// 패시브 - 치명상을 막고 뒤로 도약 (dist · speed). duration = 쿨다운
	Reveal,			// 1 단계 제외 (FoW 없음)
	Trail,			// 과다출혈 핏자국 - 시전자가 밟으면 stat 버프
	SelfCc,			// 시전자에게 CC. requires=wall(회오리가 벽에 닿음) · no_element(속성 없이 끝남) param at_end
	PetrifyWindow,	// duration 동안 단검에 맞은 적을 창이 끝날 때 magnitude ms 기절
	FallOnHit,		// 도약 중 맞으면 추락
	Throw,			// 대상을 시전자 뒤로 dist 만큼 던지고 radius 안 적에게 ad_ratio

	// ── 모자 로스터 #78 ~ #104 (2026-10-10) ──
	Immune,			// self - param 속성(fire · lightning · poison) 피해 · 상태 면역. duration 0 = 패시브(장착 중 내내), 아니면 버프
	DamageMod,		// self - 피해 배율 +magnitude. param 선택자: tag:<태그> · elem:<속성> · skill:<id> · range (long_range_cm 밖). duration 0 = 패시브
	Cleanse,		// self - param poison / burn / all(CC + 지속 피해) 해제
	Heal,			// self - 최대 체력 × magnitude 회복 (치유 감소 적용). 패시브 트리거(requires on_dot:…)와 함께
	ExtendDot,		// target - param 피해 종류(magic)의 지속 피해 남은 시간 +magnitude ms
	Doom,			// self - 배수진: duration 동안 면역, 적 플레이어 처치면 해제 + 만피, 아니면 끝에 죽는다
	DamageReduceOnce,	// self 패시브 - 받는 첫 공격 × (1 - magnitude), duration = 재발동 쿨다운

	// ── 갑옷 로스터 #106 ~ #114 (2026-10-10 오후) ──
	Reflect,		// self - duration 동안 날아오는 스킬 투사체를 쏜 쪽으로 되돌려 보낸다 (자업자득). 평타 투사체는 못 받아친다

	// ── FoW (2026-10-11) ──
	Vision,			// self 패시브 - 시야 반경 × (1 + magnitude), 카메라도 같은 배율 (천리안 1132)

	// ── 2차 로스터 (게임 ID 1115 ~ 1137 · FoW 의존 기존 스킬, 2026-10-11) ──
	Stealth,		// self - duration 동안 은신 (FoW). param break_on_damage 면 피해로도 풀린다 (그림자 감추기 · 니힐)
	VisionArea,		// self - 시전 지점 둘레 radius 를 duration 동안 본다. param walls(벽 너머) · stealth(은신까지) (맹수의 탐색)
	Hawk,			// self - dist 안 가장 가까운 적 둘레 radius 를 duration 동안 본다(벽 너머). 없으면 층에서 가장 가까운 적을 interval_ms 동안 공개 (종이매)
	Scent,			// self - duration 동안 몬스터가 표적으로 삼지 않는다 (야생의 체취)
	Shield,			// self - 최대 체력 × magnitude 만큼 피해를 먼저 막는 보호막, duration 동안 (전사의 다짐)
	Volley,			// self - 이 스킬의 투사체를 hits(+ per_level × 강화) 발. magnitude = 퍼짐 각(°), interval_ms > 0 이면 한 발씩 바라보는 방향으로, duration_ms = 첫 발까지 지연
	Clone,			// self - 시전자 모습의 분신이 조준 방향으로 dist 를 speed 로 걷는다. radius = 분신 시야(주인과 공유), duration = 최대 수명. 맞으면 사라진다 (그림자 보내기)
	RaiseWall		// self - 조준 방향으로 길이 dist · 두께 magnitude 의 벽을 duration 동안 세운다 (시작은 radius_cm 앞). 이동 · 공격 · 시선을 막는다 (지각변동)
};

enum class SkillRequire : uint8
{
	None = 0,
	MarkSpear,
	MarkArrow,
	MarkBlood,
	MarkHeavyBleed,
	MarkRope,
	Stunned,
	Element,
	NoElement,
	Hit,			// 이 스킬이 하나라도 맞혔다
	NoKill,			// 처치하지 못했다
	Recast,			// 두 번째 입력
	Wall,			// 유지형이 벽에 닿았다

	// 패시브 트리거 · 사용 조건 (모자 로스터)
	OnDot,			// on_dot:<kind> - 내가 그 종류 지속 피해를 걸었을 때 (bleed 는 heavy_bleed 포함, 그때 magnitude × 2)
	OnDashAtMe,		// on_dash - 적이 나를 향해 질주해 왔을 때 (그 적이 대상)
	OnSkill,		// on_skill:<id> - 내 그 스킬이 피해를 입혔을 때 (맞은 적이 대상)
	Alone,			// alone - 반경(radius_cm) 안에 아무도 없을 때만 사용 가능
	OnBasic			// on_basic - 이 스킬의 basic_proc 버프가 켜진 동안 내 평타가 맞을 때마다 (맞은 적이 대상, radius_cm > 0 이면 그 둘레 적 전부)
};

enum class SkillBuffStat : uint8
{
	None = 0,
	AttackPower,
	MagicPower,
	CritChance,
	AttackSpeed,
	MoveSpeed,
	Armor,
	MagicResist,
	DamagePct,
	WallPierce,		// 유령 신부의 축복 - 평타 · 스킬이 벽을 무시한다 (크기는 의미 없음)
	Immune,			// 버프 형태의 면역 - BuffInstance.param 이 속성
	DamageMod,		// 버프 형태의 피해 배율 - BuffInstance.param 이 선택자

	// ── 평타 모드 · 2차 로스터 (2026-10-11) ── 크기의 뜻이 스탯마다 다르다
	AttackRange,	// 평타 사거리 + magnitude cm (음수 = 줄어든다). 명경지수 · 통아 · 육량전
	BasicOnly,		// 그동안 스킬을 쓸 수 없다 - 평타만 (통아 · 육량전 · 은장도)
	BasicProc,		// 그 버프 스킬의 on_basic 효과가 평타 적중마다 (혼원일체 · 여진)
	BasicDash,		// 평타 = 이동 베기 - 사거리 밖 magnitude cm 안의 대상에게 질주해 벤다 (일보일검)
	BasicSpin,		// 평타 = 사방 베기 - 적중 때 나를 중심으로 magnitude cm 안의 적 전부 (난무)
	BasicMelee,		// 평타 = 근접 - 투사체 대신 magnitude cm 사거리로 벤다 (푸리아 · 은장도)
	LowHpBonus,		// 평타 피해 × (1 + magnitude × 잃은 체력 비율) (은장도)
	Thorns			// 나를 근접 평타로 때린 적에게 받은 피해 × magnitude 를 마법 피해로 되돌린다 (고슴도치)
};

struct SkillEffect
{
	SkillEffectType			type = SkillEffectType::Damage;
	bool					self = false;			// who = self (아니면 판정 대상)
	SkillRequire			condition = SkillRequire::None;
	uint32					conditionId = 0;		// on_skill:<id>
	string					conditionParam;			// on_dot:<kind>

	float					adRatio = 0.f;
	float					apRatio = 0.f;
	uint32					hits = 1;
	uint32					intervalMs = 0;

	Protocol::CcType		ccType = Protocol::CC_TYPE_NONE;
	uint32					durationMs = 0;
	float					magnitude = 0.f;

	float					distCm = 0.f;
	float					speedCms = 0.f;
	bool					ignoresWalls = false;

	SkillBuffStat			stat = SkillBuffStat::None;
	float					perLevel = 0.f;
	float					radiusCm = 0.f;
	string					param;

	// 피해 종류 - 물리 계수만 있으면 물리, 아니면 마법 (S3: 항마다 따로 완화)
	Protocol::DamageType	DamageType() const { return (adRatio > 0.f && apRatio <= 0.f) ? Protocol::DAMAGE_TYPE_PHYSICAL : Protocol::DAMAGE_TYPE_MAGIC; }
};

struct SkillDef
{
	uint32					skillId = 0;
	uint32					effectId = 0;			// = skillId. 클라 연출 키
	string					nameKey;

	SkillPower				power = SkillPower::None;
	SkillKind				kind = SkillKind::Active;

	uint32					castMs = 0;
	uint32					cooldownMs = 0;
	bool					canMoveWhileCasting = false;
	uint32					telegraphMs = 0;

	SkillShape				shape = SkillShape::None;
	SkillAimType			aimType = SkillAimType::None;
	float					rangeCm = 0.f;
	float					widthCm = 0.f;
	float					radiusCm = 0.f;
	float					angleDeg = 0.f;
	float					projectileSpeedCms = 0.f;
	bool					pierce = false;

	Protocol::EquipSlot		allowedSlot = Protocol::SLOT_NONE;

	vector<SkillEffect>		effects;
	vector<string>			tags;					// skills.csv tags 열 (세미콜론 구분) - damage_mod tag:<태그> 가 본다
	string					requiresWeapon;			// skills.csv requires_weapon 열 - "hammer" 등 무기 풀 id. 비면 조건 없음 (갑옷 #106 ~ #114)
	string					requiresWeaponTag;		// "sword:ghost" 의 ghost - 무기 칸 스킬 중 하나가 그 태그를 가져야 한다 (#113)

	bool HasHardCc() const;
	bool HasMovement() const;
	bool HasTag(const string& tag) const;
	const SkillEffect* FindEffect(SkillEffectType type) const;
};

// skill_rules.csv - 스킬 행에 자리가 없는 전역 수치. 기본값은 코드가 아니라 파일에 있다 - 키가 빠지면 기동을 거부한다
struct SkillRules
{
	uint32	dotTickMs = 0;
	uint32	dotMaxStacks = 0;
	uint32	wallStunMs = 0;
	uint32	recastWindowMs = 0;
	float	executeHpPct = 0.f;
	float	blindViewScale = 0.f;
	float	pullStopCm = 0.f;
	float	pullSpeedCms = 0.f;
	int32	decoyHp = 0;
	float	projectileRadiusCm = 0.f;
	float	basicProjectileSpeedCms = 0.f;
	float	confrontRangeCm = 0.f;
	float	markSpearBasicBonus = 0.f;
	float	chainRadiusCm = 0.f;
	uint32	delayedBurstMs = 0;
	uint32	spinMaxMs = 0;
	uint32	holdMaxMs = 0;			// 0 = 상한 없음 (빈쿨룸)
	float	buffPerLevelDefault = 0.f;
	uint32	trailBuffMs = 0;		// 핏자국을 밟았을 때 이속 버프 길이 (4 단계)
	float	trailPickupCm = 0.f;	// 핏자국을 "밟았다" 로 보는 거리
	float	tauntSpeedCms = 0.f;	// 도발 - 시전자에게 끌려가는 속도 (느리게). 서버가 옮긴다
	float	tauntStopCm = 0.f;		// 도발 - 시전자 앞 이만큼에서 멈춘다
	float	longRangeCm = 0.f;		// 명사수 - 이 거리 밖의 적에게 damage_mod range 가 붙는다
	float	basicDashSpeedCms = 0.f;	// 일보일검 - 이동 베기 질주 속도
};

class SkillTable
{
public:
	// 실패하면 이유를 error 에 남기고 false. 표는 빈 채로 둔다. ItemTable 보다 먼저 읽는다 (풀이 스킬 id 를 검사한다)
	static bool				Load(const string& dataDir, OUT string& error);
	static bool				Parse(const string& skillsCsv, const string& effectsCsv, const string& rulesCsv, OUT string& error);

	// 🔴 테스트 전용 - 코드에 박힌 작은 픽스처 (1001 · 2001 · 3001 · 9001). 실제 로스터가 아니다.
	//    GameServerTests 의 아이템 · 롤러 · 장비 테스트가 이 네 id 를 전제로 쓴다. 부팅은 Load 만 쓴다
	static void				Init();

	static const SkillDef*	Find(uint32 skillId);
	static const SkillRules& Rules() { return s_rules; }

	static const unordered_map<uint32, SkillDef>& All() { return s_defs; }

	static const char*		EffectName(SkillEffectType type);
	static const char*		BuffStatName(SkillBuffStat stat);		// skill_effects.csv stat 열 이름 그대로 - S_STATUS param

private:
	static unordered_map<uint32, SkillDef> s_defs;
	static SkillRules s_rules;
};
