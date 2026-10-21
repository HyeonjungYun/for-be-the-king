#include "pch.h"
#include "EventLog.h"
#include "CsvTable.h"
#include "ItemTable.h"

// 구조화 이벤트 로그 (2026-10-21 · EventLog.h · Tools/ServerMonitor/PROTOCOL.md). 텍스트 로그 한 줄 → 태그 · 행위자 · 대상, JSON 한 줄.
// 🔴 파일 · 스레드 없이 순수 함수만 (EventFormat · AdminJson). 실제 event_tags.csv 는 마지막 테스트가 읽는다
namespace
{
	using namespace EventFormat;

	vector<TagRule> Rules()
	{
		vector<TagRule> rules;
		string error;
		const string csv = "prefix,keyword,tag,sub\n"
			"BASECAMP,enhance,economy,enhance\n"
			"BASECAMP,,storage,storage\n"
			"SKILL FIRE,,combat,skill_fire\n"
			"DIED,,combat,died\n"
			"LOGIN,,connect,login\n";
		EXPECT_TRUE(ParseRules(csv, OUT rules, OUT error)) << error;
		return rules;
	}
}

TEST(EventLog, 머리를_떼고_줄머리로_태그를_고른다)
{
	const Parsed e = ParseLogLine("2026-10-21 20:01:40.313 I 08 [SKILL FIRE] caster=128 skill=1007 targets=1 cd=8000ms\n", Rules());
	EXPECT_EQ(e.ts, "2026-10-21 20:01:40.313");
	EXPECT_EQ(e.level, 'I');
	EXPECT_EQ(e.thread, 8);
	EXPECT_EQ(e.prefix, "SKILL FIRE");
	EXPECT_EQ(e.tag, "combat");
	EXPECT_EQ(e.sub, "skill_fire");
	ASSERT_EQ(e.actors.size(), 1u);
	EXPECT_EQ(e.actors[0], 128u);
	EXPECT_TRUE(e.targets.empty());
	EXPECT_EQ(e.msg, "[SKILL FIRE] caster=128 skill=1007 targets=1 cd=8000ms");
}

TEST(EventLog, 핵심어가_있는_규칙이_먼저다)
{
	EXPECT_EQ(ParseLogLine("[BASECAMP] Enhance char=5 item=9", Rules()).tag, "economy");		// 대소문자 무시
	EXPECT_EQ(ParseLogLine("[BASECAMP] ToVault char=5 item=9", Rules()).tag, "storage");
	// 표에 없는 줄머리는 system / 줄머리 소문자, 줄머리가 없으면 system / log
	const Parsed unknown = ParseLogLine("[SLOW FLUSH] room=w0 took=40ms", Rules());
	EXPECT_EQ(unknown.tag, "system");
	EXPECT_EQ(unknown.sub, "slow_flush");
	EXPECT_EQ(unknown.room, "w0");
	EXPECT_EQ(ParseLogLine("plain text", Rules()).sub, "log");
}

TEST(EventLog, 행위자와_대상_계정을_키로_가른다)
{
	const Parsed died = ParseLogLine("[DIED] victim=171 by=128 room=w2 account=33", Rules());
	ASSERT_EQ(died.actors.size(), 1u);
	EXPECT_EQ(died.actors[0], 128u);
	ASSERT_EQ(died.targets.size(), 1u);
	EXPECT_EQ(died.targets[0], 171u);
	EXPECT_EQ(died.account, 33u);
	EXPECT_EQ(died.room, "w2");
	// 숫자가 아닌 값 · 모르는 키는 id 가 아니다 · 같은 id 는 한 번만
	const Parsed odd = ParseLogLine("[LOGIN] ok char=12 char=12 name=bot_1 type=101 reason=wall", Rules());
	ASSERT_EQ(odd.actors.size(), 1u);
	EXPECT_EQ(odd.actors[0], 12u);
	EXPECT_TRUE(odd.targets.empty());
	// id= 는 다른 행위자 키가 없을 때만 - 장판 · 투사체 id 가 플레이어로 섞이면 플레이어별 필터에 남의 줄이 뜬다
	const Parsed zone = ParseLogLine("[ZONE] place id=119 caster=2 skill=1063 r=45", Rules());
	ASSERT_EQ(zone.actors.size(), 1u);
	EXPECT_EQ(zone.actors[0], 2u);
	const Parsed bare = ParseLogLine("[SESSION] released id=41", Rules());
	ASSERT_EQ(bare.actors.size(), 1u);
	EXPECT_EQ(bare.actors[0], 41u);
}

TEST(EventLog, JSON_한_줄은_따옴표_역슬래시_제어문자를_이스케이프한다)
{
	Parsed e;
	e.ts = "2026-10-21 20:00:00.000";
	e.tag = "system";
	e.sub = "admin";
	e.actors = { 1, 2 };
	e.msg = "say \"hi\"\\ \t\x01 한글";
	const string json = ToJson(e, 7, "ev", "{\"dmg\":3}");
	EXPECT_NE(json.find("\"seq\":7"), string::npos) << json;
	EXPECT_NE(json.find("\"a\":[1,2]"), string::npos) << json;
	EXPECT_NE(json.find("\"tg\":[]"), string::npos) << json;
	EXPECT_NE(json.find("\"src\":\"ev\""), string::npos) << json;
	EXPECT_NE(json.find("\"d\":{\"dmg\":3}"), string::npos) << json;
	EXPECT_NE(json.find("say \\\"hi\\\"\\\\ \\t\\u0001 한글"), string::npos) << json;
	EXPECT_EQ(json.front(), '{');
	EXPECT_EQ(json.back(), '}');
	EXPECT_EQ(json.find('\n'), string::npos);
}

TEST(EventLog, 관리_요청_JSON을_읽는다)
{
	const string line = "{\"op\":\"notice\",\"text\":\"\\uc810\\uac80 \\\"\\uc911\\\"\",\"id\":\"128\",\"backlog\": 500,\"neg\":-3}";
	EXPECT_EQ(AdminJson::GetString(line, "op"), "notice");
	EXPECT_EQ(AdminJson::GetString(line, "text"), "점검 \"중\"");
	EXPECT_EQ(AdminJson::GetInt(line, "id"), 128);
	EXPECT_EQ(AdminJson::GetInt(line, "backlog"), 500);
	EXPECT_EQ(AdminJson::GetInt(line, "neg"), -3);
	EXPECT_EQ(AdminJson::GetInt(line, "missing", 9), 9);
	EXPECT_EQ(AdminJson::GetString(line, "missing"), "");
}

TEST(EventLog, 표의_태그가_틀리면_거부하고_실제_표는_통과한다)
{
	vector<TagRule> rules;
	string error;
	EXPECT_FALSE(ParseRules("prefix,keyword,tag,sub\nMOVE,,teleport,x\n", OUT rules, OUT error));
	EXPECT_NE(error.find("unknown tag"), string::npos) << error;
	EXPECT_FALSE(ParseRules("prefix,keyword,tag,sub\n,,move,x\n", OUT rules, OUT error));
	EXPECT_FALSE(ParseRules("prefix,keyword,tag,sub\n", OUT rules, OUT error));

	string text;
	ASSERT_TRUE(Csv::ReadFile(ItemTable::DefaultDataDir() + "\\event_tags.csv", OUT text));
	ASSERT_TRUE(ParseRules(text, OUT rules, OUT error)) << error;
	EXPECT_GE(rules.size(), 100u);
	// 표의 13 종이 전부 한 번 이상 쓰인다
	for (const char* tag : TAGS)
	{
		bool used = false;
		for (const TagRule& r : rules)
			used = used || r.tag == tag;
		EXPECT_TRUE(used) << tag;
	}
}
