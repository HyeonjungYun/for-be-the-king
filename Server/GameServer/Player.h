#pragma once
#include "Creature.h"

class GameSession;
class Room;

class Player : public Creature
{
public:
	Player();
	virtual ~Player();

public:
	float GetEffectiveMoveSpeed() const;
	float GetSpeedCeiling(uint64 nowUs) const;

public:
	weak_ptr<GameSession> session;
	
public:
	uint64 lastMoveUs = 0;
	double moveBudget = 0.0;
	float moveExceptionSpeed = 0.f;
	uint64 moveExceptionEndUs = 0;
};
