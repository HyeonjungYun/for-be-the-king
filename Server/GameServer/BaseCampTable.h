#pragma once
#include "WallMap.h"

/*----------------
	BaseCampTable
----------------*/

// 베이스캠프 맵 (2026-10-17 - design/quick-specs/basecamp-map-2026-10-17.md).
// 플레이어마다 따로 여는 개인 방(RoomKind::BaseCamp)의 배치 - Server/Data/basecamp.csv. 클라도 같은 파일로 그린다.
//
//   kind,id,x,y,yaw,half_x,half_y
//   area   camp       캠프 바닥 (중심 · 반폭) - 걸어 다닐 수 있는 네모. 섬 바다 쪽 빈 곳(남쪽)에 둔다
//   spawn  player     들어오면 서는 곳
//   dummy  <type id>  허수아비 (monster_types.csv 의 id) - 고정 · 공격 안 함 · 맞으면 곧 만피
//   npc    stash | enhance | gate   창고지기 · 대장장이 · 출입 안내 (클라가 대화 UI 를 연다). 몸이 길을 막는다
//   prop   <이름>     장식 - 몸이 길을 막는다 (모루 · 상자 · 문기둥)
//
// 부팅 때 한 번 읽고 이후 읽기 전용이다 - 여러 Room 이 락 없이 Walls() 를 읽는다.

struct BaseCampPoint
{
	string	id;
	float	x = 0.f;
	float	y = 0.f;
	float	yaw = 0.f;
	float	halfX = 0.f;
	float	halfY = 0.f;
};

class BaseCampTable
{
public:
	// 파일이 없으면 베이스캠프 맵이 꺼진다 (Loaded() == false - 예전 UI 베이스캠프만). 잘못이면 이유를 error 에 남기고 false
	static bool		Load(const string& dataDir, OUT string& error);
	static bool		Parse(const string& csv, OUT string& error);

	// 캠프 바닥이 섬(층 0 해안선) 밖이어야 한다 - 부팅 때 WallTable 다음에 부른다
	static bool		CheckClearOf(const WallMap& floor, OUT string& error);

	static bool					Loaded() { return s_loaded; }
	static const WallMap&		Walls() { return s_walls; }		// 바닥 네모 = 해안선 · npc · prop = 기둥 상자
	static const BaseCampPoint&	Area() { return s_area; }
	static const BaseCampPoint&	Spawn() { return s_spawn; }
	static const BaseCampPoint&	Dummy() { return s_dummy; }		// id = 몬스터 종류 번호
	static const vector<BaseCampPoint>&	Npcs() { return s_npcs; }
	static uint32				DummyTypeId() { return s_dummyTypeId; }

private:
	static bool						s_loaded;
	static WallMap					s_walls;
	static BaseCampPoint			s_area;
	static BaseCampPoint			s_spawn;
	static BaseCampPoint			s_dummy;
	static uint32					s_dummyTypeId;
	static vector<BaseCampPoint>	s_npcs;
};
