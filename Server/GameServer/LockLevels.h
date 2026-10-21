#pragma once

// 매니저 락 수준 (ServerCore LockOrder.h · WRITE_LOCK_ORDERED). 먼저 잡는 쪽이 작다 - PartyManager.h 의 규약을 숫자로 (2026-10-21).
//   PartyManager 는 락 안에서 등록부들(세션 관리자 · 인스턴스 · 사망 커밋 · 끊긴 몸)을 부른다 → 등록부는 파티 락을 잡지 않는다.
//   Room 은 락이 없다 (방 큐가 직렬이다, ADR-0007). Session · Listener · Service 의 락은 잎 락이라 수준이 없다
namespace LockLevel
{
	constexpr int32 PARTY = 10;			// PartyManager
	constexpr int32 REGISTRY = 20;		// GameSessionManager · InstanceManager · AccountManager · DeathCommitTracker · DetachedBodies
}
