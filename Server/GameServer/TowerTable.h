#pragma once

/*--------------
	TowerTable
--------------*/

// Server/Data/towers.csv — 층의 탈출 타워 (레벨 빌더 내보내기, tools/level/promote_export.py 로 옮긴다).
//   tower_id,floor_id,x,y     id 는 Ta · Tb · Tc, 좌표 cm
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
	// 파일이 없으면 true (타워 0개). 내용이 잘못이면 이유를 error 에 남기고 false - 표는 빈 채로 둔다
	static bool		Load(const string& dataDir, uint32 floorCount, OUT string& error);
	static bool		Parse(const string& csv, uint32 floorCount, OUT string& error);

	static const vector<TowerDef>& All() { return s_towers; }

private:
	static vector<TowerDef>	s_towers;
};
