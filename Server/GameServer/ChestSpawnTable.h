#pragma once
#include "ItemRoller.h"

/*---------------------
	ChestSpawnTable
---------------------*/

// Server/Data/chest_spawns.csv — 상자 고정 위치 (quick-spec § 7). 층 아이템 풀 다음에 읽는다.
// 🔴 지금 좌표는 TEMP - 테스트 맵 원점 주변 격자다. 1층 레벨이 생기면 좌표만 바꾼다.

struct ChestSpawn
{
	uint32		chestId = 0;
	uint32		floorId = 0;
	ChestTier	tier = ChestTier::Open;
	float		x = 0.f;
	float		y = 0.f;
	float		z = 0.f;
	float		yaw = 0.f;
	uint32		respawnSeconds = 0;		// 첫 획득부터 Sealed 로 돌아가기까지 (chest_respawn_seconds)
};

class ChestSpawnTable
{
public:
	// 실패하면 이유를 error 에 남기고 false. 표는 빈 채로 둔다.
	static bool		Load(const string& dataDir, uint32 floorCount, OUT string& error);
	static bool		Parse(const string& csv, uint32 floorCount, OUT string& error);

	static const vector<ChestSpawn>& All() { return s_spawns; }

private:
	static vector<ChestSpawn>	s_spawns;
};
