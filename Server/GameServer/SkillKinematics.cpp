#include "pch.h"
#include "SkillKinematics.h"
#include "FloorBounds.h"

namespace SkillKinematics
{
	PushResult Push(const WallMap& walls, float x, float y, float dirX, float dirY, float distCm)
	{
		PushResult r;
		r.endX = x;
		r.endY = y;

		const float len = ::sqrtf(dirX * dirX + dirY * dirY);
		if (len <= 0.0001f || distCm <= 0.f || FloorBounds::Contains(x, y) == false)
			return r;

		dirX /= len;
		dirY /= len;

		// 층 경계 - 캡슐이 경계 안에 머무는 거리까지만 (경계 사각형을 반지름만큼 줄인다)
		constexpr float INNER = FloorBounds::HALF_CM - CAPSULE_RADIUS_CM;
		float maxDist = distCm;

		if (dirX > 0.f)			maxDist = min(maxDist, (INNER - x) / dirX);
		else if (dirX < 0.f)	maxDist = min(maxDist, (-INNER - x) / dirX);
		if (dirY > 0.f)			maxDist = min(maxDist, (INNER - y) / dirY);
		else if (dirY < 0.f)	maxDist = min(maxDist, (-INNER - y) / dirY);

		if (maxDist <= 0.f)
			return r;

		const float wantX = x + dirX * maxDist;
		const float wantY = y + dirY * maxDist;

		const float t = walls.SweepFirstHit(x, y, wantX, wantY, CAPSULE_RADIUS_CM);
		float actual = maxDist * t;

		if (t < 1.f)
		{
			r.hitWall = true;
			actual = max(0.f, actual - WALL_BACKOFF_CM);
		}

		if (actual < 1.f)
			return r;

		r.endX = x + dirX * actual;
		r.endY = y + dirY * actual;
		r.distCm = actual;
		r.moved = true;
		return r;
	}

	PushResult PullToward(const WallMap& walls, float fromX, float fromY, float toX, float toY, float stopCm)
	{
		PushResult r;
		r.endX = fromX;
		r.endY = fromY;

		const float dx = toX - fromX;
		const float dy = toY - fromY;
		const float d = ::sqrtf(dx * dx + dy * dy);

		if (d <= stopCm + 1.f)
			return r;

		// 사용자 결정 - 벽 너머로는 끌리지 않는다. 중심선이 벽에 걸리면 아예 움직이지 않는다
		if (walls.SweepBlocked(fromX, fromY, toX, toY, 0.f))
		{
			r.hitWall = true;
			return r;
		}

		return Push(walls, fromX, fromY, dx, dy, d - stopCm);
	}

	uint32 TravelMs(float distCm, float speedCms)
	{
		if (distCm <= 0.f || speedCms <= 0.f)
			return 0;

		return static_cast<uint32>(::ceilf(distCm / speedCms * 1000.f));
	}

	float SegmentCircleHit(float ax, float ay, float bx, float by, float cx, float cy, float radius)
	{
		const float dx = bx - ax;
		const float dy = by - ay;
		const float fx = ax - cx;
		const float fy = ay - cy;

		const float c = fx * fx + fy * fy - radius * radius;
		if (c <= 0.f)
			return 0.f;						// 시작점이 이미 원 안

		const float a = dx * dx + dy * dy;
		if (a <= 0.000001f)
			return NO_HIT;					// 움직이지 않았다

		const float b = 2.f * (fx * dx + fy * dy);
		const float disc = b * b - 4.f * a * c;
		if (disc < 0.f)
			return NO_HIT;

		const float t = (-b - ::sqrtf(disc)) / (2.f * a);
		return (t >= 0.f && t <= 1.f) ? t : NO_HIT;
	}
}
