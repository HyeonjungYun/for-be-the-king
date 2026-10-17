#pragma once

class Object : public enable_shared_from_this<Object>
{
public:
	Object();
	virtual ~Object();

	bool IsPlayer() const { return _isPlayer; }
	bool IsMonster() const { return _isMonster; }
	bool IsDecoy() const { return _isDecoy; }		// 떡밥 인형 - 플레이어처럼 보이지만 플레이어도 몬스터도 아니다 (skill-roster 4 단계)
	// Creature 를 상속하는 것은 이 셋뿐이다 - 맞으면 static_pointer_cast<Creature> 가 안전하다 (dynamic_pointer_cast 대신, 2026-10-17)
	bool IsCreature() const { return _isPlayer || _isMonster || _isDecoy; }
	bool IsBag() const { return objectInfo->object_type() == Protocol::OBJECT_TYPE_BAG; }

public:
	Protocol::ObjectInfo* objectInfo;
	Protocol::PosInfo* posInfo;

public:
	atomic<weak_ptr<Room>> room;

protected:
	// 생성자에서 한 번 정해진다 (Player · Monster · Decoy). AsCreature · AsPlayer 가 이것으로 static 캐스트한다
	bool _isPlayer = false;
	bool _isMonster = false;
	bool _isDecoy = false;
};

