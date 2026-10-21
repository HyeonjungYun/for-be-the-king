#pragma once
#include <functional>
#include <mutex>

class AsyncLogger;

/*-------------
	EventLog
-------------*/

// 구조화 이벤트 로그 (2026-10-21 · Tools/ServerMonitor/PROTOCOL.md). 한 줄 = JSON 하나:
//   {"t":"ev","seq":..,"ts":"..","lv":"I","th":8,"tag":"combat","sub":"skill_fire","a":[actor ids],"tg":[target ids],"acc":0,"room":"w0","src":"log","msg":".."}
// 두 갈래로 들어온다.
//   1) 텍스트 로그 미러 - AsyncLogger 의 관찰자(쓰기 스레드)가 LOG_LINE 한 줄마다 FromLogLine. 줄머리 [XXX] + 핵심어를 event_tags.csv 로 태그 13 종에,
//      줄 안의 char= · caster= · victim= … 를 행위자 · 대상 id 로. 로그 477 곳을 고치지 않고 전부 담고, 새 로그는 표에 한 줄이면 된다
//   2) 직접 이벤트 - 텍스트 로그가 없는 잦은 사건(피해 · CC) · 관리 명령. 게임 스레드가 Emit 을 부른다
// 출력은 전용 쓰기 스레드 하나: Logs/events/events-<부팅>.jsonl + 최근 N 줄 고리 버퍼 + 관리 포트 구독자(AdminServer).
// 🔴 게임 스레드는 큐에 넣기만 한다 - 파일 · 소켓을 만지지 않는다 (AsyncLogger 와 같은 원칙, roadmap § 0-11)

namespace EventFormat
{
	struct TagRule
	{
		string	prefix;		// 줄머리 [XXX] 안 - "SKILL FIRE"
		string	keyword;	// 비면 아무 줄. 아니면 줄에 이 글자가 있어야 (대소문자 무시)
		string	tag;		// 13 종 중 하나
		string	sub;
	};

	extern const char* const TAGS[13];		// connect move combat status item storage economy party escape cheat monster db system

	bool	IsKnownTag(const string& tag);
	// event_tags.csv (prefix,keyword,tag,sub). 모르는 태그 · 빈 prefix 는 거부. 순서 = 우선순위 (먼저 맞는 줄)
	bool	ParseRules(const string& csv, OUT vector<TagRule>& out, OUT string& error);

	struct Parsed
	{
		string			ts;			// "2026-10-21 20:01:40.313" - 머리가 없으면 빈 칸
		char			level = 'I';
		int32			thread = 0;
		string			prefix;		// 줄머리 [XXX] - 없으면 빈 칸
		string			tag = "system";
		string			sub = "log";
		vector<uint64>	actors;
		vector<uint64>	targets;
		uint64			account = 0;
		string			room;
		string			msg;		// 머리를 뗀 본문 (줄바꿈 제외)
	};

	// LogLine 한 덩어리(머리 "YYYY-MM-DD HH:MM:SS.mmm L TT " + 본문)를 가른다. 여러 줄이면 첫 줄만 본다
	Parsed	ParseLogLine(const string& text, const vector<TagRule>& rules);

	string	JsonEscape(const string& s);
	// t:"ev" 한 줄 (끝 줄바꿈 없음). src = "log" | "ev". data = 이미 JSON 객체인 문자열 ("{...}") 또는 빈 칸
	string	ToJson(const Parsed& e, uint64 seq, const char* src, const string& data = string());
}

// 관리 요청의 아주 작은 JSON 읽기 (순수 - EventLogTest). 문자열은 \" \\ \n \uXXXX(→ UTF-8) 를 푼다. 없으면 빈 칸 / def
namespace AdminJson
{
	string	GetString(const string& json, const char* key);
	int64	GetInt(const string& json, const char* key, int64 def = 0);
}

class EventLog
{
public:
	using Subscriber = std::function<void(const string& jsonLine)>;

	// rulesCsv = event_tags.csv 내용. dir 이 비면 파일을 쓰지 않는다 (고리 버퍼 · 구독자는 돈다). 실패하면 이유를 남기고 false
	bool	Init(const string& rulesCsv, const string& dir, OUT string& error);
	bool	Enabled() const { return _enabled; }
	const string& FilePath() const { return _path; }

	// 텍스트 로그 미러 - AsyncLogger 관찰자 (쓰기 스레드)
	void	FromLogLine(const string& text, bool toStderr);

	// 직접 이벤트 - 아무 스레드. data 는 JSON 객체 문자열 ("{\"dmg\":37}") 또는 빈 칸
	void	Emit(const char* tag, const char* sub, vector<uint64> actors, vector<uint64> targets, const string& room, const string& msg, const string& data = string());

	// 관리 포트 - 구독자는 쓰기 스레드에서 줄마다 불린다 (가볍게: 큐에 넣기만)
	void	SetSubscriber(Subscriber subscriber);
	vector<string>	Recent(size_t count);		// 최근 count 줄 (오래된 것부터)

	void	Flush();
	uint64	Count() const { return _seq.load(); }		// 지금까지 낸 이벤트 수

	static constexpr size_t RING_CAPACITY = 20'000;

private:
	void	Write(const string& line);		// 쓰기 스레드

	bool					_enabled = false;
	vector<EventFormat::TagRule> _rules;
	string					_path;
	FILE*					_file = nullptr;
	uint64					_unflushed = 0;
	uint64					_lastFlushTick = 0;
	atomic<uint64>			_seq = 0;
	AsyncLogger*			_writer = nullptr;		// 일부러 해제하지 않는다 (GetLogger 와 같은 이유 - 종료 순서)

	std::mutex				_ringLock;
	deque<string>			_ring;
	Subscriber				_subscriber;			// _ringLock 아래
};

extern EventLog GEvents;
