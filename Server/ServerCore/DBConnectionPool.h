#pragma once
#include "DBConnection.h"

/*--------------------
	DBConnectionPool
---------------------*/

class DBConnectionPool
{
public:
					~DBConnectionPool();

	bool			Connect(int32 connectionCount, const char* host, uint32 port, const char* user, const char* password, const char* schema);
	void			Clear();

	DBConnection*	Pop();
	void			Push(DBConnection* connection);

	int32			GetIdleCount();

private:
	USE_LOCK;

	vector<DBConnection*> _connections;
	vector<DBConnection*> _idle;
};

