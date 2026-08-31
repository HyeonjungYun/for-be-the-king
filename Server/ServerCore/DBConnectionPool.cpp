#include "pch.h"
#include "DBConnectionPool.h"

DBConnectionPool::~DBConnectionPool()
{
	Clear();
}

bool DBConnectionPool::Connect(int32 connectionCount, const char* host, uint32 port, const char* user, const char* password, const char* schema)
{
	WRITE_LOCK;

	for (int32 i = 0; i < connectionCount; i++)
	{
		DBConnection* connection = new DBConnection();

		if (connection->Connect(host, port, user, password, schema) == false)
		{
			// 하나라도 실패하면 전부 접는다. 절반만 연결된 풀은 나중에
			cout << "[DB] connect failed: " << connection->GetError() << endl;
			delete connection;

			for (DBConnection* c : _connections)
			{
				c->Close();
				delete c;
			}
		}

		_connections.push_back(connection);
		_idle.push_back(connection);
	}

	return true;
}

void DBConnectionPool::Clear()
{
	WRITE_LOCK;

	for (DBConnection* connection : _connections)
	{
		connection->Close();
		delete connection;
	}

	_connections.clear();
	_idle.clear();
}

DBConnection* DBConnectionPool::Pop()
{
	WRITE_LOCK;

	if (_idle.empty())
		return nullptr;

	DBConnection* connection = _idle.back();
	_idle.pop_back();
	return connection;
}

void DBConnectionPool::Push(DBConnection* connection)
{
	if (connection == nullptr)
		return;

	WRITE_LOCK;
	_idle.push_back(connection);
}

int32 DBConnectionPool::GetIdleCount()
{
	WRITE_LOCK;
	return static_cast<int32>(_idle.size());
}
