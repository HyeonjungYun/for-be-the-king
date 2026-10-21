#pragma once
#include <mysql/mysql.h>
#include "pch.h"

/*----------------
	DBConnection
-----------------*/

class DBConnection
{
public:
	// MYSQL* 를 가진다 - 복사하면 두 번 닫는다. 닫지 않고 버리면 새는 것을 막는다 (Close 는 여러 번 불러도 된다)
	DBConnection() = default;
	~DBConnection() { Close(); }
	DBConnection(const DBConnection&) = delete;
	DBConnection& operator=(const DBConnection&) = delete;

	bool			Connect(const char* host, uint32 port, const char* user, const char* password, const char* schema);
	void			Close();

	// 같은 접속 정보로 다시 붙는다 - 끊긴 연결은 저절로 살아나지 않는다 (예전엔 MySQL 이 재시작하면 그 커넥션의 모든 작업이
	// 서버가 꺼질 때까지 실패했다, ADR-0010 R-1). 한 번도 Connect 하지 않았으면 false. 실패하면 GetLastErrorNo 가 연결 오류를 준다
	bool			Reconnect();
	uint32			GetReconnectCount() const { return _reconnects; }

	// INSERT, UPDATE, DELETE, DDL 결과셋이 없는 쿼리
	bool			Execute(const char* query);

	//SELECT. 반환된 결과는 반드시 FreeResult 로 해제한다.
	//nullptr 이면 실패이며 GetError 로 원인을 볼 수 있다.
	MYSQL_RES*		Query(const char* query);
	void			FreeResult(MYSQL_RES* result);

	// 마지막 INSERT의 AUTO_INCREMENT 값
	uint64			GetLastInsertId() const;
	uint64			GetAffectedRows() const;

	//문자열을 SQL 에 안전하게 넣기 위한 이스케이프.
	// 🔴 사용자 입력을 쿼리에 붙일 때 반드시 거칠 것 — 계정명이 그 경로다.
	string			Escape(const string& value) const;

	bool			BeginTransaction();
	bool			Commit();
	bool			Rollback();

	const char*		GetError() const;
	uint32			GetLastErrorNo() const;

	bool			IsConnected() const { return _conn != nullptr; }

	// prepared statement 용 (DBStatement) - 다른 데서 쓰지 말 것
	MYSQL*			Raw() const { return _conn; }

private:
	bool			Open();

	MYSQL*			_conn = nullptr;

	// Reconnect 용 접속 정보 - Connect 가 채운다
	string			_host;
	uint32			_port = 0;
	string			_user;
	string			_password;
	string			_schema;

	uint32			_lastConnectErrno = 0;		// 연결이 없을 때 GetLastErrorNo 가 줄 값 - 마지막 연결 시도의 오류
	uint32			_reconnects = 0;
};
