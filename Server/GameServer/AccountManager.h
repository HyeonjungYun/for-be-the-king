#pragma once

class GameSesion;

/*---------------------
	AccountManager
----------------------*/

class AccountManager
{
public:
	bool TryLogin(uint64 accountId);
	void LogOut(uint64 accountId);

	int32 GetCount();

private:
	USE_LOCK;
	unordered_set<uint64> _accounts;
};

extern AccountManager GAccountManager;

