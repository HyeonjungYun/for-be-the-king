#include "pch.h"
#include "CsvTable.h"
#include <charconv>
#include <fstream>
#include <sstream>

namespace
{
	vector<string> SplitLine(const string& line)
	{
		vector<string> cells;
		size_t begin = 0;

		while (true)
		{
			const size_t comma = line.find(',', begin);
			cells.push_back(line.substr(begin, (comma == string::npos) ? string::npos : comma - begin));

			if (comma == string::npos)
				break;

			begin = comma + 1;
		}

		return cells;
	}
}

bool Csv::Parse(const string& text, const char* fileName, OUT CsvTable& out, OUT string& error)
{
	istringstream in(text);
	string line;
	int32 lineNo = 0;
	bool header = true;

	while (getline(in, line))
	{
		lineNo++;

		if (lineNo == 1 && line.compare(0, 3, "\xEF\xBB\xBF") == 0)
			line.erase(0, 3);

		if (line.empty() == false && line.back() == '\r')
			line.pop_back();

		if (line.empty())
			continue;

		vector<string> cells = SplitLine(line);

		if (header)
		{
			for (int32 i = 0; i < static_cast<int32>(cells.size()); i++)
				out.columns[cells[i]] = i;

			header = false;
			continue;
		}

		if (cells.size() != out.columns.size())
		{
			error = string(fileName) + ":" + to_string(lineNo) + " has " + to_string(cells.size())
				+ " columns, header has " + to_string(out.columns.size());
			return false;
		}

		out.rows.push_back(move(cells));
		out.lineNumbers.push_back(lineNo);
	}

	if (header)
	{
		error = string(fileName) + " is empty";
		return false;
	}

	return true;
}

bool Csv::FindColumns(const CsvTable& table, const char* fileName, const vector<const char*>& names, OUT vector<int32>& out, OUT string& error)
{
	out.clear();

	for (const char* name : names)
	{
		auto findIt = table.columns.find(name);
		if (findIt == table.columns.end())
		{
			error = string(fileName) + " has no column '" + name + "'";
			return false;
		}

		out.push_back(findIt->second);
	}

	return true;
}

bool Csv::ParseUInt(const string& s, OUT uint32& out)
{
	const char* first = s.data();
	const char* last = s.data() + s.size();

	auto [ptr, ec] = from_chars(first, last, out);
	return ec == errc() && ptr == last;
}

bool Csv::ReadFile(const string& path, OUT string& text)
{
	ifstream file(path, ios::binary);
	if (file.is_open() == false)
		return false;

	ostringstream ss;
	ss << file.rdbuf();
	text = ss.str();
	return true;
}
