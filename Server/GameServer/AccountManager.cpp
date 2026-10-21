#include "pch.h"
#include "AccountManager.h"
#include "GameSession.h"

AccountManager GAccountManager;

bool AccountManager::TryLogin(uint64 accountId, const shared_ptr<GameSession>& owner)
{
	if (accountId == 0 || owner == nullptr)
		return false;

	WRITE_LOCK;

	auto it = _accounts.find(accountId);
	if (it != _accounts.end())
	{
		shared_ptr<GameSession> current = it->second.lock();
		if (current != nullptr && current != owner)
			return false;		// 다른 살아 있는 연결이 쓰고 있다
	}

	_accounts[accountId] = owner;
	return true;
}

void AccountManager::LogOut(uint64 accountId, const GameSession* owner)
{
	if (accountId == 0)
		return;

	WRITE_LOCK;

	auto it = _accounts.find(accountId);
	if (it == _accounts.end())
		return;

	if (owner != nullptr)
	{
		shared_ptr<GameSession> current = it->second.lock();
		if (current != nullptr && current.get() != owner)
			return;			// 이미 다른 연결의 것이다 - 옛 연결의 늦은 LogOut
	}

	_accounts.erase(it);
}

shared_ptr<GameSession> AccountManager::FindOwner(uint64 accountId)
{
	WRITE_LOCK;

	auto it = _accounts.find(accountId);
	return (it != _accounts.end()) ? it->second.lock() : nullptr;
}

int32 AccountManager::Count()
{
	WRITE_LOCK;
	return static_cast<int32>(_accounts.size());
}
