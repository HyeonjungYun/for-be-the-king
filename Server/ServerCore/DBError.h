#pragma once

/*-------------
	DBError
--------------*/

// MySQL 오류 번호를 "다시 하면 되나" 로 나눈다 (ADR-0010 R-1 · 2026-10-08).
// 순수 함수 - MySQL 헤더 없이 번호만 본다. 저장 재시도(GameServer CommitRetry)와 DB 워커의 재연결이 같이 쓴다.
//
//   ConnectionLost  연결이 죽었다 - 이 커넥션으로는 무엇을 해도 실패한다. 다시 붙인 뒤 재시도
//   Transient       연결은 산 채로 이번만 실패했다 (교착 · 잠금 대기 초과 · 너무 많은 연결) - 그대로 재시도
//   Permanent       같은 문장을 다시 보내도 같은 결과다 (중복 키 · 외래 키 · 제약 위반 · 문법 · 권한 · 0)

namespace DBError
{
	enum class Kind : uint8
	{
		ConnectionLost,
		Transient,
		Permanent,
	};

	inline Kind Classify(uint32 mysqlErrno)
	{
		// 2000 번대는 클라이언트 라이브러리 오류 - 거의 전부 연결 문제다 (2002 · 2003 · 2006 · 2013 · 2055 …)
		if (mysqlErrno >= 2000 && mysqlErrno < 3000)
			return Kind::ConnectionLost;

		switch (mysqlErrno)
		{
		case 1053:		// ER_SERVER_SHUTDOWN
		case 1152:		// ER_ABORTING_CONNECTION
		case 1158:		// ER_NET_READ_ERROR
		case 1159:		// ER_NET_READ_INTERRUPTED
		case 1160:		// ER_NET_ERROR_ON_WRITE
		case 1161:		// ER_NET_WRITE_INTERRUPTED
		case 1927:		// ER_CONNECTION_KILLED
		case 4031:		// ER_CLIENT_INTERACTION_TIMEOUT
			return Kind::ConnectionLost;

		case 1040:		// ER_CON_COUNT_ERROR - 연결이 너무 많다
		case 1203:		// ER_TOO_MANY_USER_CONNECTIONS
		case 1205:		// ER_LOCK_WAIT_TIMEOUT
		case 1213:		// ER_LOCK_DEADLOCK
		case 1317:		// ER_QUERY_INTERRUPTED
		case 3572:		// ER_LOCK_NOWAIT
			return Kind::Transient;

		default:
			return Kind::Permanent;
		}
	}

	inline bool IsConnectionLost(uint32 mysqlErrno) { return Classify(mysqlErrno) == Kind::ConnectionLost; }
}
