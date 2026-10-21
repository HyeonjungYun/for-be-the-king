#include "pch.h"
#include "CrashDump.h"
#include <DbgHelp.h>
#include <filesystem>
#include <exception>

#pragma comment(lib, "dbghelp.lib")

/*--------------
	CrashDump
---------------*/

namespace
{
	string				s_dir;
	string				s_prefix;
	atomic<bool>		s_installed{ false };
	atomic<bool>		s_writing{ false };

	// 처리되지 않은 예외 · terminate · 잘못된 인자가 전부 이 번호의 SEH 예외로 모인다 (덤프 파일 이름에 code 로 남는다)
	constexpr DWORD CODE_TERMINATE = 0xE0000001;
	constexpr DWORD CODE_INVALID_PARAMETER = 0xE0000002;
	constexpr DWORD CODE_PURE_CALL = 0xE0000003;
	constexpr DWORD CODE_MANUAL = 0xE0000004;

	string DumpPath(DWORD code)
	{
		SYSTEMTIME st;
		::GetLocalTime(&st);

		char name[MAX_PATH];
		::snprintf(name, sizeof(name), "%s\\%s-%04d%02d%02d-%02d%02d%02d-%lu-%08lX.dmp", s_dir.c_str(), s_prefix.c_str(),
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, ::GetCurrentProcessId(), code);
		return name;
	}

	string WriteDump(EXCEPTION_POINTERS* info, DWORD code)
	{
		const string path = DumpPath(code);

		HANDLE file = ::CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			::fprintf(stderr, "[CRASH] code=0x%08lX - cannot create %s (err=%lu)\n", code, path.c_str(), ::GetLastError());
			::fflush(stderr);
			return string();
		}

		MINIDUMP_EXCEPTION_INFORMATION exceptionInfo;
		exceptionInfo.ThreadId = ::GetCurrentThreadId();
		exceptionInfo.ExceptionPointers = info;
		exceptionInfo.ClientPointers = FALSE;

		// 간접 참조 메모리까지 - 스택이 가리키는 힙 객체(세션 · 방 · 패킷)를 디버거에서 펼쳐 볼 수 있다. 전체 메모리는 아니다 (수백 MB)
		const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
			MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo | MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithUnloadedModules);

		const BOOL ok = ::MiniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(), file, type,
			(info != nullptr) ? &exceptionInfo : nullptr, nullptr, nullptr);
		::CloseHandle(file);

		::fprintf(stderr, "[CRASH] code=0x%08lX thread=%lu dump=%s %s\n", code, ::GetCurrentThreadId(), path.c_str(), ok ? "written" : "FAILED");
		::fflush(stderr);
		return ok ? path : string();
	}

	LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* info)
	{
		// 두 스레드가 같이 죽으면 첫 스레드만 쓴다 - 둘째는 첫째가 끝내도록 기다린다 (프로세스는 첫째가 끝낸다)
		if (s_writing.exchange(true))
		{
			::Sleep(10'000);
			return EXCEPTION_EXECUTE_HANDLER;
		}

		const DWORD code = (info != nullptr && info->ExceptionRecord != nullptr) ? info->ExceptionRecord->ExceptionCode : 0;
		WriteDump(info, code);
		return EXCEPTION_EXECUTE_HANDLER;
	}

	// CRT 경로는 SEH 예외로 바꿔 위 필터로 보낸다 - 그 자리의 스택이 덤프에 남는다
	void OnTerminate()
	{
		::RaiseException(CODE_TERMINATE, EXCEPTION_NONCONTINUABLE, 0, nullptr);
	}

	void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t)
	{
		::RaiseException(CODE_INVALID_PARAMETER, EXCEPTION_NONCONTINUABLE, 0, nullptr);
	}

	void OnPureCall()
	{
		::RaiseException(CODE_PURE_CALL, EXCEPTION_NONCONTINUABLE, 0, nullptr);
	}
}

void CrashDump::Install(const string& dir, const char* prefix)
{
	if (s_installed.exchange(true))
		return;

	s_dir = dir;
	s_prefix = (prefix != nullptr) ? prefix : "crash";

	error_code ec;
	filesystem::create_directories(s_dir, ec);

	// 대화상자 없이 - 무인 서버 · CI 가 "프로그램이 작동을 멈췄습니다" 창에 멈추지 않게
	::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
	::_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

	::SetUnhandledExceptionFilter(OnUnhandledException);
	std::set_terminate(OnTerminate);
	::_set_invalid_parameter_handler(OnInvalidParameter);
	::_set_purecall_handler(OnPureCall);
}

const string& CrashDump::Dir()
{
	return s_dir;
}

string CrashDump::WriteNow(const char* reason)
{
	if (s_installed.load() == false)
		return string();

	::fprintf(stderr, "[CRASH] manual dump - %s\n", (reason != nullptr) ? reason : "");
	return WriteDump(nullptr, CODE_MANUAL);
}
