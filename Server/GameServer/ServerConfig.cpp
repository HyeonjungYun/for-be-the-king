#include "pch.h"
#include "ServerConfig.h"
#include "ItemTable.h"

/*----------------
	ServerConfig
-----------------*/

namespace
{
	// "0" 만 끔 - FBTK_FOW · FBTK_MONSTERS · FBTK_WALLS 의 예전 의미 (raw[0] != '0')
	bool EnvOffSwitch(const char* name, bool defaultValue)
	{
		string value;
		if (ServerConfig::ReadEnv(name, OUT value) == false || value.empty())
			return defaultValue;
		return value[0] != '0';
	}

	// "1" 만 켬 - 나머지 불리언의 예전 의미 (raw[0] == '1')
	bool EnvOnSwitch(const char* name)
	{
		string value;
		return ServerConfig::ReadEnv(name, OUT value) && value.empty() == false && value[0] == '1';
	}

	uint64 EnvU64(const char* name, uint64 defaultValue)
	{
		string value;
		if (ServerConfig::ReadEnv(name, OUT value) == false || value.empty())
			return defaultValue;
		return ::strtoull(value.c_str(), nullptr, 10);
	}

	uint32 EnvU32(const char* name, uint32 defaultValue)
	{
		string value;
		if (ServerConfig::ReadEnv(name, OUT value) == false || value.empty())
			return defaultValue;
		return static_cast<uint32>(::strtoul(value.c_str(), nullptr, 10));
	}

	int32 EnvI32(const char* name, int32 defaultValue)
	{
		string value;
		if (ServerConfig::ReadEnv(name, OUT value) == false || value.empty())
			return defaultValue;
		return static_cast<int32>(::strtol(value.c_str(), nullptr, 10));
	}

	string EnvStr(const char* name, const string& defaultValue)
	{
		string value;
		if (ServerConfig::ReadEnv(name, OUT value) == false || value.empty())
			return defaultValue;
		return value;
	}

	// 1 ~ 65535 만 - 아니면 기본값 (예전 FBTK_DB_PORT · FBTK_PORT 검사)
	uint32 EnvPort(const char* name, uint32 defaultValue)
	{
		string value;
		if (ServerConfig::ReadEnv(name, OUT value) == false || value.empty())
			return defaultValue;
		const int32 parsed = ::atoi(value.c_str());
		return (parsed > 0 && parsed <= 65535) ? static_cast<uint32>(parsed) : defaultValue;
	}
}

bool ServerConfig::ReadEnv(const char* name, OUT string& value)
{
	char* raw = nullptr;
	size_t length = 0;
	if (_dupenv_s(&raw, &length, name) != 0 || raw == nullptr)
		return false;

	value = raw;
	::free(raw);
	return true;
}

ServerConfig ServerConfig::FromEnv()
{
	ServerConfig c;

	c.bindAddress = EnvStr("FBTK_BIND_ADDRESS", c.bindAddress);
	c.port = static_cast<uint16>(EnvPort("FBTK_PORT", c.port));
	c.workerThreads = (std::clamp)(EnvI32("FBTK_WORKER_THREADS", c.workerThreads), 1, 64);

	c.dbHost = EnvStr("FBTK_DB_HOST", c.dbHost);
	c.dbPort = EnvPort("FBTK_DB_PORT", c.dbPort);
	c.dbUser = EnvStr("FBTK_DB_USER", c.dbUser);
	c.dbPassword = EnvStr("FBTK_DB_PASSWORD", string());
	c.dbSchema = EnvStr("FBTK_DB_SCHEMA", c.dbSchema);
	c.dbThreads = (std::clamp)(EnvI32("FBTK_DB_THREADS", c.dbThreads), 1, 32);

	c.dataDir = ItemTable::DefaultDataDir();
	c.journalDir = EnvStr("FBTK_JOURNAL_DIR", c.dataDir + "\\..\\Logs\\pending-commits");
	c.telemetryDir = EnvStr("FBTK_TELEMETRY_DIR", c.dataDir + "\\..\\Logs\\telemetry");		// 검증 툴의 격리 스모크가 보고서 폴더로 돌린다 (2026-10-21)
	c.metricsDir = EnvStr("FBTK_METRICS_DIR", c.dataDir + "\\..\\Logs\\metrics");
	if (c.metricsDir == "0")
		c.metricsDir.clear();
	c.crashDir = EnvStr("FBTK_CRASH_DIR", c.dataDir + "\\..\\Logs\\crash");

	c.fowEnabled = EnvOffSwitch("FBTK_FOW", true);
	c.fowLogViewer = EnvU64("FBTK_FOW_LOG", 0);
	c.monstersOn = EnvOffSwitch("FBTK_MONSTERS", true);
	c.wallCheck = EnvOffSwitch("FBTK_WALLS", true);
	c.botRegear = EnvOnSwitch("FBTK_BOT_REGEAR");
	c.botLoadouts = EnvOnSwitch("FBTK_BOT_LOADOUTS");
	c.disconnectGraceUs = EnvU64("FBTK_DISCONNECT_GRACE_MS", c.disconnectGraceUs / 1000) * 1000;
	c.routeSeed = EnvU64("FBTK_ROUTE_SEED", 0);
	c.idleTimeoutUs = EnvU64("FBTK_IDLE_TIMEOUT_MS", c.idleTimeoutUs / 1000) * 1000;
	c.shutdownDrainMs = EnvU32("FBTK_SHUTDOWN_DRAIN_MS", c.shutdownDrainMs);

#if FBTK_TEST_HOOKS
	c.hooks.failDeathCommit = EnvOnSwitch("FBTK_FAIL_DEATH_COMMIT");
	c.hooks.deathCommitDelayMs = EnvU32("FBTK_DEATH_COMMIT_DELAY_MS", 0);
	c.hooks.baseCampDelayMs = EnvU32("FBTK_BASECAMP_DELAY_MS", 0);
	c.hooks.failEscapeCommits = EnvI32("FBTK_FAIL_ESCAPE_COMMIT", 0);
	c.hooks.fakeAckLoss = EnvI32("FBTK_FAKE_ACK_LOSS", 0);
	c.hooks.fakeLoginFail = EnvI32("FBTK_FAKE_LOGIN_FAIL", 0);
	c.hooks.loginDelayMs = EnvU32("FBTK_LOGIN_DELAY_MS", 0);
	c.hooks.logStallMs = EnvU32("FBTK_LOG_STALL_MS", 0);
	c.hooks.entryFromDb = EnvOnSwitch("FBTK_ENTRY_FROM_DB");
	c.hooks.debugGold = EnvOnSwitch("FBTK_DEBUG_GOLD");
	c.hooks.grantEndpoint = EnvOnSwitch("FBTK_GRANT_ENDPOINT");
	c.hooks.debugCommands = EnvOnSwitch("FBTK_DEBUG_COMMANDS");
	c.hooks.testMode = EnvOnSwitch("FBTK_TEST_MODE");
#endif

	return c;
}

#if FBTK_TEST_HOOKS
bool ServerConfig::AnyHook() const
{
	return hooks.failDeathCommit || hooks.deathCommitDelayMs > 0 || hooks.baseCampDelayMs > 0 || hooks.failEscapeCommits != 0
		|| hooks.fakeAckLoss != 0 || hooks.fakeLoginFail != 0 || hooks.loginDelayMs > 0 || hooks.logStallMs > 0 || hooks.entryFromDb
		|| hooks.debugGold || hooks.grantEndpoint || hooks.debugCommands || hooks.testMode;
}
#endif

void ServerConfig::LogSummary() const
{
	LOG_LINE << "[CONFIG] bind=" << bindAddress << ":" << port << " workers=" << workerThreads
		<< " db=" << dbUser << "@" << dbHost << ":" << dbPort << "/" << dbSchema << " db_threads=" << dbThreads
		<< " data=" << dataDir << '\n';
	LOG_LINE << "[CONFIG] fow=" << (fowEnabled ? "on" : "OFF") << " monsters=" << (monstersOn ? "on" : "OFF") << " walls=" << (wallCheck ? "on" : "OFF")
		<< " bot_regear=" << (botRegear ? 1 : 0) << " bot_loadouts=" << (botLoadouts ? 1 : 0)
		<< " disconnect_grace=" << disconnectGraceUs / 1000 << "ms idle_timeout=" << idleTimeoutUs / 1000 << "ms shutdown_drain=" << shutdownDrainMs << "ms"
		<< " route_seed=" << (routeSeed != 0 ? to_string(routeSeed) : string("boot-time")) << '\n';
	LOG_LINE << "[CONFIG] journal=" << journalDir << " metrics=" << (metricsDir.empty() ? string("off") : metricsDir) << " crash=" << crashDir << '\n';

#if FBTK_TEST_HOOKS
	LOG_LINE << "[CONFIG] test hooks compiled in (FBTK_TEST_HOOKS=1)" << (AnyHook() ? " - SOME ARE ON (see [BOOT] lines)" : " - none enabled") << '\n';
#else
	LOG_LINE << "[CONFIG] test hooks compiled OUT (FBTK_TEST_HOOKS=0) - FBTK_FAIL_* / FBTK_FAKE_* / FBTK_*_DELAY_MS / FBTK_TEST_MODE are ignored\n";
#endif
}
