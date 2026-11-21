#pragma once

// 방 배정에 묻기 (2026-11-22 동접 1만 2 단계) - 인증 서버의 GET /lobby/assign?count=N 이 가장 여유 있는 게임 서버를 준다.
// 봇은 인증을 거치지 않으므로 이것만 묻는다 (DummyClient -assign http://127.0.0.1:8080). WinHTTP 로 GET 한 번
namespace LobbyClient
{
	struct Assignment
	{
		string	host;
		uint16	port = 0;
		uint32	serverId = 0;
	};

	// 실패하면 false 와 이유
	bool	Assign(const string& baseUrl, int32 count, OUT Assignment& out, OUT string& error);
}
