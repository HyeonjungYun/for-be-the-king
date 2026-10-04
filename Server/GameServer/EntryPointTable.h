#pragma once
#include <random>

/*--------------------
	EntryPointTable
--------------------*/

// Server/Data/entry_points.csv — 층별 진입 지점 (design/ux/entry-point.md · 2026-10-05 결정).
// 플레이어가 S4 지도에서 고르고(C_ENTER_GAME.entry_point_id), 0 이거나 맞지 않으면 그 층 지점 중 무작위다.
// 클라 S1/Content/Data/S1EntryPoints.csv 와 id · 좌표가 같아야 한다 - 어긋나면 서버 좌표가 진실이다.
// 🔴 지금 좌표는 TEMP - 층마다 네 모서리 + 중앙. 맵이 생기면 맵에 둔 생성 지점 좌표로 바꾼다.

struct EntryPoint
{
	uint32	entryId = 0;
	uint32	floorId = 0;
	float	x = 0.f;
	float	y = 0.f;
	float	z = 0.f;
	float	yaw = 0.f;
};

class EntryPointTable
{
public:
	// 실패하면 이유를 error 에 남기고 false. 표는 빈 채로 둔다.
	static bool		Load(const string& dataDir, uint32 floorCount, OUT string& error);
	static bool		Parse(const string& csv, uint32 floorCount, OUT string& error);

	// 요청한 지점을 그 층에서 찾는다. 0 · 없는 id · 다른 층 id 면 그 층 지점 중 무작위이고 wasRandom 이 true.
	// 층에 지점이 하나도 없으면 nullptr - Parse 가 층마다 하나 이상을 보장하므로 정상 데이터에서는 없다.
	static const EntryPoint*	Resolve(uint32 floorId, uint32 requestedId, mt19937& rng, OUT bool& wasRandom);

	static const vector<EntryPoint>& All() { return s_points; }

private:
	static vector<EntryPoint>	s_points;
};
