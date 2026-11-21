#include "pch.h"
#include "Cluster.h"
#include "DBConnection.h"
#include "DBJobQueue.h"
#include "AccountManager.h"
#include "GameSession.h"
#include "ServerPacketHandler.h"

namespace Cluster
{
	namespace
	{
		Config				GConfig;
		atomic<bool>		GEnabled = false;
		atomic<bool>		GBroken = false;			// 질의가 실패했다 (표가 없는 옛 DB 등) - 한 번 알리고 혼자 도는 서버처럼
		uint64				GLastBeatUs = 0;			// 주 루프 전용
		uint64				GLastPollUs = 0;
		atomic<bool>		GPollInFlight = false;		// 끊기 부탁 확인이 DB 큐에 이미 있다 - 밀려 있으면 겹쳐 쌓지 않는다
		atomic<bool>		GBeatInFlight = false;

		constexpr uint64	BEAT_US = 5'000'000;
		constexpr uint64	POLL_US = 1'000'000;
		constexpr int32		STALE_SEC = 15;			// 이만큼 소식이 없는 서버의 접속 줄은 새 서버가 가져간다

		void Broken(const char* what, DBConnection* conn)
		{
			if (GBroken.exchange(true) == false)
				LOG_LINE << "[CLUSTER] " << what << " failed - err=" << conn->GetError() << " (missing Tools/db/schema_cluster.sql?) - running as a lone server\n";
		}

		bool Beat(DBConnection* conn, int32 ccu, int32 channels, bool boot)
		{
			char query[512];
			::snprintf(query, sizeof(query),
				"INSERT INTO game_servers (server_id, host, port, admin_port, ccu, max_sessions, channels, status, started_at, updated_at) "
				"VALUES (%u, '%s', %u, %u, %d, %d, %d, 'up', NOW(), NOW()) "
				"ON DUPLICATE KEY UPDATE host = VALUES(host), port = VALUES(port), admin_port = VALUES(admin_port), ccu = VALUES(ccu), "
				"max_sessions = VALUES(max_sessions), channels = VALUES(channels), status = 'up', updated_at = NOW()%s",
				GConfig.serverId, conn->Escape(GConfig.host).c_str(), GConfig.port, GConfig.adminPort, ccu, GConfig.maxSessions, channels,
				boot ? ", started_at = NOW()" : "");
			if (conn->Execute(query) == false)
			{
				Broken("register", conn);
				return false;
			}
			return true;
		}

		void PollKicks(DBConnection* conn)
		{
			char query[256];
			::snprintf(query, sizeof(query), "SELECT account_id FROM online_accounts WHERE server_id = %u AND kick_requested = 1 LIMIT 500", GConfig.serverId);
			MYSQL_RES* res = conn->Query(query);
			if (res == nullptr)
			{
				Broken("kick poll", conn);
				return;
			}
			vector<uint64> accounts;
			while (MYSQL_ROW row = ::mysql_fetch_row(res))
				accounts.push_back(::_strtoui64(row[0], nullptr, 10));
			conn->FreeResult(res);

			for (uint64 accountId : accounts)
			{
				if (GameSessionRef older = GAccountManager.FindOwner(accountId))
				{
					LOG_LINE << "[CLUSTER] kick - account=" << accountId << " logged in on another server\n";
					Protocol::S_KICK kickPkt;
					kickPkt.set_reason(Protocol::KICK_REASON_DUPLICATE_LOGIN);
					older->Send(ServerPacketHandler::MakeSendBuffer(kickPkt));
					older->Disconnect(L"logged in on another server");
				}
				// 줄을 지워 부탁한 서버가 가져가게 한다 - 이 서버에 같은 계정이 아직 붙어 있어도(끊는 중) 이제 저쪽 것이다
				::snprintf(query, sizeof(query), "DELETE FROM online_accounts WHERE account_id = %llu AND server_id = %u", accountId, GConfig.serverId);
				conn->Execute(query);
			}
		}
	}

	void Init(const Config& config)
	{
		GConfig = config;
		GEnabled.store(config.enabled);
	}

	bool Enabled()
	{
		return GEnabled.load() && GBroken.load() == false;
	}

	uint32 ServerId()
	{
		return GConfig.serverId;
	}

	void Boot(DBConnection* conn)
	{
		if (GEnabled.load() == false || conn == nullptr)
			return;
		if (Beat(conn, 0, 0, true) == false)
			return;

		char query[160];
		::snprintf(query, sizeof(query), "DELETE FROM online_accounts WHERE server_id = %u", GConfig.serverId);
		conn->Execute(query);
		LOG_LINE << "[CLUSTER] registered server_id=" << GConfig.serverId << " " << GConfig.host << ":" << GConfig.port
			<< " admin=" << GConfig.adminPort << " (cleared " << conn->GetAffectedRows() << " stale online row(s))\n";
	}

	Claim ClaimAccount(DBConnection* conn, uint64 accountId, uint64 characterId)
	{
		if (Enabled() == false || accountId == 0)
			return Claim::Ok;

		char query[640];
		// 있으면 그대로 두고, 없으면 이 서버 것으로
		::snprintf(query, sizeof(query),
			"INSERT INTO online_accounts (account_id, server_id, character_id, kick_requested, since) VALUES (%llu, %u, %llu, 0, NOW()) "
			"ON DUPLICATE KEY UPDATE account_id = account_id",
			accountId, GConfig.serverId, characterId);
		if (conn->Execute(query) == false)
		{
			Broken("claim", conn);
			return Claim::Ok;
		}

		::snprintf(query, sizeof(query),
			"SELECT o.server_id, IFNULL(TIMESTAMPDIFF(SECOND, g.updated_at, NOW()), 99999), IFNULL(g.status, 'down') "
			"FROM online_accounts o LEFT JOIN game_servers g ON g.server_id = o.server_id WHERE o.account_id = %llu", accountId);
		MYSQL_RES* res = conn->Query(query);
		if (res == nullptr)
		{
			Broken("claim select", conn);
			return Claim::Ok;
		}
		uint32 owner = 0;
		int64 ageSec = 99999;
		string status = "down";
		if (MYSQL_ROW row = ::mysql_fetch_row(res))
		{
			owner = static_cast<uint32>(::strtoul(row[0], nullptr, 10));
			ageSec = ::_strtoi64(row[1], nullptr, 10);
			status = row[2] != nullptr ? row[2] : "down";
		}
		conn->FreeResult(res);

		if (owner == GConfig.serverId || owner == 0)
		{
			::snprintf(query, sizeof(query), "UPDATE online_accounts SET character_id = %llu, kick_requested = 0 WHERE account_id = %llu AND server_id = %u",
				characterId, accountId, GConfig.serverId);
			conn->Execute(query);
			return Claim::Ok;
		}

		// 주인 서버가 죽었다 (소식 없음 · down) - 가져간다
		if (ageSec > STALE_SEC || status != "up")
		{
			::snprintf(query, sizeof(query),
				"UPDATE online_accounts SET server_id = %u, character_id = %llu, kick_requested = 0, since = NOW() WHERE account_id = %llu AND server_id = %u",
				GConfig.serverId, characterId, accountId, owner);
			conn->Execute(query);
			LOG_LINE << "[CLUSTER] took over account=" << accountId << " from silent server " << owner << " (" << ageSec << " s)\n";
			return Claim::Ok;
		}

		// 살아 있는 다른 서버 - 끊어 달라고 깃발을 세운다
		::snprintf(query, sizeof(query), "UPDATE online_accounts SET kick_requested = 1 WHERE account_id = %llu AND server_id = %u", accountId, owner);
		conn->Execute(query);
		LOG_LINE << "[CLUSTER] account=" << accountId << " is online on server " << owner << " - asked it to kick, retrying\n";
		return Claim::OtherServer;
	}

	void ReleaseAccount(uint64 accountId)
	{
		if (Enabled() == false || accountId == 0)
			return;
		GDBQueue.Push([accountId](DBConnection* conn)
			{
				// 그 사이 같은 계정이 이 서버에 다시 붙었다 (재접속 · 중복 로그인으로 새 연결) - 그 연결의 줄이다
				if (GAccountManager.FindOwner(accountId) != nullptr)
					return;
				char query[160];
				::snprintf(query, sizeof(query), "DELETE FROM online_accounts WHERE account_id = %llu AND server_id = %u", accountId, GConfig.serverId);
				conn->Execute(query);
			}, DBPriority::Low);
	}

	void Tick(uint64 nowUs, int32 ccu, int32 channels)
	{
		if (Enabled() == false)
			return;

		if (nowUs - GLastBeatUs >= BEAT_US && GBeatInFlight.exchange(true) == false)
		{
			GLastBeatUs = nowUs;
			GDBQueue.Push([ccu, channels](DBConnection* conn) { Beat(conn, ccu, channels, false); GBeatInFlight.store(false); });
		}

		if (nowUs - GLastPollUs >= POLL_US && GPollInFlight.exchange(true) == false)
		{
			GLastPollUs = nowUs;
			GDBQueue.Push([](DBConnection* conn) { PollKicks(conn); GPollInFlight.store(false); });
		}
	}

	void Shutdown()
	{
		if (Enabled() == false)
			return;
		GDBQueue.Push([](DBConnection* conn)
			{
				char query[200];
				::snprintf(query, sizeof(query), "UPDATE game_servers SET status = 'down', ccu = 0, updated_at = NOW() WHERE server_id = %u", GConfig.serverId);
				conn->Execute(query);
				::snprintf(query, sizeof(query), "DELETE FROM online_accounts WHERE server_id = %u", GConfig.serverId);
				conn->Execute(query);
			}, DBPriority::High);
	}
}
