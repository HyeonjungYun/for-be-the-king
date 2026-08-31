#include "pch.h"
#include "AccountManager.h"

AccountManager GAccountManager;

bool AccountManager::TryLogin(uint64 accountId)
{
	if (accountId == 0)
		return false;

	WRITE_LOCK;

	return _accounts.insert(accountId).second;
}

void AccountManager::LogOut(uint64 accountId)
{
	if (accountId == 0)
		return;

	WRITE_LOCK;
	_accounts.erase(accountId);
}

int32 AccountManager::GetCount()
{
	WRITE_LOCK;
	return static_cast<int32>(_accounts.size());
}
