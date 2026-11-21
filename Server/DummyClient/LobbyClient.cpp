#include "pch.h"
#include "LobbyClient.h"
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

namespace LobbyClient
{
	namespace
	{
		wstring Widen(const string& s)
		{
			wstring w;
			for (char c : s)
				w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
			return w;
		}

		// "key":"value" 또는 "key":123 - 응답이 작고 모양이 정해져 있어 JSON 라이브러리를 들이지 않는다
		string Field(const string& json, const string& key)
		{
			const string needle = "\"" + key + "\":";
			size_t p = json.find(needle);
			if (p == string::npos)
				return string();
			p += needle.size();
			while (p < json.size() && json[p] == ' ')
				p++;
			if (p < json.size() && json[p] == '"')
			{
				const size_t end = json.find('"', p + 1);
				return end == string::npos ? string() : json.substr(p + 1, end - p - 1);
			}
			size_t end = p;
			while (end < json.size() && (isdigit(static_cast<unsigned char>(json[end])) || json[end] == '-'))
				end++;
			return json.substr(p, end - p);
		}
	}

	bool Assign(const string& baseUrl, int32 count, OUT Assignment& out, OUT string& error)
	{
		const wstring url = Widen(baseUrl + "/lobby/assign?count=" + to_string(count));
		URL_COMPONENTS parts = {};
		parts.dwStructSize = sizeof(parts);
		wchar_t host[128] = {};
		wchar_t path[512] = {};
		parts.lpszHostName = host;
		parts.dwHostNameLength = _countof(host);
		parts.lpszUrlPath = path;
		parts.dwUrlPathLength = _countof(path);
		wchar_t extra[256] = {};
		parts.lpszExtraInfo = extra;
		parts.dwExtraInfoLength = _countof(extra);
		if (::WinHttpCrackUrl(url.c_str(), 0, 0, &parts) == FALSE)
		{
			error = "bad url";
			return false;
		}

		HINTERNET session = ::WinHttpOpen(L"FbtkDummyClient/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		HINTERNET connect = session ? ::WinHttpConnect(session, host, parts.nPort, 0) : nullptr;
		const wstring fullPath = wstring(path) + extra;
		HINTERNET request = connect ? ::WinHttpOpenRequest(connect, L"GET", fullPath.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr;

		string body;
		DWORD status = 0;
		if (request && ::WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && ::WinHttpReceiveResponse(request, nullptr))
		{
			DWORD size = sizeof(status);
			::WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
			DWORD available = 0;
			while (::WinHttpQueryDataAvailable(request, &available) && available > 0)
			{
				string chunk(available, '\0');
				DWORD read = 0;
				if (::WinHttpReadData(request, chunk.data(), available, &read) == FALSE)
					break;
				body.append(chunk.data(), read);
			}
		}
		else
		{
			error = "request failed err=" + to_string(::GetLastError());
		}

		if (request) ::WinHttpCloseHandle(request);
		if (connect) ::WinHttpCloseHandle(connect);
		if (session) ::WinHttpCloseHandle(session);

		if (status != 200)
		{
			if (error.empty())
				error = "http " + to_string(status) + " " + body.substr(0, 120);
			return false;
		}

		out.host = Field(body, "host");
		out.port = static_cast<uint16>(::atoi(Field(body, "port").c_str()));
		out.serverId = static_cast<uint32>(::atoi(Field(body, "server_id").c_str()));
		if (out.host.empty() || out.port == 0)
		{
			error = "bad body " + body.substr(0, 120);
			return false;
		}
		return true;
	}
}
