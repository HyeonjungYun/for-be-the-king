#include "pch.h"
#include "AdminServer.h"
#include "EventLog.h"
#include "Room.h"
#include "Player.h"
#include "GameSession.h"
#include "GameSessionManager.h"
#include "InstanceManager.h"
#include "ServerPacketHandler.h"
#include "ServerStats.h"
#include "DBJobQueue.h"
#include "DBConnection.h"
#include "DBStatement.h"
#include <bcrypt.h>
#include <deque>
#include <fstream>
#include <future>

#pragma comment(lib, "bcrypt.lib")

AdminServer GAdmin;

/*-------- AdminServer --------*/

struct AdminServer::Client
{
	SOCKET			socket = INVALID_SOCKET;
	string			in;
	std::mutex		outLock;
	std::deque<string> out;		// outLock 아래
	size_t			outBytes = 0;
	string			sending;	// 관리 스레드 전용 - 반쯤 보낸 줄
	size_t			sendingAt = 0;
	bool			authed = false;
	atomic<bool>	events = false;
	atomic<bool>	metrics = false;
	string			mapRoom;	// 관리 스레드 전용
	uint64			nextMapUs = 0;
	uint64			mapCount = 0;
	std::future<string> mapFuture;
	bool			mapPending = false;
	atomic<uint64>	dropped = 0;
	atomic<bool>	closed = false;
};

namespace
{
	using EventFormat::JsonEscape;

	string RandomHex(size_t bytes)
	{
		vector<uint8> buf(bytes);
		::BCryptGenRandom(nullptr, buf.data(), static_cast<ULONG>(buf.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
		static const char* hex = "0123456789abcdef";
		string s;
		for (uint8 b : buf) { s += hex[b >> 4]; s += hex[b & 15]; }
		return s;
	}

	string NowText()
	{
		SYSTEMTIME st;
		::GetLocalTime(&st);
		char buf[32];
		::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
		return buf;
	}

	vector<RoomRef> AllRooms()
	{
		vector<RoomRef> rooms;
		for (int32 i = 0; i < FLOOR_COUNT; i++)
			if (GRooms[i] != nullptr)
				rooms.push_back(GRooms[i]);
		GInstances.ForEachRoom([&rooms](const RoomRef& room) { rooms.push_back(room); });
		return rooms;
	}

	// 방 잡에서 문자열 하나를 만들어 받는다 - 방이 닫히는 중이면 시간 안에 안 올 수 있다 (빈 칸)
	std::future<string> AskRoom(const RoomRef& room, function<string(Room&)> fn)
	{
		auto promise = make_shared<std::promise<string>>();
		std::future<string> future = promise->get_future();
		room->DoAsync([room, promise, fn = move(fn)]() { promise->set_value(fn(*room)); });
		return future;
	}

	string Wait(std::future<string>& f, uint32 ms = 1000)
	{
		if (f.valid() && f.wait_for(std::chrono::milliseconds(ms)) == std::future_status::ready)
			return f.get();
		return string();
	}

	string Result(const char* op, bool ok, const string& msg)
	{
		return string("{\"t\":\"result\",\"op\":\"") + op + "\",\"ok\":" + (ok ? "true" : "false") + ",\"msg\":\"" + JsonEscape(msg) + "\"}";
	}

	string Inner(const string& array)		// "[a,b]" -> "a,b"
	{
		if (array.size() < 2)
			return string();
		return array.substr(1, array.size() - 2);
	}
}

bool AdminServer::Start(const AdminConfig& config, OUT string& error)
{
	_config = config;
	_token = RandomHex(32);
	_boot = NowText();

	SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET)
	{
		error = "socket failed " + to_string(::WSAGetLastError());
		return false;
	}
	BOOL exclusive = TRUE;		// 다른 프로세스가 같은 포트를 가로채지 못하게
	::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
	SOCKADDR_IN addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = ::htons(config.port);
	addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);		// 이 PC 에서만
	if (::bind(s, reinterpret_cast<SOCKADDR*>(&addr), sizeof(addr)) == SOCKET_ERROR || ::listen(s, 8) == SOCKET_ERROR)
	{
		error = "bind 127.0.0.1:" + to_string(config.port) + " failed " + to_string(::WSAGetLastError());
		::closesocket(s);
		return false;
	}
	u_long nonBlocking = 1;
	::ioctlsocket(s, FIONBIO, &nonBlocking);
	_listen = static_cast<uintptr_t>(s);

	// 정보 파일 - 앱이 읽는다. 토큰이 들어 있으니 이 PC 사용자만 볼 수 있는 Logs 아래에 둔다
	{
		std::ofstream f(config.infoFile, std::ios::binary | std::ios::trunc);
		if (f.is_open() == false)
		{
			error = "cannot write " + config.infoFile;
			::closesocket(s);
			return false;
		}
		f << "{\"port\":" << config.port << ",\"token\":\"" << _token << "\",\"pid\":" << ::GetCurrentProcessId()
			<< ",\"game_port\":" << config.gamePort << ",\"boot\":\"" << _boot << "\",\"dataDir\":\"" << JsonEscape(config.dataDir) << "\""
			<< ",\"eventsFile\":\"" << JsonEscape(GEvents.FilePath()) << "\"}\n";
	}

	GEvents.SetSubscriber([this](const string& line) { PublishEvent(line); });
	_running.store(true);
	_thread = std::thread([this]() { Run(); });
	return true;
}

void AdminServer::Stop()
{
	if (_running.exchange(false) == false)
		return;
	_stop.store(true);
	GEvents.SetSubscriber(nullptr);
	if (_thread.joinable())
		_thread.join();
	::DeleteFileA(_config.infoFile.c_str());		// 꺼진 서버의 토큰을 남기지 않는다
}

void AdminServer::Send(const ClientRef& client, string line)
{
	line += '\n';
	std::lock_guard<std::mutex> guard(client->outLock);
	if (client->outBytes + line.size() > MAX_QUEUED_BYTES)
	{
		client->dropped.fetch_add(1);
		return;
	}
	client->outBytes += line.size();
	client->out.push_back(move(line));
}

void AdminServer::PublishEvent(const string& line)
{
	std::lock_guard<std::mutex> guard(_clientsLock);
	for (const ClientRef& c : _clients)
		if (c->authed && c->events.load())
			Send(c, line);
}

void AdminServer::Broadcast(const string& line, bool metricsOnly)
{
	std::lock_guard<std::mutex> guard(_clientsLock);
	for (const ClientRef& c : _clients)
		if (c->authed && (metricsOnly == false || c->metrics.load()))
			Send(c, line);
}

void AdminServer::Run()
{
	LThreadId = 90;		// 로그 머리의 스레드 번호 - 관리 스레드
	char buf[16 * 1024];
	uint64 lastMetricsCheckUs = 0;

	while (_stop.load() == false)
	{
		vector<ClientRef> clients;
		{
			std::lock_guard<std::mutex> guard(_clientsLock);
			clients = _clients;
		}

		fd_set readSet, writeSet;
		FD_ZERO(&readSet);
		FD_ZERO(&writeSet);
		FD_SET(static_cast<SOCKET>(_listen), &readSet);
		for (const ClientRef& c : clients)
		{
			FD_SET(c->socket, &readSet);
			bool pending = c->sending.empty() == false;
			if (pending == false)
			{
				std::lock_guard<std::mutex> guard(c->outLock);
				pending = c->out.empty() == false;
			}
			if (pending)
				FD_SET(c->socket, &writeSet);
		}
		timeval tv{ 0, 50'000 };
		::select(0, &readSet, &writeSet, nullptr, &tv);

		if (FD_ISSET(static_cast<SOCKET>(_listen), &readSet))
		{
			SOCKET s = ::accept(static_cast<SOCKET>(_listen), nullptr, nullptr);
			if (s != INVALID_SOCKET)
			{
				if (static_cast<int32>(clients.size()) >= MAX_CLIENTS)
				{
					const char* busy = "{\"t\":\"error\",\"msg\":\"too many admin clients\"}\n";
					::send(s, busy, static_cast<int>(::strlen(busy)), 0);
					::closesocket(s);
				}
				else
				{
					u_long nonBlocking = 1;
					::ioctlsocket(s, FIONBIO, &nonBlocking);
					auto c = make_shared<Client>();
					c->socket = s;
					std::lock_guard<std::mutex> guard(_clientsLock);
					_clients.push_back(c);
					LOG_LINE << "[ADMIN] client connected (" << _clients.size() << "/" << MAX_CLIENTS << ")\n";
				}
			}
		}

		for (const ClientRef& c : clients)
		{
			if (FD_ISSET(c->socket, &readSet))
			{
				const int n = ::recv(c->socket, buf, sizeof(buf), 0);
				if (n <= 0)
					c->closed.store(true);
				else
				{
					c->in.append(buf, static_cast<size_t>(n));
					if (c->in.size() > 1024 * 1024)
						c->closed.store(true);		// 줄 끝 없이 1 MB - 정상 앱이 아니다
					size_t nl;
					while (c->closed.load() == false && (nl = c->in.find('\n')) != string::npos)
					{
						string line = c->in.substr(0, nl);
						c->in.erase(0, nl + 1);
						if (line.empty() == false && line.back() == '\r')
							line.pop_back();
						if (line.empty() == false)
							HandleLine(c, line);
					}
				}
			}

			// 보내기 - 줄 하나씩, 반쯤 보낸 것은 다음 바퀴에
			for (int32 burst = 0; burst < 256 && c->closed.load() == false; burst++)
			{
				if (c->sending.empty())
				{
					std::lock_guard<std::mutex> guard(c->outLock);
					if (c->out.empty())
						break;
					c->sending = move(c->out.front());
					c->out.pop_front();
					c->outBytes -= c->sending.size();
					c->sendingAt = 0;
				}
				const int n = ::send(c->socket, c->sending.data() + c->sendingAt, static_cast<int>(c->sending.size() - c->sendingAt), 0);
				if (n == SOCKET_ERROR)
				{
					if (::WSAGetLastError() != WSAEWOULDBLOCK)
						c->closed.store(true);
					break;
				}
				c->sendingAt += static_cast<size_t>(n);
				if (c->sendingAt < c->sending.size())
					break;
				c->sending.clear();
			}
		}

		// 닫힌 접속자 정리
		{
			std::lock_guard<std::mutex> guard(_clientsLock);
			for (auto it = _clients.begin(); it != _clients.end();)
			{
				if ((*it)->closed.load())
				{
					::closesocket((*it)->socket);
					LOG_LINE << "[ADMIN] client left" << ((*it)->dropped.load() ? " - dropped " + to_string((*it)->dropped.load()) + " event line(s) for a slow reader" : string()) << '\n';
					it = _clients.erase(it);
				}
				else
					++it;
			}
		}

		const uint64 nowUs = Utils::NowMicroseconds();
		PumpMaps(nowUs);
		if (nowUs - lastMetricsCheckUs >= 500'000)
		{
			lastMetricsCheckUs = nowUs;
			PumpMetrics();
		}
	}

	std::lock_guard<std::mutex> guard(_clientsLock);
	for (const ClientRef& c : _clients)
		::closesocket(c->socket);
	_clients.clear();
	::closesocket(static_cast<SOCKET>(_listen));
}

void AdminServer::HandleLine(const ClientRef& c, const string& line)
{
	const string op = AdminJson::GetString(line, "op");
	if (c->authed == false)
	{
		if (op == "hello" && AdminJson::GetString(line, "token") == _token)
		{
			c->authed = true;
			string tags = "[";
			for (size_t i = 0; i < size(EventFormat::TAGS); i++)
				tags += string(i ? "," : "") + "\"" + EventFormat::TAGS[i] + "\"";
			tags += "]";
			Send(c, "{\"t\":\"hello\",\"ok\":true,\"pid\":" + to_string(::GetCurrentProcessId()) + ",\"port\":" + to_string(_config.gamePort)
				+ ",\"admin_port\":" + to_string(_config.port) + ",\"boot\":\"" + _boot + "\",\"dataDir\":\"" + JsonEscape(_config.dataDir) + "\""
				+ ",\"eventsFile\":\"" + JsonEscape(GEvents.FilePath()) + "\",\"tags\":" + tags + "}");
			LOG_LINE << "[ADMIN] client authenticated - " << AdminJson::GetString(line, "client") << '\n';
		}
		else
		{
			LOG_LINE << "[ADMIN] rejected a client - " << (op == "hello" ? "wrong token" : "first message was not hello") << '\n';
			// 큐를 거치지 않고 바로 - 닫히는 접속은 보내기 차례가 오지 않는다
			static const char* refused = "{\"t\":\"error\",\"msg\":\"unauthorized\"}\n";
			::send(c->socket, refused, static_cast<int>(::strlen(refused)), 0);
			c->closed.store(true);
		}
		return;
	}

	if (op == "ping")
		Send(c, "{\"t\":\"pong\"}");
	else if (op == "events")
	{
		const int64 backlog = (std::clamp)(AdminJson::GetInt(line, "backlog", 2000), 0ll, static_cast<int64>(EventLog::RING_CAPACITY));
		// 지난 줄을 먼저 넣고 실시간을 켠다 - 그 사이에 쓰인 줄이 한두 개 겹칠 수 있다 (앱이 seq 로 거른다)
		for (string& old : GEvents.Recent(static_cast<size_t>(backlog)))
			Send(c, move(old));
		c->events.store(true);
	}
	else if (op == "players")
		Send(c, "{\"t\":\"players\",\"list\":" + PlayersJson() + "}");
	else if (op == "player")
	{
		const string p = PlayerJson(static_cast<uint64>(AdminJson::GetInt(line, "id")));
		Send(c, p.empty() ? string("{\"t\":\"player\",\"p\":null}") : "{\"t\":\"player\",\"p\":" + p + "}");
	}
	else if (op == "rooms")
		Send(c, "{\"t\":\"rooms\",\"list\":" + RoomsJson() + "}");
	else if (op == "map")
	{
		c->mapRoom = AdminJson::GetString(line, "room");
		c->nextMapUs = 0;
		c->mapCount = 0;
	}
	else if (op == "metrics")
	{
		c->metrics.store(true);
		Send(c, MetricsJson());
	}
	else if (op == "kick")
		Send(c, Kick(static_cast<uint64>(AdminJson::GetInt(line, "id")), AdminJson::GetString(line, "reason")));
	else if (op == "unban")
		Unban(c, static_cast<uint64>(AdminJson::GetInt(line, "account")), AdminJson::GetString(line, "name"));
	else if (op == "notice")
		Send(c, Notice(AdminJson::GetString(line, "text")));
	else
		Send(c, "{\"t\":\"error\",\"msg\":\"unknown op " + JsonEscape(op) + "\"}");
}

void AdminServer::PumpMaps(uint64 nowUs)
{
	vector<ClientRef> clients;
	{
		std::lock_guard<std::mutex> guard(_clientsLock);
		clients = _clients;
	}
	vector<RoomRef> rooms;
	for (const ClientRef& c : clients)
	{
		if (c->authed == false)
			continue;
		if (c->mapPending && c->mapFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		{
			c->mapPending = false;
			const string json = c->mapFuture.get();
			if (json.empty() == false)
				Send(c, "{\"t\":\"map\"," + json.substr(1));
		}
		if (c->mapRoom.empty() || c->mapPending || nowUs < c->nextMapUs)
			continue;
		if (rooms.empty())
			rooms = AllRooms();
		RoomRef target;
		for (const RoomRef& r : rooms)
			if (r->Tag() == c->mapRoom)
				target = r;
		if (target == nullptr)
		{
			Send(c, "{\"t\":\"map\",\"room\":\"" + JsonEscape(c->mapRoom) + "\",\"gone\":true,\"objs\":[]}");
			c->mapRoom.clear();
			continue;
		}
		const bool routes = (c->mapCount++ % 25) == 0;		// 순찰 길은 처음 한 번 + 5 초마다
		c->mapFuture = AskRoom(target, [routes](Room& room) { return room.AdminMapJson(routes); });
		c->mapPending = true;
		c->nextMapUs = nowUs + 200'000;		// 초당 5 번
	}
}

void AdminServer::PumpMetrics()
{
	const uint64 seq = GStats.GetSnapshot().seq;
	if (seq == _metricsSeq)
		return;
	_metricsSeq = seq;
	Broadcast(MetricsJson(), true);
}

string AdminServer::PlayersJson()
{
	vector<std::future<string>> asks;
	for (const RoomRef& room : AllRooms())
		asks.push_back(AskRoom(room, [](Room& r) { return r.AdminPlayersJson(); }));

	string list;
	for (std::future<string>& f : asks)
	{
		const string inner = Inner(Wait(f));
		if (inner.empty() == false)
			list += (list.empty() ? "" : ",") + inner;
	}

	// 방에 몸이 없는 접속 - 로그인 · 캐릭터 고르기 · 베이스캠프로 가는 중
	for (const GameSessionRef& s : GSessionManager.Snapshot())
	{
		const uint64 account = s->accountId.load();
		if (s->player.load() != nullptr)
			continue;
		const string name = s->characters.empty() ? string() : s->characters[0].name();
		const uint64 id = s->characters.empty() ? 0 : s->characters[0].object_info().object_id();
		list += string(list.empty() ? "" : ",") + "{\"id\":" + to_string(id) + ",\"name\":\"" + JsonEscape(name) + "\",\"acc\":" + to_string(account)
			+ ",\"room\":\"\",\"state\":\"" + (account == 0 ? "connecting" : "lobby") + "\",\"hp\":0,\"mhp\":0,\"x\":0,\"y\":0"
			+ ",\"bot\":" + (name.rfind("bot_", 0) == 0 ? "true" : "false") + ",\"party\":0}";
	}
	return "[" + list + "]";
}

string AdminServer::RoomsJson()
{
	vector<std::future<string>> asks;
	for (const RoomRef& room : AllRooms())
		asks.push_back(AskRoom(room, [](Room& r) { return r.AdminSummaryJson(); }));
	string list;
	for (std::future<string>& f : asks)
	{
		const string one = Wait(f);
		if (one.empty() == false)
			list += (list.empty() ? "" : ",") + one;
	}
	return "[" + list + "]";
}

string AdminServer::PlayerJson(uint64 characterId)
{
	GameSessionRef session = GSessionManager.FindByCharacter(characterId);
	PlayerRef player = (session != nullptr) ? session->player.load() : nullptr;
	RoomRef room = (player != nullptr) ? player->room.load().lock() : nullptr;
	if (room == nullptr)
		return string();
	std::future<string> f = AskRoom(room, [characterId](Room& r) { return r.AdminPlayerJson(characterId); });
	return Wait(f);
}

string AdminServer::MetricsJson()
{
	const ServerStats::Snapshot s = GStats.GetSnapshot();
	char buf[640];
	::snprintf(buf, sizeof(buf),
		"{\"t\":\"metrics\",\"m\":{\"seq\":%llu,\"sessions\":%d,\"flush_n\":%llu,\"flush_p95_us\":%llu,\"flush_p99_us\":%llu,\"flush_max_us\":%llu,"
		"\"recv_mbps\":%.3f,\"sent_mbps\":%.3f,\"per_client_kbps\":%.1f,\"move_reject\":%llu,\"commit_mb\":%.1f,\"workingset_mb\":%.1f,"
		"\"db_pending\":%d,\"db_delayed\":%d,\"instances\":%d,\"events\":%llu}}",
		s.seq, s.sessions, s.flushCount, s.flushP95Us, s.flushP99Us, s.flushMaxUs, s.recvMbps, s.sentMbps, s.perClientKbps, s.moveRejects,
		s.commitMB, s.workingSetMB, s.dbPending, s.dbDelayed, GInstances.LiveCount(), static_cast<unsigned long long>(GEvents.Count()));
	return buf;
}

string AdminServer::Kick(uint64 characterId, const string& reason)
{
	GameSessionRef session = GSessionManager.FindByCharacter(characterId);
	if (session == nullptr)
		return Result("kick", false, "접속 중이 아니다 - char=" + to_string(characterId));
	Protocol::S_KICK kick;
	kick.set_reason(Protocol::KICK_REASON_NONE);		// 클라는 "서버가 연결을 끊었습니다" (login.error.kicked) - .proto 에 관리자 사유가 없다
	session->Send(ServerPacketHandler::MakeSendBuffer(kick));
	session->Disconnect(L"admin kick");
	LOG_LINE << "[ADMIN] kick char=" << characterId << " account=" << session->accountId.load() << " reason=\"" << reason << "\"\n";
	return Result("kick", true, "강제 종료 - char=" + to_string(characterId));
}

string AdminServer::Notice(const string& text)
{
	if (text.empty() || text.size() > 400)
		return Result("notice", false, "공지는 1 ~ 400 바이트");
	Protocol::S_CHAT chat;
	chat.set_playerid(0);		// 0 = 서버 공지 (클라 Handle_S_CHAT 이 HUD 알림으로)
	chat.set_msg(text);
	SendBufferRef buffer = ServerPacketHandler::MakeSendBuffer(chat);
	int32 sent = 0;
	for (const GameSessionRef& s : GSessionManager.Snapshot())
	{
		if (s->accountId.load() == 0)
			continue;
		s->Send(buffer);
		sent++;
	}
	LOG_LINE << "[ADMIN] notice sent=" << sent << " text=\"" << text << "\"\n";
	return Result("notice", true, "공지를 " + to_string(sent) + " 명에게 보냈다");
}

void AdminServer::Unban(const ClientRef& client, uint64 accountId, const string& name)
{
	if (accountId == 0 && name.empty())
	{
		Send(client, Result("unban", false, "account 또는 name 이 필요하다"));
		return;
	}
	weak_ptr<Client> weak = client;
	GDBQueue.Push([this, weak, accountId, name](DBConnection* conn)
		{
			uint64 account = accountId;
			if (account == 0)
			{
				DBStatement find(*conn, "SELECT account_id FROM characters WHERE name = ?");
				vector<string> row;
				if (find.BindString(name).Query() && find.Fetch(OUT row) && row.empty() == false)
					account = ::_strtoui64(row[0].c_str(), nullptr, 10);
			}
			string msg;
			bool ok = false;
			if (account == 0)
				msg = "그 이름의 캐릭터가 없다 - " + name;
			else
			{
				DBStatement clear(*conn, "UPDATE accounts SET banned_until = NULL WHERE account_id = ?");
				DBStatement strikes(*conn, "DELETE FROM cheat_strikes WHERE account_id = ?");
				ok = clear.BindUInt64(account).Execute() && strikes.BindUInt64(account).Execute();
				const uint64 removed = ok ? strikes.AffectedRows() : 0;
				msg = ok ? "정지 해제 - account=" + to_string(account) + " (strike " + to_string(removed) + " 줄 지움)" : "DB 오류 " + string(clear.Error());
				LOG_LINE << "[ADMIN] unban account=" << account << (ok ? " ok" : " failed") << " strikes_removed=" << removed << '\n';
			}
			if (ClientRef c = weak.lock())
				Send(c, Result("unban", ok, msg));
		});
}
