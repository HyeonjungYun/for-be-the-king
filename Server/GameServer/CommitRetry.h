#pragma once

/*----------------
	CommitRetry
-----------------*/

// 사망 · 탈출 · 만료 저장을 "끝날 때까지" 다시 하되, 평생 돌지 않게 하는 규칙 (ADR-0010 R-1, 사용자 결정 2026-10-08).
//
//   1. 엄격(Strict)  - 평소 문장. 메모리가 말한 아이템이 DB 에 그대로 있어야 한다
//   2. 실패를 나눈다 - 연결 끊김 · 교착 · 잠금 대기는 "다시 하면 된다"(Transient) → 같은 단계를 간격을 늘려 계속
//                     데이터 어긋남 · 중복 키 · 제약 위반은 "다시 해도 같다"(Permanent) → 관대(Lenient) 단계로
//   3. 관대(Lenient) - "그 순간 DB 에 실제로 남아 있는 그 캐릭터의 것만" 옮기고, 어긋난 것은 경보 로그로 남긴다
//   4. 관대에서도 Permanent 면 포기(Abandon) - DB 는 저장 전 그대로. 🔴 경보 로그. 여기 오는 길은 데이터가 깨진 경우뿐이다
//
// 평생 도는 경우는 "DB 가 계속 죽어 있음" 하나 - 그때는 로그인 · 저장 전부가 멈춘 서버 장애다.
// 순수 함수 - 시계 · DB 없이 단위 테스트한다 (GameServerTests CommitRetryTest).

enum class CommitOutcome : uint8
{
	Ok,
	Transient,		// 다시 하면 된다
	Permanent,		// 같은 문장으로는 영원히 안 된다
};

enum class CommitMode : uint8
{
	Strict,
	Lenient,
};

struct CommitStep
{
	enum class Kind : uint8 { Done, Retry, Abandon };

	Kind		kind = Kind::Done;
	CommitMode	mode = CommitMode::Strict;	// Retry 일 때 다음 시도의 단계
	uint32		delayMs = 0;				// Retry 일 때 기다릴 시간
};

namespace CommitRetry
{
	constexpr uint32	FIRST_DELAY_MS	= 100;
	constexpr uint32	MAX_DELAY_MS	= 5000;		// 예전 탈출 재시도와 같은 100 ms → ×2 → 5 s
	constexpr uint32	WARN_EVERY		= 12;		// Transient 재시도 12 번마다 (최대 간격이면 약 1 분) 경보 로그

	// transientRetries 번째 재시도 전에 기다릴 시간 - 100, 200, 400, … 5000 에서 멈춘다
	uint32			BackoffMs(uint32 transientRetries);

	// 이번 시도의 결과로 다음 할 일을 정한다. transientRetries = 지금까지의 Transient 재시도 횟수
	CommitStep		Next(CommitMode mode, CommitOutcome outcome, uint32 transientRetries);

	// 실패한 문장의 MySQL 오류 번호 → 결과. 연결 끊김 · Transient 는 Transient, 나머지(0 포함)는 Permanent
	CommitOutcome	FromMysqlError(uint32 mysqlErrno);
}
