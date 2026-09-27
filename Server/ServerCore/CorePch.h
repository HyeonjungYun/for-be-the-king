#pragma once

#include "Types.h"
#include "CoreMacro.h"
#include "CoreTLS.h"
#include "CoreGlobal.h"

#include <array>
#include <vector>
#include <list>
#include <queue>
#include <stack>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <chrono>

#include <windows.h>
#include <iostream>
#include <syncstream>
#include <assert.h>
using namespace std;

// 로그 출력 — 한 문장이 통째로 원자적으로 나간다.
//
// 🔴 cout 을 직접 쓰지 말 것. 여러 스레드가 동시에 찍으면 토큰 중간에서 섞여
//    집계가 깨진다. 2026-09-20 30분 소크에서 이것 때문에 게이트 판정이 2건
//    허위로 났다 (max=6491863860us · reject=2290). 상세: roadmap.md § 0-10 4차
//
// 🔴 wcout 도 쓰지 말 것. 같은 stdout 에 좁은/넓은 스트림을 섞는 것은 미정의다.
//
// 한 이벤트를 여러 문장으로 나눠 찍어야 하면 매크로 대신 객체를 직접 둔다.
//    std::osyncstream out(std::cout);   // 소멸할 때 블록 전체가 한 번에 나간다
//    out << ... ;
#define LOG_LINE	std::osyncstream(std::cout)

#include <winsock2.h>
#include <mswsock.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include "SendBuffer.h"
#include "Session.h"
#include "JobQueue.h"


