#pragma once

class GameSession;

/*---------------------
	AccountManager
----------------------*/

// 계정 하나에 연결 하나. 계정이 어느 연결의 것인지도 든다 (2026-10-21) - 예전엔 id 집합뿐이라 두 가지가 안 됐다:
//   · 중복 로그인에서 옛 연결을 찾아 끊기 (kick-old). 선이 뽑힌 옛 연결을 서버가 알아차릴 때까지(최대 keepalive 약 40 s) 그 사람이 못 들어왔다
//   · 옛 연결의 늦은 LogOut 이 새 연결의 계정을 풀어 버리는 것 막기 (주인이 맞을 때만 지운다)
class AccountManager
{
public:
	// 비어 있거나 주인이 이미 사라진 연결이면 owner 를 주인으로 적고 true. 다른 살아 있는 연결이 쓰고 있으면 false
	bool						TryLogin(uint64 accountId, const shared_ptr<GameSession>& owner);

	// owner 가 주인일 때만 지운다 (nullptr 이면 무조건)
	void						LogOut(uint64 accountId, const GameSession* owner);

	// 지금 이 계정을 쓰는 연결. 없거나 사라졌으면 nullptr
	shared_ptr<GameSession>		FindOwner(uint64 accountId);

	int32						Count();

private:
	USE_LOCK;
	unordered_map<uint64, weak_ptr<GameSession>> _accounts;
};

extern AccountManager GAccountManager;
