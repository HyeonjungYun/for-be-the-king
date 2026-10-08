#include "pch.h"
#include "DBConnection.h"

namespace
{
	// 죽은 호스트에 붙으려다 DB 워커가 오래 서지 않게 (기본값은 OS TCP 시간 - 수십 초)
	constexpr unsigned int CONNECT_TIMEOUT_S = 3;

	// 멈춘 서버에 보낸 쿼리가 워커를 영원히 세우지 않게. 끝나면 2013(연결 끊김)으로 돌아와 재연결된다.
	// 이 서버의 쿼리는 전부 짧다 - 부팅 검사도 1 초 안쪽 (ADR-0010 R-1)
	constexpr unsigned int READ_WRITE_TIMEOUT_S = 30;
}

bool DBConnection::Connect(const char* host, uint32 port, const char* user, const char* password, const char* schema)
{
	_host = host != nullptr ? host : "";
	_port = port;
	_user = user != nullptr ? user : "";
	_password = password != nullptr ? password : "";
	_schema = schema != nullptr ? schema : "";

	return Open();
}

bool DBConnection::Open()
{
	_conn = ::mysql_init(nullptr);
	if (_conn == nullptr)
		return false;

	::mysql_options(_conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");
	::mysql_options(_conn, MYSQL_OPT_CONNECT_TIMEOUT, &CONNECT_TIMEOUT_S);
	::mysql_options(_conn, MYSQL_OPT_READ_TIMEOUT, &READ_WRITE_TIMEOUT_S);
	::mysql_options(_conn, MYSQL_OPT_WRITE_TIMEOUT, &READ_WRITE_TIMEOUT_S);

	if (::mysql_real_connect(_conn, _host.c_str(), _user.c_str(), _password.c_str(), _schema.c_str(), _port, nullptr, 0) == nullptr)
	{
		_lastConnectErrno = ::mysql_errno(_conn);
		::mysql_close(_conn);
		_conn = nullptr;
		return false;
	}

	_lastConnectErrno = 0;
	return true;
}

bool DBConnection::Reconnect()
{
	if (_host.empty())
		return false;

	Close();
	_reconnects++;

	const bool ok = Open();
	LOG_LINE << "[DB] reconnect #" << _reconnects << (ok ? " ok" : " failed") << " err=" << _lastConnectErrno << '\n';
	return ok;
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
	// 연결이 없으면 마지막 연결 시도의 오류 - 재연결이 실패한 커넥션의 작업이 "오류 0" 으로 보이면 영구 실패로 오판된다
	return (_conn != nullptr) ? ::mysql_errno(_conn) : _lastConnectErrno;
}
