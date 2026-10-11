#pragma once

/*-----------------
	InstanceRules
-----------------*/

// PVE 인스턴스 · 팀 규칙 (ADR-0014 · design/quick-specs/pve-modes-2026-10-11.md). 순수 함수 - Room 이 부르고 GameServerTests 가 검사한다
namespace InstanceRules
{
	// 같은 팀 = 둘 다 0 이 아닌 같은 값. 0 = 팀 없음 - 모두에게 적대 (PvP · MVP 규칙 combat-system Rule 9).
	// PVE 협동은 입장 때 파티 id 를 팀으로 받고 그 런 동안 바뀌지 않는다 - 파티를 나가도 배신이 생기지 않는다
	inline bool Allied(uint64 teamA, uint64 teamB)
	{
		return teamA != 0 && teamA == teamB;
	}

	// 마지막 사람이 나간 뒤 이만큼 비어 있으면 닫는다 - 사망 퇴장 · 탈출 직후 합류 요청이 오는 틈
	constexpr uint64 EMPTY_GRACE_US = 3'000'000;

	enum class CloseVerdict : uint8
	{
		Keep,			// 사람이 있거나 사망 커밋이 남았다 - 그대로
		StartGrace,		// 방금 비었다 - 지금부터 잰다
		Close,			// 유예만큼 비어 있었다 - 닫는다 (입장 예약은 InstanceManager 가 따로 본다)
	};

	// players = 방에 남은 플레이어 몸 (살았든 죽었든 끊겼든), pendingDeathCommits = 끝나지 않은 사망 커밋 (가방이 아직 안 생겼다)
	inline CloseVerdict JudgeClose(int32 players, int32 pendingDeathCommits, uint64 emptySinceUs, uint64 nowUs, uint64 graceUs = EMPTY_GRACE_US)
	{
		if (players > 0 || pendingDeathCommits > 0)
			return CloseVerdict::Keep;
		if (emptySinceUs == 0)
			return CloseVerdict::StartGrace;
		return (nowUs >= emptySinceUs + graceUs) ? CloseVerdict::Close : CloseVerdict::Keep;
	}

	// PVE 인스턴스 사망 가방의 DB 행 floor_id - 층 + 이 값. 부팅이 지우고 월드에 되살리지 않는다 (인스턴스는 재시작을 넘지 않는다)
	constexpr uint32 INSTANCE_BAG_FLOOR_BASE = 1000;

	inline bool IsInstanceBagFloor(uint32 floorId)
	{
		return floorId >= INSTANCE_BAG_FLOOR_BASE;
	}
}
