#pragma once

/*--------------
	CheatGuard
--------------*/

// 안티치트 위반 기록 (design/quick-specs/anticheat-2026-10-19.md · ADR-0019).
//
// 서버는 이미 이동 · 공격 · 스킬 · 루팅을 검증하고 거부한다. 문제는 거부가 끝이었다는 것 - 조작 클라는 되돌림을 맞고
// 몇 번이든 다시 보낼 수 있었다. 여기서는 거부를 "위반" 으로 모아 점수로 만들고, 점수가 기준을 넘으면 경고 → 끊기로 올린다.
// 끊긴 기록(strike)은 DB 에 남고, 창 안에서 쌓이면 계정을 정지한다 (Room 이 DB 잡을 건다 - 이 파일은 순수 계산만).
//
//  - 점수는 위반마다 가중치만큼 오르고 초당 decayPerSec 씩 내려간다. 렉으로 가끔 되돌림을 맞는 정상 플레이어는 쌓이지 않는다
//  - 가중치 0 인 종류는 세기만 한다 (통계 · 로그용). 정상 클라에서도 흔한 거부(평타 사거리 경계 등)는 낮게 둔다
//  - 값은 전부 Server/Data/anticheat.csv - 없으면 아래 기본값
//
// 방 스레드에서만 쓴다 (Player 가 하나씩 갖는다) - 락이 없다.

enum class Violation : uint8
{
	MoveSpeed = 0,		// 이동 예산 초과 (속도핵 · 순간이동)
	MoveWall,			// 벽 통과
	MoveBounds,			// 층 밖 · NaN 좌표
	MoveClock,			// client_ms 가 서버 간격보다 크게 앞섰다 (MoveClock 이 잘랐다)
	MoveWhileLocked,	// CC · 후딜 중 이동
	AttackRange,		// 평타 사거리 밖
	AttackWall,			// 벽 너머 평타
	AttackHidden,		// 이 클라가 모르는(안개 · 은신) 대상 평타
	SkillCooldown,		// 쿨다운 전 스킬
	SkillRange,			// 대상 지정 스킬 사거리 밖
	SkillWall,			// 벽 너머 대상 지정
	SkillHidden,		// 모르는 대상 지정
	LootRange,			// 루팅 거리 밖 · 벽 너머 가방
	LootHidden,			// 모르는 가방
	ActionWhileLocked,	// 시전 · CC 중 장비 교체 등
	Count
};

const char* ViolationName(Violation v);

struct CheatRules
{
	float		weights[static_cast<size_t>(Violation::Count)] = {};
	float		decayPerSec = 2.f;			// 초당 내려가는 점수
	float		warnScore = 20.f;			// [CHEAT] warn 로그 (한 번 넘을 때마다 한 줄)
	float		kickScore = 60.f;			// 끊는다 + DB 에 strike 한 줄
	uint32		strikesForBan = 3;			// banWindowHours 안의 strike 가 이만큼이면 정지
	uint32		banWindowHours = 24;
	uint32		banMinutes = 60;			// 0 = 정지하지 않는다 (끊기만)
	bool		enabled = true;				// false = 세기만 하고 끊지 않는다 (관찰 모드)

	CheatRules();

	float		Weight(Violation v) const { return weights[static_cast<size_t>(v)]; }

	// anticheat.csv (key,value). 위반 가중치 키는 weight_<이름> (예: weight_move_speed). 모르는 키 · 범위 밖은 false
	bool		Parse(const string& csv, OUT string& error);
	bool		Load(const string& dataDir, OUT string& error);

	static CheatRules& Get();
};

enum class CheatAction : uint8
{
	None,
	Warn,		// 경고 기준을 방금 넘었다 - 로그
	Kick,		// 끊기 기준을 넘었다 - 끊고 strike 를 남긴다. 한 번 Kick 을 돌려준 뒤엔 다시 None (같은 세션)
};

class CheatGuard
{
public:
	// 위반 하나 - 할 일을 돌려준다
	CheatAction		Report(Violation v, uint64 nowUs, const CheatRules& rules = CheatRules::Get());

	float			Score(uint64 nowUs, const CheatRules& rules = CheatRules::Get());
	uint32			Count(Violation v) const { return _counts[static_cast<size_t>(v)]; }
	uint32			Total() const;
	bool			Kicked() const { return _kicked; }

	// "move_speed:5 move_wall:2" - 많은 순 상위 n 개 (로그 · DB detail)
	string			Summary(size_t n = 4) const;

	void			Reset() { *this = CheatGuard(); }

private:
	void			Decay(uint64 nowUs, const CheatRules& rules);

	float			_score = 0.f;
	uint64			_lastUs = 0;
	bool			_warned = false;
	bool			_kicked = false;
	uint32			_counts[static_cast<size_t>(Violation::Count)] = {};
};

namespace CheatBan
{
	// strike 들(최근 것부터 아무 순서)로 정지 여부 - 창 안의 strike 수가 기준 이상이면 정지 끝 시각(unix s), 아니면 0
	uint64			BanUntil(const vector<uint64>& strikeUnixSecs, uint64 nowUnixSec, const CheatRules& rules = CheatRules::Get());
}
