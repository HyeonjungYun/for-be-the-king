#pragma once

/*--------------------
	SamplingProfiler
--------------------*/

// 프로세스 안 샘플링 프로파일러 (2026-11-21 동접 1만 - docs/server-scaling-10k.md § 2).
// 관리자 권한 없이(wpr · xperf 는 커널 로거라 관리자가 필요하다) 서버 CPU 가 어디에 쓰이는지 본다.
//
//   FBTK_PROFILE=1           켠다 (기본 끔 - 끄면 스레드도 만들지 않는다)
//   FBTK_PROFILE_MS=2        샘플 간격 (ms)
//   결과: <metricsDir>\profile-<boot>.txt 에 30 초마다 그 구간의 상위 함수 (self = 맨 위 프레임, total = 스택 어디든)
//
// 방법: 간격마다 이 프로세스의 다른 스레드를 하나씩 SuspendThread → 레지스터 · 스택을 RtlVirtualUnwind 로 풀어 주소만 복사 → Resume.
// 멈춘 동안에는 힙을 쓰지 않는다 (멈춘 스레드가 힙 락을 쥐고 있을 수 있다). 지난 샘플 뒤로 CPU 사이클이 늘지 않은 스레드(대기 중)는 세지 않는다 -
// 그래서 결과는 "CPU 를 쓴 곳" 이다. 주소 → 함수 이름은 덤프할 때 DbgHelp + PDB 로.
namespace SamplingProfiler
{
	bool	Start(const string& outDir, uint32 intervalMs);
	void	Stop();
}
