#pragma once
#include <mysql/mysql.h>
#include "pch.h"

/*----------------
	DBConnection
-----------------*/

class DBConnection
{
public:
	bool			Connect(const char* host, uint32 port, const char* user, const char* password, const char* schema);
	void			Close();

	// INSERT, UPDATE, DELETE, DDL 결과셋이 없는 쿼리
	bool			Excute(const char* query);

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

	const char*		GetError() const;
	bool			IsConnected() const { return _conn != nullptr; }

private:
	MYSQL* _conn = nullptr;
};
