#pragma once
#include "DBConnection.h"
#include <deque>

/*----------------
	DBStatement
-----------------*/

// MySQL prepared statement (2026-10-21 리뷰 후속). 클라가 준 문자열이 SQL 문장에 닿는 자리 - 로그인 토큰 해시 · 지급 request_id / reason ·
// 저널 id · 치트 요약 - 를 문자열 연결 + Escape 대신 바인딩으로 보낸다. 숫자만 들어가는 문장은 그대로 snprintf 다 (인젝션 표면이 아니다).
// 문장마다 prepare 하고 버린다 - 이 서버의 DB 호출은 초당 수십 건이라 문장 캐시가 필요 없다.
//
//   DBStatement stmt(*conn, "SELECT account_id FROM login_sessions WHERE token = ? AND expires_at > NOW()");
//   vector<string> row;
//   if (stmt.BindString(hash).Query() && stmt.Fetch(OUT row)) { ... row[0] ... }
//
// 결과 열은 전부 문자열로 받는다 (숫자 열도 "123") - 호출부가 strtoull 로 읽던 MYSQL_ROW 와 같은 모양. NULL 은 빈 문자열.
class DBStatement
{
public:
	DBStatement(DBConnection& conn, const char* sql);
	~DBStatement();

	DBStatement(const DBStatement&) = delete;
	DBStatement& operator=(const DBStatement&) = delete;

	bool			IsValid() const { return _stmt != nullptr; }

	// ? 자리에 순서대로. 값은 복사해 든다 (실행 때까지 살아 있어야 한다)
	DBStatement&	BindString(const string& value);
	DBStatement&	BindUInt64(uint64 value);
	DBStatement&	BindInt64(int64 value);

	bool			Execute();						// INSERT · UPDATE · DELETE
	bool			Query();						// SELECT - 결과를 클라 쪽에 다 받아 둔다. 이어서 Fetch
	bool			Fetch(OUT vector<string>& row);	// 다음 행. 끝이면 false

	uint64			AffectedRows() const;
	uint32			ErrorNo() const;
	const char*		Error() const;

private:
	bool			BindParams();
	bool			BindResults();
	void			FreeResults();

	MYSQL_STMT*					_stmt = nullptr;
	uint32						_prepareErrno = 0;
	string						_prepareError;

	// 매개변수 - 주소가 안 바뀌게 deque
	deque<string>				_strings;
	deque<uint64>				_uints;
	deque<int64>				_ints;
	deque<unsigned long>		_lengths;
	vector<MYSQL_BIND>			_params;
	bool						_paramsBound = false;

	// 결과
	MYSQL_RES*					_meta = nullptr;
	vector<MYSQL_BIND>			_results;
	vector<vector<char>>		_buffers;
	vector<unsigned long>		_resultLengths;
	vector<bool>				_nullFlags;		// MYSQL_BIND::is_null 은 bool* (MySQL 8) - vector<bool> 은 주소를 못 주므로 아래 배열을 쓴다
	unique_ptr<bool[]>			_nulls;
	unique_ptr<bool[]>			_errors;
	bool						_stored = false;
};
