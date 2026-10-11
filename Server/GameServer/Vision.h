#pragma once
#include "WallMap.h"

/*-----------
	Vision
-----------*/

// FoW (전장의 안개, design/gdd/fog-of-war.md · 2026-10-11 사용자 지시). 서버가 권위를 갖는다 - 클라에는 보이는 생물만 보낸다.
// 여기는 규칙(Server/Data/vision_rules.csv)과 순수 판정 함수만 있다. "누가 누구를 아는가" 집합 · 패킷은 Room 이 든다
// (Player::fowKnown · Room::UpdateVisibility). GameServerTests VisionTest 가 검사한다.
//
// 결정 (에이전트, YOLO): 시선은 점 대 점(생물 중심끼리). 벽 · 문틀 · 기둥 · 타워가 막고 엄폐물(cover)은 낮아서 통과.
// 시야를 잃어도 grace 동안은 계속 보낸다 - 벽 모서리에서 깜빡이지 않게, 사망 · 마지막 이동이 도착하게.

struct VisionRules
{
	float	radiusCm = 0.f;			// vision_radius_cm - 기본 시야 반경
	uint32	graceMs = 0;			// vision_grace_ms - 시야를 잃고도 이만큼 더 보낸다
	float	minRadiusCm = 0.f;		// vision_min_radius_cm - 실명 등으로 줄어도 이보다 작아지지 않는다
};

// 추가 시야원 하나 (그림자 분신 · 종이매 · 맹수의 탐색). Player::visionExtras 에 들고, S_VISION 으로 주인에게 알린다
struct VisionExtra
{
	uint64	sourceId = 0;			// 클라 표시용 (분신 id · 종이매 id · 탐지 id). 같은 id 를 다시 넣으면 갱신
	uint64	followId = 0;			// 0 = 고정 지점, 아니면 그 생물의 현재 위치 (분신 · 종이매가 맴도는 적)
	float	x = 0.f;
	float	y = 0.f;
	float	radiusCm = 0.f;
	uint64	untilUs = 0;			// 0 = 따라가는 생물이 방에 있는 동안
	bool	needsLos = true;		// false = 벽을 넘어 본다 (공중의 종이매 · 땅 울림 탐지)
	bool	seesStealth = false;	// true = 은신도 보인다 (맹수의 탐색)
	string	kind;					// hawk · clone · detect - 클라 연출 (종이매는 매를 그린다, 2026-10-11)
};

namespace Vision
{
	// 시야원 하나를 판정용으로 푼 것 - 위치는 이미 정해졌다
	struct Source
	{
		float	x = 0.f;
		float	y = 0.f;
		float	radiusCm = 0.f;
		bool	needsLos = true;
		bool	seesStealth = false;
	};

	bool				Load(const string& dataDir, OUT string& error);
	bool				Parse(const string& csv, OUT string& error);
	const VisionRules&	Rules();

	// FBTK_FOW=0 이면 끈다 - 예전처럼 전원에게 전부 보낸다 (부하 A/B 비교 · 비상용)
	void				SetEnabled(bool on);
	bool				Enabled();

	// 시선 - 벽 · 문틀 · 기둥 · 타워가 막는다. 엄폐물은 통과 (WallMap::SightBlocked)
	bool				LineOfSight(const WallMap& walls, float ax, float ay, float bx, float by);

	// 시야원 하나가 점 (x, y) 를 보는가 - 반경 안(경계 포함)이고, 필요하면 시선이 트였다
	bool				SourceSees(const WallMap& walls, const Source& src, float x, float y);

	// 여러 시야원 중 하나라도 보는가. stealthed = 그 대상이 은신 중 - seesStealth 인 시야원만 센다
	bool				AnySees(const WallMap& walls, const vector<Source>& sources, float x, float y, bool stealthed);

	// 선분 (ax, ay) → (bx, by) 가 원 (cx, cy, r) 에 닿는가 - 투사체 경로가 시야 안을 지나는지 (시선은 보지 않는다)
	bool				SegmentNearCircle(float ax, float ay, float bx, float by, float cx, float cy, float r);
}
