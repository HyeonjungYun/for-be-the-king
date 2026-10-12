#pragma once

/*-----------------
	HouseTable
-----------------*/

// Server/Data/houses.csv — 집 볼륨 사각형 (level-map.md Core Rule 4 · 레벨 빌더가 내보낸다).
// 2026-10-12 무리 동선 생성이 "집 근처 금지" 를 지키려고 읽는다 (pack-routes spec R2). 부팅 때 한 번, 이후 읽기 전용.
// 파일이 없으면 집 0 — 집이 없는 층(2~4층 TEMP)은 그대로 돈다.

struct HouseDef
{
	string	houseId;
	uint32	floorId = 0;
	float	x0 = 0.f;		// 남서 모서리 (cm, +X 북 · +Y 동)
	float	x1 = 0.f;		// 북동 모서리
	float	y0 = 0.f;
	float	y1 = 0.f;

	// 점에서 사각형까지 거리 (안이면 0)
	float	Distance(float x, float y) const;
};

class HouseTable
{
public:
	// 실패하면 이유를 error 에 남기고 false. 표는 빈 채로 둔다
	static bool		Load(const string& dataDir, uint32 floorCount, OUT string& error);
	static bool		Parse(const string& csv, uint32 floorCount, OUT string& error);

	static const vector<HouseDef>&	All() { return s_houses; }
	static vector<HouseDef>			OnFloor(uint32 floorId);

private:
	static vector<HouseDef>	s_houses;
};
