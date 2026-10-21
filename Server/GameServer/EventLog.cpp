#include "pch.h"
#include "EventLog.h"
#include "CsvTable.h"
#include <cctype>
#include <filesystem>
#include <share.h>

EventLog GEvents;

/*-------- EventFormat (순수 - EventLogTest) --------*/

namespace EventFormat
{
	const char* const TAGS[13] = { "connect", "move", "combat", "status", "item", "storage", "economy", "party", "escape", "cheat", "monster", "db", "system" };

	bool IsKnownTag(const string& tag)
	{
		for (const char* t : TAGS)
			if (tag == t)
				return true;
		return false;
	}

	bool ParseRules(const string& csv, OUT vector<TagRule>& out, OUT string& error)
	{
		out.clear();
		CsvTable table;
		vector<int32> c;
		if (Csv::Parse(csv, "event_tags.csv", OUT table, OUT error) == false
			|| Csv::FindColumns(table, "event_tags.csv", { "prefix", "keyword", "tag", "sub" }, OUT c, OUT error) == false)
			return false;
		for (size_t r = 0; r < table.rows.size(); r++)
		{
			const vector<string>& row = table.rows[r];
			const string where = "event_tags.csv:" + to_string(table.lineNumbers[r]);
			TagRule rule{ row[c[0]], row[c[1]], row[c[2]], row[c[3]] };
			if (rule.prefix.empty())
			{
				error = where + " prefix is empty";
				return false;
			}
			if (IsKnownTag(rule.tag) == false)
			{
				error = where + " unknown tag '" + rule.tag + "' (connect move combat status item storage economy party escape cheat monster db system)";
				return false;
			}
			if (rule.sub.empty())
				rule.sub = rule.prefix;
			for (char& ch : rule.keyword)
				ch = static_cast<char>(::tolower(static_cast<unsigned char>(ch)));
			out.push_back(move(rule));
		}
		if (out.empty())
		{
			error = "event_tags.csv has no rules";
			return false;
		}
		return true;
	}

	namespace
	{
		bool IsDigit(char ch) { return ch >= '0' && ch <= '9'; }

		// 행위자 · 대상 키 - 로그가 쓰는 이름 그대로 (PROTOCOL.md). 나머지 키(bag= · item= · type= …)는 msg 에만 남는다
		const char* const ACTOR_KEYS[] = { "char", "caster", "attacker", "owner", "by", "killer", "player", "from", "leader", "inviter", "id", "instigator", "kicker", "self" };
		const char* const TARGET_KEYS[] = { "target", "victim", "to", "invitee", "member", "kicked", "hit", "other" };

		bool InList(const string& key, const char* const* list, size_t n)
		{
			for (size_t i = 0; i < n; i++)
				if (key == list[i])
					return true;
			return false;
		}

		void AddUnique(vector<uint64>& v, uint64 id)
		{
			if (id != 0 && find(v.begin(), v.end(), id) == v.end())
				v.push_back(id);
		}
	}

	Parsed ParseLogLine(const string& text, const vector<TagRule>& rules)
	{
		Parsed e;
		string line = text.substr(0, text.find('\n'));
		if (line.empty() == false && line.back() == '\r')
			line.pop_back();

		// 머리 "2026-10-21 20:01:40.313 I 08 " (AsyncLogger LogLine::WritePrefix) - 23 + 1 + 1 + 1 + 2 + 1
		if (line.size() >= 29 && line[4] == '-' && line[10] == ' ' && line[19] == '.' && line[23] == ' ' && line[25] == ' ' && IsDigit(line[26]) && line[28] == ' ')
		{
			e.ts = line.substr(0, 23);
			e.level = line[24];
			e.thread = (line[26] - '0') * 10 + (line[27] - '0');
			line = line.substr(29);
		}
		e.msg = line;

		// 줄머리 [XXX]
		if (line.size() > 2 && line[0] == '[')
		{
			const size_t close = line.find(']');
			if (close != string::npos && close > 1 && close < 32)
				e.prefix = line.substr(1, close - 1);
		}

		if (e.prefix.empty() == false)
		{
			string lower = line;
			for (char& ch : lower)
				ch = static_cast<char>(::tolower(static_cast<unsigned char>(ch)));
			bool matched = false;
			for (const TagRule& rule : rules)
			{
				if (rule.prefix != e.prefix)
					continue;
				if (rule.keyword.empty() == false && lower.find(rule.keyword) == string::npos)
					continue;
				e.tag = rule.tag;
				e.sub = rule.sub;
				matched = true;
				break;
			}
			if (matched == false)
			{
				e.tag = "system";
				e.sub = e.prefix;
				for (char& ch : e.sub)
					ch = (ch == ' ') ? '_' : static_cast<char>(::tolower(static_cast<unsigned char>(ch)));
			}
		}

		// key=value - 값이 숫자면 id, room= 은 글자 그대로
		for (size_t i = 0; i < line.size(); i++)
		{
			if (line[i] != '=' || i == 0)
				continue;
			size_t k = i;
			while (k > 0 && (::isalpha(static_cast<unsigned char>(line[k - 1])) || line[k - 1] == '_'))
				k--;
			if (k == i)
				continue;
			const string key = line.substr(k, i - k);
			const size_t v = i + 1;
			size_t end = v;
			while (end < line.size() && line[end] != ' ' && line[end] != ',' && line[end] != ')' && line[end] != '(')
				end++;
			const string value = line.substr(v, end - v);
			if (key == "room")
			{
				if (e.room.empty())
					e.room = value;
				continue;
			}
			if (value.empty() || IsDigit(value[0]) == false)
				continue;
			bool digits = true;
			for (char ch : value)
				digits = digits && IsDigit(ch);
			if (digits == false || value.size() > 19)
				continue;
			const uint64 id = ::_strtoui64(value.c_str(), nullptr, 10);
			if (key == "account" || key == "acc")
				e.account = (e.account == 0) ? id : e.account;
			else if (InList(key, ACTOR_KEYS, size(ACTOR_KEYS)))
				AddUnique(e.actors, id);
			else if (InList(key, TARGET_KEYS, size(TARGET_KEYS)))
				AddUnique(e.targets, id);
		}
		return e;
	}

	string JsonEscape(const string& s)
	{
		string out;
		out.reserve(s.size() + 8);
		for (const char ch : s)
		{
			switch (ch)
			{
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (static_cast<unsigned char>(ch) < 0x20)
				{
					char buf[8];
					::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(ch));
					out += buf;
				}
				else
					out += ch;		// UTF-8 바이트는 그대로 (JSON 은 UTF-8)
			}
		}
		return out;
	}

	namespace
	{
		string Ids(const vector<uint64>& ids)
		{
			string s = "[";
			for (size_t i = 0; i < ids.size(); i++)
				s += (i ? "," : "") + to_string(ids[i]);
			return s + "]";
		}
	}

	string ToJson(const Parsed& e, uint64 seq, const char* src, const string& data)
	{
		string s;
		s.reserve(e.msg.size() + 160);
		s += "{\"t\":\"ev\",\"seq\":" + to_string(seq);
		s += ",\"ts\":\"" + JsonEscape(e.ts) + "\"";
		s += ",\"lv\":\"";
		s += e.level;
		s += "\",\"th\":" + to_string(e.thread);
		s += ",\"tag\":\"" + JsonEscape(e.tag) + "\",\"sub\":\"" + JsonEscape(e.sub) + "\"";
		s += ",\"a\":" + Ids(e.actors) + ",\"tg\":" + Ids(e.targets);
		s += ",\"acc\":" + to_string(e.account);
		s += ",\"room\":\"" + JsonEscape(e.room) + "\"";
		s += ",\"src\":\"";
		s += src;
		s += "\"";
		if (data.empty() == false)
			s += ",\"d\":" + data;
		s += ",\"msg\":\"" + JsonEscape(e.msg) + "\"}";
		return s;
	}
}

/*-------- AdminJson --------*/

namespace AdminJson
{
	namespace
	{
		size_t FindValue(const string& json, const char* key)
		{
			const string needle = string("\"") + key + "\"";
			size_t at = json.find(needle);
			while (at != string::npos)
			{
				size_t p = at + needle.size();
				while (p < json.size() && (json[p] == ' ' || json[p] == '\t'))
					p++;
				if (p < json.size() && json[p] == ':')
				{
					p++;
					while (p < json.size() && (json[p] == ' ' || json[p] == '\t'))
						p++;
					return p;
				}
				at = json.find(needle, at + 1);
			}
			return string::npos;
		}

		void AppendUtf8(string& out, uint32 cp)
		{
			if (cp < 0x80) out += static_cast<char>(cp);
			else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
			else { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
		}
	}

	string GetString(const string& json, const char* key)
	{
		size_t p = FindValue(json, key);
		if (p == string::npos || p >= json.size() || json[p] != '"')
			return string();
		string out;
		for (p = p + 1; p < json.size() && json[p] != '"'; p++)
		{
			if (json[p] != '\\' || p + 1 >= json.size())
			{
				out += json[p];
				continue;
			}
			const char esc = json[++p];
			switch (esc)
			{
			case 'n': out += '\n'; break;
			case 't': out += '\t'; break;
			case 'r': out += '\r'; break;
			case 'u':
				if (p + 4 < json.size())
				{
					AppendUtf8(out, static_cast<uint32>(::strtoul(json.substr(p + 1, 4).c_str(), nullptr, 16)));
					p += 4;
				}
				break;
			default: out += esc; break;		// \" \\ \/
			}
		}
		return out;
	}

	int64 GetInt(const string& json, const char* key, int64 def)
	{
		size_t p = FindValue(json, key);
		if (p == string::npos || p >= json.size())
			return def;
		if (json[p] == '"')
			p++;		// "id":"128" 도 받는다
		if (p >= json.size() || (json[p] != '-' && (json[p] < '0' || json[p] > '9')))
			return def;
		return ::_strtoi64(json.c_str() + p, nullptr, 10);
	}
}

/*-------- EventLog --------*/

namespace
{
	string NowStamp()
	{
		SYSTEMTIME st;
		::GetLocalTime(&st);
		char buf[32];
		::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
		return buf;
	}
}

bool EventLog::Init(const string& rulesCsv, const string& dir, OUT string& error)
{
	if (EventFormat::ParseRules(rulesCsv, OUT _rules, OUT error) == false)
		return false;

	if (dir.empty() == false)
	{
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		SYSTEMTIME st;
		::GetLocalTime(&st);
		char name[64];
		::snprintf(name, sizeof(name), "events-%04d%02d%02d-%02d%02d%02d.jsonl", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
		_path = dir + "\\" + name;
		// 쓰기만 막고 읽기는 연다 - ServerMonitor 의 "지난 로그 열기" · 검증 툴이 도는 중인 서버의 파일을 읽는다 (fopen_s 는 읽기도 막았다)
		_file = ::_fsopen(_path.c_str(), "ab", _SH_DENYWR);
		if (_file == nullptr)
		{
			error = "cannot open " + _path;
			return false;
		}
	}

	// 전용 쓰기 스레드 - 파일 · 고리 버퍼 · 구독자. 큐가 차면 줄을 버리고 개수만 남긴다 (AsyncLogger 의 대가 그대로)
	_writer = new AsyncLogger([this](const std::string& text, bool) {
		size_t start = 0;
		while (start < text.size())
		{
			size_t end = text.find('\n', start);
			if (end == string::npos)
				end = text.size();
			if (end > start)
				Write(text.substr(start, end - start));
			start = end + 1;
		}
	});
	_enabled = true;
	return true;
}

void EventLog::Write(const string& line)
{
	if (_file != nullptr)
	{
		::fwrite(line.data(), 1, line.size(), _file);
		::fputc('\n', _file);
		const uint64 tick = ::GetTickCount64();
		if (++_unflushed >= 200 || tick - _lastFlushTick >= 1000)
		{
			::fflush(_file);
			_unflushed = 0;
			_lastFlushTick = tick;
		}
	}
	Subscriber subscriber;
	{
		std::lock_guard<std::mutex> guard(_ringLock);
		_ring.push_back(line);
		if (_ring.size() > RING_CAPACITY)
			_ring.pop_front();
		subscriber = _subscriber;
	}
	if (subscriber)
		subscriber(line);
}

void EventLog::FromLogLine(const string& text, bool toStderr)
{
	if (_enabled == false || text.empty())
		return;
	EventFormat::Parsed e = EventFormat::ParseLogLine(text, _rules);
	if (toStderr)
		e.level = 'E';
	if (e.prefix == "STATS" || (e.prefix.empty() && e.msg.find("=====") != string::npos))
		return;		// 10 초 통계 덩어리 - 관리 포트는 metrics 로 따로 받는다
	_writer->Push(EventFormat::ToJson(e, _seq.fetch_add(1) + 1, "log") + "\n");
}

void EventLog::Emit(const char* tag, const char* sub, vector<uint64> actors, vector<uint64> targets, const string& room, const string& msg, const string& data)
{
	if (_enabled == false)
		return;
	EventFormat::Parsed e;
	e.ts = NowStamp();
	e.thread = static_cast<int32>(LThreadId);
	e.tag = tag;
	e.sub = sub;
	e.actors = move(actors);
	e.targets = move(targets);
	e.room = room;
	e.msg = msg;
	_writer->Push(EventFormat::ToJson(e, _seq.fetch_add(1) + 1, "ev", data) + "\n");
}

void EventLog::SetSubscriber(Subscriber subscriber)
{
	std::lock_guard<std::mutex> guard(_ringLock);
	_subscriber = move(subscriber);
}

vector<string> EventLog::Recent(size_t count)
{
	std::lock_guard<std::mutex> guard(_ringLock);
	const size_t n = min(count, _ring.size());
	return vector<string>(_ring.end() - static_cast<ptrdiff_t>(n), _ring.end());
}

void EventLog::Flush()
{
	if (_writer != nullptr)
		_writer->Flush();
	if (_file != nullptr)
		::fflush(_file);
}
