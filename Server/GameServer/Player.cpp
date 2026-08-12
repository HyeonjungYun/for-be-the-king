#include "pch.h"
#include "Player.h"

namespace
{
	constexpr float BASE_MOVE_SPEED = 340.F;
	constexpr float MS_MIN_RATIO = 0.5;
	constexpr float MS_MAX_RATIO = 1.5;
}

Player::Player()
{
	_isPlayer = true;
}

Player::~Player()
{
}

float Player::GetEffectiveMoveSpeed() const
{
	const float flatBonus = 0.f;
	const float percentBonus = 0.f;

	const float speed = (BASE_MOVE_SPEED + flatBonus) * (1.f + percentBonus) * (1.f - activeSlow);

	return std::clamp(speed, BASE_MOVE_SPEED * MS_MIN_RATIO, BASE_MOVE_SPEED * MS_MAX_RATIO);
}

float Player::GetSpeedCeiling(uint64 nowUs) const
{
	if (moveExceptionEndUs != 0 && nowUs < moveExceptionEndUs)
		return moveExceptionSpeed;

	return GetEffectiveMoveSpeed();
}
