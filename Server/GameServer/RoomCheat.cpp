#include "pch.h"
#include "Room.h"
#include "Player.h"
#include "GameSession.h"
#include "DBJobQueue.h"
#include "DBConnection.h"

// 안티치트 (2026-10-19, design/quick-specs/anticheat-2026-10-19.md · ADR-0019)
//
// 거부 지점(이동 · 공격 · 스킬 · 루팅 · 장비)이 ReportCheat 를 부른다. 판단은 CheatGuard(순수 계산)가 하고, 여기서는 결과를 실행한다:
//   Warn - [CHEAT] warn 한 줄
//   Kick - S_KICK(CHEAT_SUSPECT) → DB 잡: cheat_strikes 한 줄 · 창 안 strike 수로 정지(accounts.banned_until) → 끊기
// 끊기를 DB 잡 끝에 두는 이유: S_KICK 이 나갈 틈을 주고, 정지가 기록된 뒤에 끊어야 바로 다시 붙어도 로그인에서 막힌다.

namespace
{
	atomic<uint64> GCheatWarns = 0;
	atomic<uint64> GCheatKicks = 0;
	atomic<uint64> GCheatBans = 0;
}

void Room::ReportCheat(const CreatureRef& creature, Violation violation, uint64 nowUs)
{
	if (creature == nullptr || creature->IsPlayer() == false)
		return;

	PlayerRef player = static_pointer_cast<Player>(creature);
	const CheatRules& rules = CheatRules::Get();
	const CheatAction action = player->cheat.Report(violation, nowUs, rules);
	if (action == CheatAction::None)
		return;

	const uint64 characterId = player->objectInfo->object_id();
	GameSessionRef session = player->session.lock();
	const uint64 accountId = session != nullptr ? session->accountId.load() : 0;
	const float score = player->cheat.Score(nowUs, rules);
	const string summary = player->cheat.Summary();

	if (action == CheatAction::Warn)
	{
		GCheatWarns.fetch_add(1);
		LOG_LINE << "[CHEAT] warn char=" << characterId << " account=" << accountId << " room=" << Tag()
			<< " score=" << score << " last=" << ViolationName(violation) << " top=" << summary << '\n';
		return;
	}

	// Kick
	GCheatKicks.fetch_add(1);
	LOG_LINE << "[CHEAT] kick char=" << characterId << " account=" << accountId << " room=" << Tag()
		<< " score=" << score << " last=" << ViolationName(violation) << " top=" << summary
		<< " (warns=" << GCheatWarns.load() << " kicks=" << GCheatKicks.load() << " bans=" << GCheatBans.load() << ")\n";

	if (session == nullptr)
		return;

	// S_KICK 은 DB 판정 뒤에 하나만 보낸다 - 정지면 BANNED, 아니면 CHEAT_SUSPECT. 예전엔 여기서 CHEAT_SUSPECT 를 먼저 보내서
	// 클라가 그것을 받자마자 연결을 정리했고, 뒤이은 BANNED 를 못 받아 "정지됐다" 대신 "끊겼다" 를 보였다 (2026-10-19 UI 실측)
	GDBQueue.Push([session, accountId, characterId, score, summary](DBConnection* conn)
		{
			const CheatRules& rules = CheatRules::Get();
			uint64 bannedUntil = 0;

			if (accountId != 0)
			{
				char query[512];
				const string safeDetail = conn->Escape(summary.substr(0, 250));
				::snprintf(query, sizeof(query),
					"INSERT INTO cheat_strikes (account_id, character_id, score, detail) VALUES (%llu, %llu, %.2f, '%s')",
					accountId, characterId, score, safeDetail.c_str());
				if (conn->Excute(query) == false)
					LOG_LINE << "[CHEAT] strike insert failed - account=" << accountId << " err=" << conn->GetError() << '\n';

				// 창 안의 strike 와 지금 시각 - 시계는 MySQL 하나 (인증 서버도 NOW() 로 비교한다)
				::snprintf(query, sizeof(query),
					"SELECT UNIX_TIMESTAMP(created_at), UNIX_TIMESTAMP(NOW()) FROM cheat_strikes "
					"WHERE account_id = %llu AND created_at > NOW() - INTERVAL %u HOUR",
					accountId, rules.banWindowHours);
				vector<uint64> strikes;
				uint64 nowUnix = 0;
				if (MYSQL_RES* result = conn->Query(query))
				{
					while (MYSQL_ROW row = ::mysql_fetch_row(result))
					{
						strikes.push_back(::strtoull(row[0], nullptr, 10));
						nowUnix = ::strtoull(row[1], nullptr, 10);
					}
					conn->FreeResult(result);
				}

				bannedUntil = (nowUnix != 0) ? CheatBan::BanUntil(strikes, nowUnix, rules) : 0;
				if (bannedUntil != 0)
				{
					::snprintf(query, sizeof(query),
						"UPDATE accounts SET banned_until = FROM_UNIXTIME(%llu) WHERE account_id = %llu", bannedUntil, accountId);
					if (conn->Excute(query))
					{
						GCheatBans.fetch_add(1);
						LOG_LINE << "[CHEAT] ban account=" << accountId << " strikes=" << strikes.size() << " in " << rules.banWindowHours
							<< "h until_unix=" << bannedUntil << " (" << rules.banMinutes << " min)\n";
					}
					else
					{
						LOG_LINE << "[CHEAT] ban update failed - account=" << accountId << " err=" << conn->GetError() << '\n';
						bannedUntil = 0;
					}
				}
				else
				{
					LOG_LINE << "[CHEAT] strike account=" << accountId << " strikes=" << strikes.size() << "/" << rules.strikesForBan
						<< " in " << rules.banWindowHours << "h\n";
				}
			}

			{
				Protocol::S_KICK kickPkt;
				kickPkt.set_reason(bannedUntil != 0 ? Protocol::KICK_REASON_BANNED : Protocol::KICK_REASON_CHEAT_SUSPECT);
				kickPkt.set_banned_until(bannedUntil);
				SEND_PACKET_DECLARATION(kickPkt);
				session->Send(sendBuffer);
			}
			session->Disconnect(L"anti-cheat kick");
		});
}

void Room::TestCheat(uint64 playerId, uint32 violation, int32 count)
{
	JOB_TAG();
	auto findIt = _objects.find(playerId);
	if (findIt == _objects.end() || violation >= static_cast<uint32>(Violation::Count))
		return;

	CreatureRef creature = AsCreature(findIt->second);
	const uint64 nowUs = Utils::NowMicroseconds();
	LOG_LINE << "[TEST] cheat char=" << playerId << " violation=" << ViolationName(static_cast<Violation>(violation)) << " x" << count << "\n";
	for (int32 i = 0; i < count; i++)
		ReportCheat(creature, static_cast<Violation>(violation), nowUs);
}
