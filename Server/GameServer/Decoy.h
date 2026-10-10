#pragma once
#include "Creature.h"

/*-----------
	Decoy
-----------*/

// 떡밥 (skill-roster 4 단계) - 시전자와 똑같이 생긴 인형. 클라에는 CREATE_TYPE_PLAYER 로 스폰되어 플레이어처럼 그려지고,
// 서버에서는 플레이어도 몬스터도 아니다 (IsDecoy). 체력 skill_rules decoy_hp(1) - 어떤 공격이든 맞으면 사라진다. duration 뒤 저절로 사라진다.
// 몬스터 AI 는 플레이어 목록(BrainPlayer)만 보므로 속지 않는다 - "적들을 속인다" 는 플레이어 상대 (사용자 결정 미끼 기본안).
constexpr uint64 DECOY_ID_BASE = 6'000'000'000;
extern atomic<uint64> GNextDecoyId;

class Decoy : public Creature
{
public:
	Decoy(uint64 objectId, uint64 ownerId, uint64 expiresUs, int32 decoyHp)
		: ownerId(ownerId), expiresUs(expiresUs)
	{
		_isDecoy = true;
		objectInfo->set_object_id(objectId);
		objectInfo->set_creature_type(Protocol::CREATE_TYPE_PLAYER);
		posInfo->set_object_id(objectId);
		maxHp = decoyHp;
		hp = decoyHp;
	}

	uint64	ownerId = 0;
	uint64	expiresUs = 0;

	// 그림자 분신 (그림자 보내기 1126, 2026-10-11) - 조준 방향으로 걷는다 (Room::StepClones). 걷기가 끝나거나 막히면 사라진다.
	// 주인은 분신의 시야를 함께 본다 (FoW 추가 시야원). 0 이면 서 있는 미끼(떡밥)
	float	walkDirX = 0.f;
	float	walkDirY = 0.f;
	float	walkSpeedCms = 0.f;
	float	walkRemainingCm = 0.f;
	bool	isClone = false;
};
