#pragma once
#include "..\GameServer\WallMap.h"

/*-----------
	BotPath
-----------*/

// -tower 봇의 길찾기 (escape-timer.md AC-ET-26 - 층 30명 동시 완충).
// 예전엔 타워까지 직선으로 걷다가 엄폐물에 막혀 30명 중 22명만 닿았다 (g3-2026-10-07.md · reason=wall 69,066건).
//
// 서버와 같은 walls.csv 를 같은 WallMap 으로 읽어 1 m 격자 A* 를 돌고, 벽에 닿지 않는 한 멀리 건너뛰어 꺾는 점만 남긴다.
// 서버 이동 검증은 점(반지름 0)으로 쓸어 보므로, 칸은 벽에서 CLEARANCE 만큼 떨어져야 지나갈 수 있게 해 여유를 둔다.
// 벽이 있는 층은 지금 0층뿐이다 (level_versions.csv) - 다른 층은 Load 가 false 라 직선 그대로.
namespace BotPath
{
	// walls.csv 에서 floorId 의 벽을 읽어 격자를 만든다. 파일이 없거나 그 층 벽이 0개면 false
	bool	Load(uint32 floorId);
	bool	IsLoaded();
	uint32	LoadedFloor();

	// (fromX, fromY) → (toX, toY) 의 꺾는 점 목록. 마지막 점이 도착점이다 - 도착점이 벽 안이면(타워 중심 등) 가장 가까운 빈 칸.
	// 시작점은 넣지 않는다. 길이 없으면 빈 목록
	vector<pair<float, float>>	Find(float fromX, float fromY, float toX, float toY);
}
