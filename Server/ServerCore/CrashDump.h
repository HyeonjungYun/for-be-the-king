#pragma once

/*--------------
	CrashDump
---------------*/

// 처리되지 않은 예외 · CRASH / ASSERT_CRASH 매크로 · std::terminate · CRT 잘못된 인자 때 미니덤프를 남긴다 (2026-10-21).
// 예전엔 어떤 크래시든 덤프 없이 프로세스가 사라져 원인을 로그 몇 줄로만 추정해야 했다.
//
//   CrashDump::Install("C:\\Server\\MMO\\Server\\Logs\\crash", "GameServer");
//   → <dir>\GameServer-20261021-140322-12345.dmp (MiniDumpWithIndirectlyReferencedMemory | ThreadInfo | DataSegs)
//
// 덤프를 쓴 뒤에는 그대로 죽는다 (EXCEPTION_EXECUTE_HANDLER). WER 대화상자는 띄우지 않는다 - 무인 서버가 대화상자에 멈추지 않게.
// 로거 큐에 남은 줄은 비우지 않는다 - 크래시 문맥에서 다른 스레드의 락을 기다리면 덤프조차 못 쓴다. 대신 [CRASH] 한 줄을 표준 오류에 직접 쓴다
class CrashDump
{
public:
	static void				Install(const string& dir, const char* prefix);
	static const string&	Dir();

	// 테스트 · 수동 확인용 - 지금 스택으로 덤프 하나를 쓴다 (죽지 않는다). 쓴 파일 경로, 실패면 빈 문자열
	static string			WriteNow(const char* reason);
};
