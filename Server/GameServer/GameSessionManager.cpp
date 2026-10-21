#include "pch.h"
#include "GameSessionManager.h"
#include "GameSession.h"

GameSessionManager GSessionManager;

namespace
{
	int32 MaxConnPerIp()
	{
		static const int32 limit = []()
		{
			char* raw = nullptr;
			size_t length = 0;
			int32 value = GameSessionManager::MAX_CONN_PER_IP_DEFAULT;
			if (_dupenv_s(&raw, &length, "FBTK_MAX_CONN_PER_IP") == 0 && raw != nullptr)
			{
				value = ::atoi(raw);
				::free(raw);
			}
			return value;
		}();
		return limit;
	}

	bool IsLoopback(const string& ip)
	{
		return ip.rfind("127.", 0) == 0 || ip == "::1";
	}
}

bool GameSessionManager::Add(GameSessionRef session, const string& ip)
{
	WRITE_LOCK_ORDERED(LockLevel::REGISTRY);

	const int32 limit = MaxConnPerIp();
	if (limit > 0 && ip.empty() == false && IsLoopback(ip) == false)
	{
		int32& count = _perIp[ip];
		if (count >= limit)
			return false;
		count++;
		_ipOf[session.get()] = ip;
	}

	_sessions.insert(session);
	return true;
}

void GameSessionManager::Remove(GameSessionRef session)
{
	WRITE_LOCK_ORDERED(LockLevel::REGISTRY);
	_sessions.erase(session);

	auto it = _ipOf.find(session.get());
	if (it != _ipOf.end())
	{
		auto countIt = _perIp.find(it->second);
		if (countIt != _perIp.end() && --countIt->second <= 0)
			_perIp.erase(countIt);
		_ipOf.erase(it);
	}
}

int32 GameSessionManager::ConnectionsFrom(const string& ip)
{
	WRITE_LOCK_ORDERED(LockLevel::REGISTRY);
	auto it = _perIp.find(ip);
	return (it != _perIp.end()) ? it->second : 0;
}

int32 GameSessionManager::DropUnauthenticated(uint64 nowUs, uint64 timeoutUs)
{
	vector<GameSessionRef> idle;
	{
		WRITE_LOCK_ORDERED(LockLevel::REGISTRY);
		for (const GameSessionRef& session : _sessions)
		{
			const uint64 since = session->connectedAtUs.load();
			// 로그인 처리 중(loginStarted)이면 DB 가 느린 것이다 - 건드리지 않는다
			if (session->loginStarted.load() == false && session->accountId.load() == 0 && since != 0 && nowUs > since + timeoutUs)
				idle.push_back(session);
		}
	}

	// 락 밖에서 끊는다 - Disconnect 의 정리(OnDisconnected → Remove)가 이 락을 다시 잡는다
	for (const GameSessionRef& session : idle)
		session->Disconnect(L"login timeout");
	return static_cast<int32>(idle.size());
}

vector<GameSessionRef> GameSessionManager::Snapshot()
{
	WRITE_LOCK_ORDERED(LockLevel::REGISTRY);
	return vector<GameSessionRef>(_sessions.begin(), _sessions.end());
}

int32 GameSessionManager::DisconnectAll(const WCHAR* cause, const function<void(const GameSessionRef&)>& before)
{
	vector<GameSessionRef> all;
	{
		WRITE_LOCK_ORDERED(LockLevel::REGISTRY);
		all.assign(_sessions.begin(), _sessions.end());
	}

	// 락 밖에서 끊는다 (DropUnauthenticated 와 같은 이유)
	for (const GameSessionRef& session : all)
	{
		if (before != nullptr)
			before(session);
		session->Disconnect(cause);
	}
	return static_cast<int32>(all.size());
}

int32 GameSessionManager::DropIdle(uint64 nowUs, uint64 timeoutUs)
{
	if (timeoutUs == 0)
		return 0;

	vector<GameSessionRef> idle;
	{
		WRITE_LOCK_ORDERED(LockLevel::REGISTRY);
		for (const GameSessionRef& session : _sessions)
		{
			const uint64 last = session->lastRecvUs.load();
			if (session->accountId.load() != 0 && last != 0 && nowUs > last + timeoutUs)
				idle.push_back(session);
		}
	}

	for (const GameSessionRef& session : idle)
	{
		LOG_LINE << "[RATE] idle timeout - account=" << session->accountId.load() << " silent for " << (nowUs - session->lastRecvUs.load()) / 1000 << "ms\n";
		session->Disconnect(L"idle timeout");
	}
	return static_cast<int32>(idle.size());
}

GameSessionRef GameSessionManager::FindByCharacter(uint64 characterId)
{
	WRITE_LOCK_ORDERED(LockLevel::REGISTRY);
	for (const GameSessionRef& session : _sessions)
	{
		// accountId 가 0 이 아니면 로그인 작업이 characters 를 다 채운 뒤다 (accountId 를 마지막에 쓴다)
		if (session->accountId.load() == 0)
			continue;

		for (const Protocol::CharacterInfo& character : session->characters)
		{
			if (character.object_info().object_id() == characterId)
				return session;
		}
	}
	return nullptr;
}

GameSessionRef GameSessionManager::FindByName(const string& name)
{
	if (name.empty())
		return nullptr;

	WRITE_LOCK_ORDERED(LockLevel::REGISTRY);
	for (const GameSessionRef& session : _sessions)
	{
		if (session->accountId.load() == 0)
			continue;

		for (const Protocol::CharacterInfo& character : session->characters)
		{
			if (character.name() == name)
				return session;
		}
	}
	return nullptr;
}

GameSessionRef GameSessionManager::FindInBaseCamp(uint64 characterId)
{
	WRITE_LOCK_ORDERED(LockLevel::REGISTRY);
	for (const GameSessionRef& session : _sessions)
	{
		// place 를 먼저 본다 - 로그인 작업이 characters 를 채운 뒤에 place 를 BaseCamp 로 바꾸므로(atomic),
		// BaseCamp 로 보이면 characters 는 이미 다 쓰였다.
		if (session->place.load() != SessionPlace::BaseCamp)
			continue;

		for (const Protocol::CharacterInfo& character : session->characters)
		{
			if (character.object_info().object_id() == characterId)
				return session;
		}
	}

	return nullptr;
}