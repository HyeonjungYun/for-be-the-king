#pragma once
#include "WallMap.h"
#include "EntryPointTable.h"

/*-------------
	WallTable
-------------*/

// 층별 서버 벽 - Server/Data/walls.csv · level_versions.csv (docs/architecture/adr-0006-server-2d-walls.md).
// 부팅 때 EntryPointTable 다음에 한 번 읽고 이후 읽기 전용이다 - 여러 Room 이 락 없이 Get 한다.
//
//   walls.csv            floor_id,kind,cx,cy,half_length,half_thickness,yaw     레벨 빌더 내보내기만 쓴다
//   level_versions.csv   floor_id,data_version                                  레벨이 있는 층
//
// 🔴 레벨이 아직 없는 층(TEMP)은 벽 0개로 돈다 - 지금 동작 그대로다 (level-map.md § Edge Cases).
//    level_versions.csv 에 있는 층은 레벨이 있다는 뜻이라 벽이 0개면 부팅을 거부한다 - 파일을 지워 벽이 조용히 꺼지지 않게.

class WallTable
{
public:
	// 진입 지점 무벽 원 (level-map.md Core Rule 15 · Tuning Knobs entry_clear_radius 9 m)
	static constexpr float	ENTRY_CLEAR_RADIUS_CM = 900.f;

	// 두 파일 모두 없어도 된다 (전층 벽 0개). 하나라도 잘못이면 이유를 error 에 남기고 false - 부팅 거부.
	// entries 는 9 m 무벽 원 검사용 - EntryPointTable::All()
	static bool				Load(const string& dataDir, uint32 floorCount, const vector<EntryPoint>& entries, OUT string& error);

	// nullptr = 그 파일이 없다. 실패하면 모든 층이 빈 맵으로 남는다
	static bool				Parse(const string* wallsCsv, const string* versionsCsv, uint32 floorCount,
								const vector<EntryPoint>& entries, OUT string& error);

	// 벽 없는 층 · 범위 밖 층은 빈 맵 (nullptr 이 아니다)
	static const WallMap&	Get(uint32 floorId);

	// level_versions.csv 의 값. 없으면 빈 문자열 - 레벨이 아직 없는 층
	static const string&	DataVersion(uint32 floorId);

private:
	static vector<WallMap>	s_maps;
	static vector<string>	s_versions;
};

// FBTK_WALLS=0 이면 false - 이동 검증의 벽 단계를 건너뛴다 (켬/끔 소크 비교 · 비상용, ADR-0006 § ⑥)
extern atomic<bool> GWallCheck;
