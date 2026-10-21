#pragma once

/*--------------
	TowerTable
--------------*/

// Server/Data/towers.csv — 층의 탈출 타워 (레벨 빌더 내보내기, Tools/level/promote_export.py 로 옮긴다).
//   tower_id,floor_id,x,y     id 는 Ta · Tb · Tc · Td, 좌표 cm (섬 1층은 4개 - 2026-10-11)
// 몬스터 자리 검증(monster-ai.md F8 — 타워에서 35 m) · 탈출 게이지(escape-timer.md)가 읽는다.
// 파일이 없으면 타워 0개로 돈다 - 레벨이 아직 없는 층과 같다.

struct TowerDef
{
	string	towerId;
	uint32	floorId = 0;
	float	x = 0.f;
	float	y = 0.f;
};

class TowerTable
{
public:
	// 타워가 있는 층의 타워 수 (level-map.md Core Rule 6 · ADR-0013). 섬 1층은 4개 - 3개로는 동쪽 끝이 F5 를 못 넘었다.
	// 상한은 Player::MAX_TOWERS (게이지 배열 크기)와 같다 - Room.cpp static_assert
	static constexpr size_t	MIN_TOWERS_PER_FLOOR = 3;
	static constexpr size_t	MAX_TOWERS_PER_FLOOR = 4;
	static constexpr float	COAST_MIN_CM = 1200.f;		// 해안선에서 12 m (Tuning Knobs tower_boundary)

	// 파일이 없으면 true (타워 0개). 내용이 잘못이면 이유를 error 에 남기고 false - 표는 빈 채로 둔다
	static bool		Load(const string& dataDir, uint32 floorCount, OUT string& error);
	static bool		Parse(const string& csv, uint32 floorCount, OUT string& error);

	static const vector<TowerDef>& All() { return s_towers; }

	// 탈출 규칙 검사 (escape-timer.md Core Rule 17) - 타워가 있는 층은 3~4개, 서로 2 × R_leave 와
	// 2 × R_alert 보다 멀어야 한다(구역 · 경보 집합이 겹치지 않음). 해안선이 있으면 그 안쪽 12 m.
	// WallTable 을 읽은 뒤에 부른다. 타워 없는 층(TEMP)은 통과
	static bool		ValidateForEscape(float leaveCm, float alertCm, uint32 floorCount, OUT string& error);

private:
	static vector<TowerDef>	s_towers;
};
