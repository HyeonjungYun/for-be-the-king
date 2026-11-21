#include "pch.h"
#include "SamplingProfiler.h"
#include <tlhelp32.h>
#include <dbghelp.h>
#include <fstream>
#include <filesystem>

#pragma comment(lib, "dbghelp.lib")

namespace SamplingProfiler
{
	namespace
	{
		constexpr int32		MAX_FRAMES = 32;
		constexpr uint64	DUMP_EVERY_US = 30'000'000;

		atomic<bool>		GRunning = false;
		thread				GThread;

		struct ThreadSlot
		{
			HANDLE	handle = nullptr;
			DWORD	id = 0;
			ULONG64	lastCycles = 0;
		};

		// 멈춘 스레드의 스택을 푼다 - 힙 · 락 없이. 프레임 수를 돌려준다
		int32 Unwind(CONTEXT& ctx, DWORD64* frames)
		{
			int32 n = 0;
			while (n < MAX_FRAMES && ctx.Rip != 0)
			{
				frames[n++] = ctx.Rip;
				DWORD64 imageBase = 0;
				PRUNTIME_FUNCTION fn = ::RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
				if (fn == nullptr)
				{
					// 잎 함수 - 복귀 주소가 스택 꼭대기에 있다
					if (ctx.Rsp == 0)
						break;
					ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
					ctx.Rsp += 8;
					continue;
				}
				PVOID handlerData = nullptr;
				DWORD64 establisher = 0;
				::RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
			}
			return n;
		}

		string SymbolOf(HANDLE process, DWORD64 addr)
		{
			alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
			SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(buffer);
			sym->SizeOfStruct = sizeof(SYMBOL_INFO);
			sym->MaxNameLen = 511;
			DWORD64 disp = 0;
			if (::SymFromAddr(process, addr, &disp, sym))
				return string(sym->Name, sym->NameLen);

			IMAGEHLP_MODULE64 mod = {};
			mod.SizeOfStruct = sizeof(mod);
			if (::SymGetModuleInfo64(process, addr, &mod))
				return string(mod.ModuleName) + "!?";
			return "?";
		}

		void Dump(const string& path, const unordered_map<DWORD64, uint64>& self, const unordered_map<DWORD64, uint64>& total,
			uint64 samples, uint64 idleSkipped, double windowSec)
		{
			HANDLE process = ::GetCurrentProcess();
			unordered_map<string, uint64> selfByName;
			unordered_map<string, uint64> totalByName;
			unordered_map<DWORD64, string> cache;
			auto name = [&](DWORD64 a) -> const string&
			{
				auto it = cache.find(a);
				if (it == cache.end())
					it = cache.emplace(a, SymbolOf(process, a)).first;
				return it->second;
			};
			for (const auto& [a, n] : self)
				selfByName[name(a)] += n;
			for (const auto& [a, n] : total)
				totalByName[name(a)] += n;

			auto top = [](const unordered_map<string, uint64>& m, size_t k)
			{
				vector<pair<uint64, string>> v;
				for (const auto& [s, n] : m)
					v.emplace_back(n, s);
				sort(v.begin(), v.end(), greater<>());
				if (v.size() > k)
					v.resize(k);
				return v;
			};

			ofstream out(path, ios::app);
			SYSTEMTIME st;
			::GetLocalTime(&st);
			char head[160];
			::snprintf(head, sizeof(head), "\n===== PROFILE %02d:%02d:%02d window %.1fs busy samples %llu (idle skipped %llu) =====\n",
				st.wHour, st.wMinute, st.wSecond, windowSec, samples, idleSkipped);
			out << head;
			out << "-- self (top frame) --\n";
			for (const auto& [n, s] : top(selfByName, 45))
			{
				char line[600];
				::snprintf(line, sizeof(line), "%6.2f%%  %7llu  %s\n", samples ? n * 100.0 / samples : 0.0, n, s.c_str());
				out << line;
			}
			out << "-- total (anywhere on the stack) --\n";
			for (const auto& [n, s] : top(totalByName, 60))
			{
				char line[600];
				::snprintf(line, sizeof(line), "%6.2f%%  %7llu  %s\n", samples ? n * 100.0 / samples : 0.0, n, s.c_str());
				out << line;
			}
		}

		void Run(string path, uint32 intervalMs)
		{
			HANDLE process = ::GetCurrentProcess();
			::SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
			::SymInitialize(process, nullptr, TRUE);

			const DWORD selfId = ::GetCurrentThreadId();
			const DWORD pid = ::GetCurrentProcessId();
			vector<ThreadSlot> threads;
			uint64 lastScanUs = 0;

			unordered_map<DWORD64, uint64> self;
			unordered_map<DWORD64, uint64> total;
			uint64 samples = 0;
			uint64 idleSkipped = 0;
			uint64 windowStartUs = Utils::NowMicroseconds();
			DWORD64 frames[MAX_FRAMES];

			while (GRunning.load())
			{
				this_thread::sleep_for(chrono::milliseconds(intervalMs));
				const uint64 nowUs = Utils::NowMicroseconds();

				// 스레드 목록 - 2 초마다 새로 (워커는 부팅 때 생기고, DB 스레드 · 로거도 같이 본다)
				if (nowUs - lastScanUs > 2'000'000)
				{
					lastScanUs = nowUs;
					unordered_map<DWORD, ULONG64> keep;
					for (const ThreadSlot& t : threads)
						keep[t.id] = t.lastCycles;
					for (ThreadSlot& t : threads)
						::CloseHandle(t.handle);
					threads.clear();

					HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
					THREADENTRY32 te = {};
					te.dwSize = sizeof(te);
					if (snap != INVALID_HANDLE_VALUE && ::Thread32First(snap, &te))
					{
						do
						{
							if (te.th32OwnerProcessID != pid || te.th32ThreadID == selfId)
								continue;
							HANDLE h = ::OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
							if (h == nullptr)
								continue;
							ThreadSlot slot;
							slot.handle = h;
							slot.id = te.th32ThreadID;
							auto it = keep.find(te.th32ThreadID);
							slot.lastCycles = (it != keep.end()) ? it->second : 0;
							threads.push_back(slot);
						} while (::Thread32Next(snap, &te));
					}
					if (snap != INVALID_HANDLE_VALUE)
						::CloseHandle(snap);
				}

				for (ThreadSlot& t : threads)
				{
					ULONG64 cycles = 0;
					::QueryThreadCycleTime(t.handle, &cycles);
					const bool busy = (t.lastCycles != 0 && cycles > t.lastCycles + 20'000);	// 대기 중이던 스레드는 거의 늘지 않는다
					t.lastCycles = cycles;
					if (busy == false)
					{
						idleSkipped++;
						continue;
					}

					if (::SuspendThread(t.handle) == static_cast<DWORD>(-1))
						continue;
					CONTEXT ctx = {};
					ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
					int32 n = 0;
					if (::GetThreadContext(t.handle, &ctx))
						n = Unwind(ctx, frames);
					::ResumeThread(t.handle);

					if (n == 0)
						continue;
					samples++;
					self[frames[0]]++;
					// 같은 스택에 같은 주소가 두 번(재귀)이면 한 번만 - 함수 단위 합산은 덤프에서
					for (int32 i = 0; i < n; i++)
					{
						bool seen = false;
						for (int32 j = 0; j < i && seen == false; j++)
							seen = (frames[j] == frames[i]);
						if (seen == false)
							total[frames[i]]++;
					}
				}

				if (nowUs - windowStartUs >= DUMP_EVERY_US)
				{
					Dump(path, self, total, samples, idleSkipped, (nowUs - windowStartUs) / 1e6);
					self.clear();
					total.clear();
					samples = 0;
					idleSkipped = 0;
					windowStartUs = nowUs;
				}
			}

			for (ThreadSlot& t : threads)
				::CloseHandle(t.handle);
			::SymCleanup(process);
		}
	}

	bool Start(const string& outDir, uint32 intervalMs)
	{
		if (GRunning.exchange(true))
			return false;
		std::error_code ec;
		filesystem::create_directories(outDir, ec);
		SYSTEMTIME st;
		::GetLocalTime(&st);
		char file[80];
		::snprintf(file, sizeof(file), "\\profile-%04d%02d%02d-%02d%02d%02d-%lu.txt", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, ::GetCurrentProcessId());
		GThread = thread(Run, outDir + file, (std::max)(1u, intervalMs));
		return true;
	}

	void Stop()
	{
		if (GRunning.exchange(false) && GThread.joinable())
			GThread.join();
	}
}
