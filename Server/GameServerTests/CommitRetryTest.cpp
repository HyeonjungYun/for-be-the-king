#include "pch.h"
#include "CommitRetry.h"
#include "DBError.h"

// 런 끝 저장 재시도 규칙 (ADR-0010 R-1 · 사용자 결정 2026-10-08 "끝날 때까지 다시 하되 평생 돌지 않게")
// 순수 함수만 - DB 없이. 실제 DB 경로는 실서버에서 연결 강제 종료 · 잠금 · 데이터 어긋남으로 확인한다 (evidence 문서)

TEST(CommitRetry, 일시_실패는_같은_단계를_간격을_늘려_다시_한다)
{
	const CommitStep first = CommitRetry::Next(CommitMode::Strict, CommitOutcome::Transient, 0);
	EXPECT_EQ(first.kind, CommitStep::Kind::Retry);
	EXPECT_EQ(first.mode, CommitMode::Strict) << "일시 실패로 관대 단계에 가면 안 된다 - 남의 것을 덜 옮기는 길은 데이터가 깨졌을 때만";
	EXPECT_EQ(first.delayMs, 100u);

	EXPECT_EQ(CommitRetry::BackoffMs(1), 200u);
	EXPECT_EQ(CommitRetry::BackoffMs(2), 400u);
	EXPECT_EQ(CommitRetry::BackoffMs(5), 3200u);
	EXPECT_EQ(CommitRetry::BackoffMs(6), 5000u) << "5 s 에서 멈춘다";
	EXPECT_EQ(CommitRetry::BackoffMs(1000), 5000u) << "오래 막혀도 넘치지 않는다";

	// 관대 단계의 일시 실패도 관대 단계 그대로
	const CommitStep lenient = CommitRetry::Next(CommitMode::Lenient, CommitOutcome::Transient, 3);
	EXPECT_EQ(lenient.kind, CommitStep::Kind::Retry);
	EXPECT_EQ(lenient.mode, CommitMode::Lenient);
	EXPECT_EQ(lenient.delayMs, 800u);
}

TEST(CommitRetry, 영구_실패는_엄격에서_관대로_관대에서는_포기로_반드시_끝난다)
{
	const CommitStep toLenient = CommitRetry::Next(CommitMode::Strict, CommitOutcome::Permanent, 7);
	EXPECT_EQ(toLenient.kind, CommitStep::Kind::Retry);
	EXPECT_EQ(toLenient.mode, CommitMode::Lenient);
	EXPECT_EQ(toLenient.delayMs, 0u) << "기다려도 같은 결과라 바로";

	const CommitStep abandon = CommitRetry::Next(CommitMode::Lenient, CommitOutcome::Permanent, 0);
	EXPECT_EQ(abandon.kind, CommitStep::Kind::Abandon);

	// 영구 실패만 계속 나오는 저장은 두 번 만에 끝난다 - 평생 돌지 않는다
	CommitMode mode = CommitMode::Strict;
	int32 attempts = 0;
	for (; attempts < 10; attempts++)
	{
		const CommitStep step = CommitRetry::Next(mode, CommitOutcome::Permanent, 0);
		if (step.kind != CommitStep::Kind::Retry)
			break;
		mode = step.mode;
	}
	EXPECT_EQ(attempts, 1);
}

TEST(CommitRetry, 성공하면_그_단계에서_끝난다)
{
	EXPECT_EQ(CommitRetry::Next(CommitMode::Strict, CommitOutcome::Ok, 0).kind, CommitStep::Kind::Done);

	const CommitStep lenientDone = CommitRetry::Next(CommitMode::Lenient, CommitOutcome::Ok, 4);
	EXPECT_EQ(lenientDone.kind, CommitStep::Kind::Done);
	EXPECT_EQ(lenientDone.mode, CommitMode::Lenient) << "관대 단계로 끝났다는 것을 부른 쪽이 안다(로그)";
}

TEST(DBError, 연결_끊김_일시_영구를_오류_번호로_나눈다)
{
	using DBError::Kind;

	// 연결 끊김 - 다시 붙여야 한다 (KILL CONNECTION 뒤 첫 쿼리는 2013, 그다음은 2006)
	EXPECT_EQ(DBError::Classify(2006), Kind::ConnectionLost);		// CR_SERVER_GONE_ERROR
	EXPECT_EQ(DBError::Classify(2013), Kind::ConnectionLost);		// CR_SERVER_LOST
	EXPECT_EQ(DBError::Classify(2003), Kind::ConnectionLost);		// CR_CONN_HOST_ERROR - 재연결이 실패한 커넥션
	EXPECT_EQ(DBError::Classify(1927), Kind::ConnectionLost);		// ER_CONNECTION_KILLED
	EXPECT_EQ(DBError::Classify(4031), Kind::ConnectionLost);		// 오래 놀아 서버가 끊음
	EXPECT_TRUE(DBError::IsConnectionLost(2055));

	// 일시 - 연결은 살아 있다
	EXPECT_EQ(DBError::Classify(1205), Kind::Transient);			// 잠금 대기 초과
	EXPECT_EQ(DBError::Classify(1213), Kind::Transient);			// 교착
	EXPECT_FALSE(DBError::IsConnectionLost(1213));

	// 영구 - 같은 문장은 영원히 실패
	EXPECT_EQ(DBError::Classify(1062), Kind::Permanent);			// 중복 키
	EXPECT_EQ(DBError::Classify(1452), Kind::Permanent);			// 외래 키
	EXPECT_EQ(DBError::Classify(3819), Kind::Permanent);			// CHECK 제약 (chk_owner_xor_bag)
	EXPECT_EQ(DBError::Classify(1064), Kind::Permanent);			// 문법
}

TEST(CommitRetry, MySQL_오류를_재시도_결과로_옮긴다)
{
	EXPECT_EQ(CommitRetry::FromMysqlError(2013), CommitOutcome::Transient) << "연결 끊김은 다시 붙여 다시 한다";
	EXPECT_EQ(CommitRetry::FromMysqlError(1213), CommitOutcome::Transient);
	EXPECT_EQ(CommitRetry::FromMysqlError(1062), CommitOutcome::Permanent);
	EXPECT_EQ(CommitRetry::FromMysqlError(0), CommitOutcome::Permanent) << "DB 오류가 아니면(데이터 어긋남 등) 다시 해도 같다";
}
