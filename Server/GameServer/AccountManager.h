#pragma once

/*---------------------
	AccountManager
----------------------*/

class AccountManager
{
public:
	bool TryLogin(uint64 accountId);
	void LogOut(uint64 accountId);


private:
	USE_LOCK;
	unordered_set<uint64> _accounts;
};

extern AccountManager GAccountManager;

