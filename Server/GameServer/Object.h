#pragma once

class Object : public enable_shared_from_this<Object>
{
public:
	Object();
	virtual ~Object();

	bool IsPlayer() { return _isPlayer; }
	bool IsMonster() const { return _isMonster; }
	bool IsDecoy() const { return _isDecoy; }		// 떡밥 인형 - 플레이어처럼 보이지만 플레이어도 몬스터도 아니다 (skill-roster 4 단계)
	bool IsBag() const { return objectInfo->object_type() == Protocol::OBJECT_TYPE_BAG; }

public:
	Protocol::ObjectInfo* objectInfo;
	Protocol::PosInfo* posInfo;

public:
	atomic<weak_ptr<Room>> room;

protected:
	bool _isPlayer = false;
	bool _isMonster = false;
	bool _isDecoy = false;
};

