#pragma once
#include "WallMap.h"

/*-------------------
	SkillKinematics
-------------------*/

// 서버가 직접 옮기는 변위 - 넉백 · 끌기 · 치명상 도약 (skill-roster 1 단계). 순수 함수 - WallMap 과 층 경계만 본다.
// 사용자 결정 (2026-10-10): 끌기는 벽에 막히고 시전자 바로 앞까지 · 넉백이 벽에 닿으면 거기서 멈추고 벽 기절(규칙 wall_stun_ms)
// GameServerTests SkillKinematicsTest 가 검사한다

namespace SkillKinematics
{
	constexpr float	CAPSULE_RADIUS_CM	= 42.f;		// ADR-0006 § ⑦ - 넉백 · 대시 · 블링크 반지름
	constexpr float	WALL_BACKOFF_CM		= 1.f;		// 벽 면에 딱 붙지 않게 (WallMap 의 "벽 안" 은 열린 집합 - 면은 밖이지만 여유를 둔다)

	struct PushResult
	{
		float	endX = 0.f;
		float	endY = 0.f;
		float	distCm = 0.f;		// 실제로 옮긴 거리
		bool	hitWall = false;	// 벽에 닿아 짧아졌다
		bool	moved = false;		// 1 cm 라도 옮겼다
	};

	// (x, y) 에서 (dirX, dirY) 로 distCm. 방향은 정규화하지 않아도 된다. 층 경계(반지름만큼 안쪽)에서 잘리고, 벽에 닿으면 그 앞에서 멈춘다
	PushResult	Push(const WallMap& walls, float x, float y, float dirX, float dirY, float distCm);

	// from 을 to 쪽으로 stopCm 앞까지 끈다. 둘 사이 중심선이 벽에 막히면 moved = false · hitWall = true (끌리지 않는다).
	// 이미 stopCm 안이면 moved = false
	PushResult	PullToward(const WallMap& walls, float fromX, float fromY, float toX, float toY, float stopCm);

	// 날아가는 시간 (ms) - 넉백 CC 길이. 거리 0 이면 0
	uint32		TravelMs(float distCm, float speedCms);
}
