#include "pch.h"
#include "DBStatement.h"

/*----------------
	DBStatement
-----------------*/

namespace
{
	constexpr size_t RESULT_BUFFER_BYTES = 256;		// 열마다 처음 잡는 크기 - 넘치면 그 열만 다시 받는다 (Fetch)
}

DBStatement::DBStatement(DBConnection& conn, const char* sql)
{
	MYSQL* raw = conn.Raw();
	if (raw == nullptr)
	{
		_prepareErrno = 2006;		// CR_SERVER_GONE_ERROR 와 같은 번호 - DBError 가 연결 끊김으로 분류한다
		_prepareError = "not connected";
		return;
	}

	_stmt = ::mysql_stmt_init(raw);
	if (_stmt == nullptr)
	{
		_prepareErrno = ::mysql_errno(raw);
		_prepareError = ::mysql_error(raw);
		return;
	}

	if (::mysql_stmt_prepare(_stmt, sql, static_cast<unsigned long>(::strlen(sql))) != 0)
	{
		_prepareErrno = ::mysql_stmt_errno(_stmt);
		_prepareError = ::mysql_stmt_error(_stmt);
		::mysql_stmt_close(_stmt);
		_stmt = nullptr;
	}
}

DBStatement::~DBStatement()
{
	FreeResults();

	if (_stmt != nullptr)
		::mysql_stmt_close(_stmt);
}

DBStatement& DBStatement::BindString(const string& value)
{
	_strings.push_back(value);
	_lengths.push_back(static_cast<unsigned long>(value.size()));

	MYSQL_BIND bind = {};
	bind.buffer_type = MYSQL_TYPE_STRING;
	bind.buffer = const_cast<char*>(_strings.back().data());
	bind.buffer_length = static_cast<unsigned long>(value.size());
	bind.length = &_lengths.back();
	_params.push_back(bind);
	return *this;
}

DBStatement& DBStatement::BindUInt64(uint64 value)
{
	_uints.push_back(value);

	MYSQL_BIND bind = {};
	bind.buffer_type = MYSQL_TYPE_LONGLONG;
	bind.buffer = &_uints.back();
	bind.is_unsigned = true;
	_params.push_back(bind);
	return *this;
}

DBStatement& DBStatement::BindInt64(int64 value)
{
	_ints.push_back(value);

	MYSQL_BIND bind = {};
	bind.buffer_type = MYSQL_TYPE_LONGLONG;
	bind.buffer = &_ints.back();
	bind.is_unsigned = false;
	_params.push_back(bind);
	return *this;
}

bool DBStatement::BindParams()
{
	if (_paramsBound || _params.empty())
		return true;

	if (::mysql_stmt_param_count(_stmt) != _params.size())
		return false;

	if (::mysql_stmt_bind_param(_stmt, _params.data()) != 0)
		return false;

	_paramsBound = true;
	return true;
}

bool DBStatement::Execute()
{
	if (_stmt == nullptr)
		return false;

	if (BindParams() == false)
		return false;

	return ::mysql_stmt_execute(_stmt) == 0;
}

bool DBStatement::Query()
{
	if (Execute() == false)
		return false;

	FreeResults();

	_meta = ::mysql_stmt_result_metadata(_stmt);
	if (_meta == nullptr)
		return true;		// 결과셋이 없는 문장 - Fetch 는 바로 false

	if (BindResults() == false)
		return false;

	// 결과를 클라 쪽에 다 받아 둔다 - 커넥션이 다음 문장을 바로 받을 수 있게
	if (::mysql_stmt_store_result(_stmt) != 0)
		return false;

	_stored = true;
	return true;
}

bool DBStatement::BindResults()
{
	const uint32 fieldCount = ::mysql_num_fields(_meta);
	if (fieldCount == 0)
		return true;

	_results.assign(fieldCount, MYSQL_BIND{});
	_buffers.assign(fieldCount, vector<char>(RESULT_BUFFER_BYTES));
	_resultLengths.assign(fieldCount, 0);
	_nulls = make_unique<bool[]>(fieldCount);
	_errors = make_unique<bool[]>(fieldCount);

	for (uint32 i = 0; i < fieldCount; i++)
	{
		_nulls[i] = false;
		_errors[i] = false;

		MYSQL_BIND& bind = _results[i];
		bind.buffer_type = MYSQL_TYPE_STRING;		// 숫자 열도 문자열로 받는다 (호출부가 strtoull 로 읽는다)
		bind.buffer = _buffers[i].data();
		bind.buffer_length = static_cast<unsigned long>(_buffers[i].size());
		bind.length = &_resultLengths[i];
		bind.is_null = &_nulls[i];
		bind.error = &_errors[i];
	}

	return ::mysql_stmt_bind_result(_stmt, _results.data()) == 0;
}

bool DBStatement::Fetch(OUT vector<string>& row)
{
	row.clear();

	if (_stmt == nullptr || _meta == nullptr || _stored == false)
		return false;

	const int32 rc = ::mysql_stmt_fetch(_stmt);
	if (rc == MYSQL_NO_DATA || rc == 1)
		return false;

	const size_t fieldCount = _results.size();
	row.resize(fieldCount);

	for (size_t i = 0; i < fieldCount; i++)
	{
		if (_nulls[i])
			continue;

		// 처음 잡은 크기를 넘는 열(긴 문자열)은 그 열만 넉넉한 버퍼로 다시 받는다 (MYSQL_DATA_TRUNCATED)
		if (_resultLengths[i] > _buffers[i].size())
		{
			_buffers[i].resize(_resultLengths[i]);
			MYSQL_BIND& bind = _results[i];
			bind.buffer = _buffers[i].data();
			bind.buffer_length = static_cast<unsigned long>(_buffers[i].size());
			if (::mysql_stmt_fetch_column(_stmt, &bind, static_cast<unsigned int>(i), 0) != 0)
				return false;
		}

		row[i].assign(_buffers[i].data(), (std::min<size_t>)(_resultLengths[i], _buffers[i].size()));
	}

	return true;
}

void DBStatement::FreeResults()
{
	if (_stmt != nullptr && _stored)
		::mysql_stmt_free_result(_stmt);
	_stored = false;

	if (_meta != nullptr)
	{
		::mysql_free_result(_meta);
		_meta = nullptr;
	}
}

uint64 DBStatement::AffectedRows() const
{
	return (_stmt != nullptr) ? ::mysql_stmt_affected_rows(_stmt) : 0;
}

uint32 DBStatement::ErrorNo() const
{
	return (_stmt != nullptr) ? ::mysql_stmt_errno(_stmt) : _prepareErrno;
}

const char* DBStatement::Error() const
{
	return (_stmt != nullptr) ? ::mysql_stmt_error(_stmt) : _prepareError.c_str();
}
