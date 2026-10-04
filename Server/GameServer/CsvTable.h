#pragma once

/*---------------
	CsvTable
---------------*/

// Server/Data/*.csv 공용 읽기. 쉼표 구분 · 따옴표 없음.
// 빈 줄은 건너뛰고 줄 끝 \r 과 첫 줄의 UTF-8 BOM 은 떼어 낸다.
struct CsvTable
{
	unordered_map<string, int32>	columns;		// 헤더 이름 -> 열 번호
	vector<vector<string>>			rows;
	vector<int32>					lineNumbers;	// 오류 메시지용 원본 줄 번호
};

namespace Csv
{
	bool	Parse(const string& text, const char* fileName, OUT CsvTable& out, OUT string& error);

	// 열은 이름으로 찾는다 - 열 순서가 바뀌어도 읽히고, 이름이 바뀌면 여기서 멈춘다.
	bool	FindColumns(const CsvTable& table, const char* fileName, const vector<const char*>& names, OUT vector<int32>& out, OUT string& error);

	bool	ParseUInt(const string& s, OUT uint32& out);

	// 파일 전체를 바이트 그대로 읽는다. 열 수 없으면 false
	bool	ReadFile(const string& path, OUT string& text);
}
