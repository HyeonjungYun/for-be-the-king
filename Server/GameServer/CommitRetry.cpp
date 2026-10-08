#include "pch.h"
#include "CommitRetry.h"
#include "DBError.h"

uint32 CommitRetry::BackoffMs(uint32 transientRetries)
{
	uint32 delay = FIRST_DELAY_MS;
	for (uint32 i = 0; i < transientRetries && delay < MAX_DELAY_MS; i++)
		delay *= 2;

	return min(delay, MAX_DELAY_MS);
}

CommitStep CommitRetry::Next(CommitMode mode, CommitOutcome outcome, uint32 transientRetries)
{
	CommitStep step;

	switch (outcome)
	{
	case CommitOutcome::Ok:
		step.kind = CommitStep::Kind::Done;
		step.mode = mode;
		return step;

	case CommitOutcome::Transient:
		// 같은 단계를 다시 - 연결이 살아나거나 잠금이 풀리면 끝난다
		step.kind = CommitStep::Kind::Retry;
		step.mode = mode;
		step.delayMs = BackoffMs(transientRetries);
		return step;

	case CommitOutcome::Permanent:
	default:
		if (mode == CommitMode::Strict)
		{
			// 기다려도 같다 - 곧바로 관대 단계로
			step.kind = CommitStep::Kind::Retry;
			step.mode = CommitMode::Lenient;
			step.delayMs = 0;
			return step;
		}

		step.kind = CommitStep::Kind::Abandon;
		step.mode = mode;
		return step;
	}
}

CommitOutcome CommitRetry::FromMysqlError(uint32 mysqlErrno)
{
	if (mysqlErrno == 0)
		return CommitOutcome::Permanent;		// DB 오류가 아니다 - 같은 일을 다시 해도 같다

	return (DBError::Classify(mysqlErrno) == DBError::Kind::Permanent) ? CommitOutcome::Permanent : CommitOutcome::Transient;
}
