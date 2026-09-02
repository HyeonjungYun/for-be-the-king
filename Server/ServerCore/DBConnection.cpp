#include "pch.h"
#include "DBConnection.h"

bool DBConnection::Connect(const char* host, uint32 port, const char* user, const char* password, const char* schema)
{
	_conn = ::mysql_init(nullptr);
	if (_conn == nullptr)
		return false;

	::mysql_options(_conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");

	if (::mysql_real_connect(_conn, host, user, password, schema, port, nullptr, 0) == nullptr)
	{
		::mysql_close(_conn);
		_conn = nullptr;
		return false;
	}

	return true;
}

void DBConnection::Close()
{
	if (_conn == nullptr)
		return;

	::mysql_close(_conn);
	_conn = nullptr;
}

bool DBConnection::Excute(const char* query)
{
	if (_conn == nullptr)
		return false;

	return ::mysql_query(_conn, query) == 0;
}

MYSQL_RES* DBConnection::Query(const char* query)
{
	if (_conn == nullptr)
		return nullptr;

	if (::mysql_query(_conn, query) != 0)
		return nullptr;

	return ::mysql_store_result(_conn);
}

void DBConnection::FreeResult(MYSQL_RES* result)
{
	if (result != nullptr)
		::mysql_free_result(result);
}

uint64 DBConnection::GetLastInsertId() const
{
	return (_conn != nullptr) ? ::mysql_insert_id(_conn) : 0;
}

uint64 DBConnection::GetAffectedRows() const
{
	return (_conn != nullptr) ? ::mysql_affected_rows(_conn) : 0;
}

string DBConnection::Escape(const string& value) const
{
	if (_conn == nullptr)
		return nullptr;

	string escaped;
	escaped.resize(value.size() * 2 + 1);

	const unsigned long length = ::mysql_real_escape_string(_conn, escaped.data(), value.c_str(), static_cast<unsigned long>(value.size()));

	escaped.resize(length);
	return escaped;
}

bool DBConnection::BeginTransaction()
{
	return Excute("START TRANSACTION");
}

bool DBConnection::Commit()
{
	return Excute("COMMIT");
}

bool DBConnection::Rollback()
{
	return Excute("ROLLBACK");
}

const char* DBConnection::GetError() const
{
	return (_conn != nullptr) ? ::mysql_error(_conn) : "not connected";
}

uint32 DBConnection::GetLastErrorNo() const
{
	return (_conn != nullptr) ? ::mysql_errno(_conn) : 0;
}
