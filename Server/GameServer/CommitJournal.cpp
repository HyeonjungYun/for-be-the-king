#include "pch.h"
#include "CommitJournal.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <charconv>
#include <chrono>

namespace
{
	atomic<uint64> s_journalSeq = 0;

	namespace fs = std::filesystem;

	fs::path FilePath(const string& dir, const string& id)
	{
		return fs::path(dir) / (id + CommitJournal::EXTENSION);
	}

	vector<string> SplitComma(const string& s)
	{
		vector<string> parts;
		size_t start = 0;
		while (true)
		{
			const size_t comma = s.find(',', start);
			parts.push_back(s.substr(start, comma == string::npos ? string::npos : comma - start));
			if (comma == string::npos)
				break;
			start = comma + 1;
		}
		return parts;
	}

	bool ToU64(const string& s, OUT uint64& out)
	{
		auto [ptr, ec] = from_chars(s.data(), s.data() + s.size(), out);
		return ec == errc() && ptr == s.data() + s.size() && s.empty() == false;
	}

	bool ToU32(const string& s, OUT uint32& out)
	{
		uint64 v = 0;
		if (ToU64(s, v) == false || v > UINT32_MAX)
			return false;
		out = static_cast<uint32>(v);
		return true;
	}

	bool ToFloat(const string& s, OUT float& out)
	{
		auto [ptr, ec] = from_chars(s.data(), s.data() + s.size(), out);
		return ec == errc() && ptr == s.data() + s.size() && s.empty() == false;
	}

	// instanceId,typeId,grade,level,skillPrimary,skillSecondary,isNew,lonePath (lonePath 는 2026-10-17 부터 - 7 칸 저널도 읽는다)
	string FormatItem(const ItemEntry& e)
	{
		return to_string(e.instanceId) + "," + to_string(e.itemTypeId) + "," + to_string(static_cast<int32>(e.grade)) + ","
			+ to_string(e.level) + "," + to_string(e.skillIdPrimary) + "," + to_string(e.skillIdSecondary) + "," + (e.isNew ? "1" : "0")
			+ "," + (e.lonePath ? "1" : "0");
	}

	bool ParseItem(const vector<string>& f, size_t first, OUT ItemEntry& e)
	{
		if (f.size() != first + 7 && f.size() != first + 8)
			return false;

		uint32 grade = 0;
		uint64 isNew = 0;
		uint64 lone = 0;
		if (ToU64(f[first], e.instanceId) == false || ToU32(f[first + 1], e.itemTypeId) == false || ToU32(f[first + 2], grade) == false
			|| ToU32(f[first + 3], e.level) == false || ToU32(f[first + 4], e.skillIdPrimary) == false
			|| ToU32(f[first + 5], e.skillIdSecondary) == false || ToU64(f[first + 6], isNew) == false || isNew > 1)
			return false;
		if (f.size() == first + 8 && (ToU64(f[first + 7], lone) == false || lone > 1))
			return false;
		e.lonePath = (lone == 1);

		if (grade > Protocol::ITEM_GRADE_LEGENDARY)
			return false;

		e.grade = static_cast<Protocol::ItemGrade>(grade);
		e.isNew = (isNew == 1);
		return e.instanceId != 0;
	}

	string FloatText(float v)
	{
		char buf[32];
		auto [ptr, ec] = to_chars(buf, buf + sizeof(buf), v);		// 최단 왕복 표현 - 읽으면 같은 float
		return string(buf, ptr);
	}
}

const char* CommitJournal::KindName(JournalKind kind)
{
	switch (kind)
	{
	case JournalKind::Death:	return "DEATH";
	case JournalKind::Escape:	return "ESCAPE";
	case JournalKind::Expiry:	return "EXPIRE";
	default:					return "?";
	}
}

string CommitJournal::NewId(JournalKind kind, uint64 characterId)
{
	const uint64 unixMs = static_cast<uint64>(chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now().time_since_epoch()).count());
	return string(KindName(kind)) + "-" + to_string(characterId) + "-" + to_string(unixMs) + "-" + to_string(s_journalSeq.fetch_add(1));
}

string CommitJournal::Format(const JournalEntry& entry)
{
	ostringstream os;
	os << "v=" << VERSION << '\n';
	os << "id=" << entry.id << '\n';
	os << "kind=" << KindName(entry.kind) << '\n';
	os << "char=" << entry.characterId << '\n';

	switch (entry.kind)
	{
	case JournalKind::Death:
	{
		const DeathSnapshot& d = entry.death;
		os << "bag=" << d.bagId << '\n';
		os << "floor=" << d.floorId << '\n';
		os << "pos=" << FloatText(d.x) << "," << FloatText(d.y) << "," << FloatText(d.z) << '\n';
		for (const ItemEntry& e : d.items)
			os << "item=" << FormatItem(e) << '\n';
		for (uint64 id : d.droppedIds)
			os << "dropped=" << id << '\n';
		break;
	}
	case JournalKind::Escape:
	{
		const EscapeSnapshot& s = entry.escape;
		os << "gold=" << s.gold << '\n';
		for (const ItemEntry& e : s.carried)
			os << "carried=" << FormatItem(e) << '\n';
		for (int32 i = 0; i < SKILL_SLOT_COUNT; i++)
		{
			if (s.equipped[i].instanceId != 0)
				os << "equipped=" << i << "," << FormatItem(s.equipped[i]) << '\n';
		}
		for (uint64 id : s.droppedIds)
			os << "dropped=" << id << '\n';
		break;
	}
	case JournalKind::Expiry:
	{
		const ExpirySnapshot& x = entry.expiry;
		os << "gold_kept=" << x.goldKept << '\n';
		os << "destroyed=" << x.itemsDestroyed << '\n';
		for (uint64 id : x.rowIds)
			os << "row=" << id << '\n';
		break;
	}
	}

	os << "end=1\n";
	return os.str();
}

bool CommitJournal::Parse(const string& text, OUT JournalEntry& out, OUT string& error)
{
	out = JournalEntry{};

	bool hasVersion = false;
	bool hasKind = false;
	bool hasEnd = false;
	vector<uint64> dropped;

	istringstream in(text);
	string line;
	int32 lineNo = 0;

	while (getline(in, line))
	{
		lineNo++;
		if (line.empty() == false && line.back() == '\r')
			line.pop_back();
		if (line.empty())
			continue;

		if (hasEnd)
		{
			error = "line " + to_string(lineNo) + ": text after end";
			return false;
		}

		const size_t eq = line.find('=');
		if (eq == string::npos)
		{
			error = "line " + to_string(lineNo) + ": no '='";
			return false;
		}

		const string key = line.substr(0, eq);
		const string value = line.substr(eq + 1);
		const string where = "line " + to_string(lineNo) + " (" + key + ")";
		bool ok = true;

		if (key == "v")
		{
			uint32 v = 0;
			ok = ToU32(value, v) && v == VERSION;
			hasVersion = ok;
		}
		else if (key == "id")
		{
			out.id = value;
			ok = value.empty() == false;
		}
		else if (key == "kind")
		{
			if (value == "DEATH")		out.kind = JournalKind::Death;
			else if (value == "ESCAPE")	out.kind = JournalKind::Escape;
			else if (value == "EXPIRE")	out.kind = JournalKind::Expiry;
			else ok = false;
			hasKind = ok;
		}
		else if (key == "char")
		{
			ok = ToU64(value, out.characterId) && out.characterId != 0;
		}
		else if (key == "bag")			ok = ToU64(value, out.death.bagId);
		else if (key == "floor")		ok = ToU32(value, out.death.floorId);
		else if (key == "pos")
		{
			const vector<string> f = SplitComma(value);
			ok = f.size() == 3 && ToFloat(f[0], out.death.x) && ToFloat(f[1], out.death.y) && ToFloat(f[2], out.death.z);
		}
		else if (key == "item")
		{
			ItemEntry e;
			ok = ParseItem(SplitComma(value), 0, e);
			if (ok)
				out.death.items.push_back(e);
		}
		else if (key == "gold")			ok = ToU64(value, out.escape.gold);
		else if (key == "carried")
		{
			ItemEntry e;
			ok = ParseItem(SplitComma(value), 0, e);
			if (ok)
				out.escape.carried.push_back(e);
		}
		else if (key == "equipped")
		{
			const vector<string> f = SplitComma(value);
			uint32 slot = 0;
			ItemEntry e;
			ok = f.empty() == false && ToU32(f[0], slot) && slot < SKILL_SLOT_COUNT && ParseItem(f, 1, e)
				&& out.escape.equipped[slot].instanceId == 0;
			if (ok)
				out.escape.equipped[slot] = e;
		}
		else if (key == "dropped")
		{
			uint64 id = 0;
			ok = ToU64(value, id) && id != 0;
			if (ok)
				dropped.push_back(id);
		}
		else if (key == "gold_kept")	ok = ToU64(value, out.expiry.goldKept);
		else if (key == "destroyed")	ok = ToU32(value, out.expiry.itemsDestroyed);
		else if (key == "row")
		{
			uint64 id = 0;
			ok = ToU64(value, id) && id != 0;
			if (ok)
				out.expiry.rowIds.push_back(id);
		}
		else if (key == "end")
		{
			ok = (value == "1");
			hasEnd = ok;
		}
		else
		{
			error = where + ": unknown key";
			return false;
		}

		if (ok == false)
		{
			error = where + ": bad value '" + value + "'";
			return false;
		}
	}

	if (hasVersion == false || hasKind == false || out.id.empty() || out.characterId == 0)
	{
		error = "missing v / id / kind / char";
		return false;
	}

	// 쓰다 끊긴 파일 - 아이템 줄 일부만 있을 수 있다. 일부만 적용하면 잘못된 결과가 확정된다
	if (hasEnd == false)
	{
		error = "no end line (truncated)";
		return false;
	}

	out.death.characterId = out.escape.characterId = out.expiry.characterId = out.characterId;
	if (out.kind == JournalKind::Death)
		out.death.droppedIds = move(dropped);
	else if (out.kind == JournalKind::Escape)
		out.escape.droppedIds = move(dropped);
	else if (dropped.empty() == false)
	{
		error = "dropped lines in an EXPIRE record";
		return false;
	}

	if (out.kind == JournalKind::Death && out.death.bagId == 0)
	{
		error = "DEATH record without bag";
		return false;
	}

	return true;
}

bool CommitJournal::Write(const string& dir, const JournalEntry& entry, OUT string& error)
{
	std::error_code ec;
	fs::create_directories(dir, ec);
	if (ec)
	{
		error = "create_directories: " + ec.message();
		return false;
	}

	const fs::path finalPath = FilePath(dir, entry.id);
	fs::path tmpPath = finalPath;
	tmpPath += ".tmp";

	{
		ofstream out(tmpPath, ios::binary | ios::trunc);
		if (out.is_open() == false)
		{
			error = "cannot open " + tmpPath.string();
			return false;
		}

		out << Format(entry);
		out.flush();
		if (out.good() == false)
		{
			error = "write failed " + tmpPath.string();
			return false;
		}
	}

	fs::rename(tmpPath, finalPath, ec);
	if (ec)
	{
		error = "rename: " + ec.message();
		fs::remove(tmpPath, ec);
		return false;
	}

	return true;
}

bool CommitJournal::Read(const string& dir, const string& id, OUT JournalEntry& out, OUT string& error)
{
	ifstream in(FilePath(dir, id), ios::binary);
	if (in.is_open() == false)
	{
		error = "cannot open " + FilePath(dir, id).string();
		return false;
	}

	ostringstream text;
	text << in.rdbuf();
	return Parse(text.str(), OUT out, OUT error);
}

bool CommitJournal::Remove(const string& dir, const string& id)
{
	std::error_code ec;
	return fs::remove(FilePath(dir, id), ec) && !ec;
}

bool CommitJournal::MoveToAbandoned(const string& dir, const string& id)
{
	std::error_code ec;
	const fs::path abandonedDir = fs::path(dir) / ABANDONED_DIR;
	fs::create_directories(abandonedDir, ec);
	fs::rename(FilePath(dir, id), abandonedDir / (id + EXTENSION), ec);
	return !ec;
}

vector<string> CommitJournal::ListIds(const string& dir)
{
	vector<string> ids;
	std::error_code ec;

	if (fs::is_directory(dir, ec) == false)
		return ids;

	for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec))
	{
		if (entry.is_regular_file(ec) && entry.path().extension() == EXTENSION)
			ids.push_back(entry.path().stem().string());
	}

	sort(ids.begin(), ids.end());
	return ids;
}

void CommitJournal::MaxIds(const JournalEntry& entry, OUT uint64& maxInstanceId, OUT uint64& maxBagId)
{
	auto take = [&maxInstanceId](const ItemEntry& e) { maxInstanceId = max(maxInstanceId, e.instanceId); };

	switch (entry.kind)
	{
	case JournalKind::Death:
		maxBagId = max(maxBagId, entry.death.bagId);
		for (const ItemEntry& e : entry.death.items)
			take(e);
		break;
	case JournalKind::Escape:
		for (const ItemEntry& e : entry.escape.carried)
			take(e);
		for (const ItemEntry& e : entry.escape.equipped)
			take(e);
		break;
	case JournalKind::Expiry:
		break;
	}
}
