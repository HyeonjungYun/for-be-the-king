#pragma once

class Object : public enable_shared_from_this<Object>
{
public:
	Object();
	virtual ~Object();

	bool IsPlayer() { return _isPlayer; }
	bool IsBag() const { return objectInfo->object_type() == Protocol::OBJECT_TYPE_BAG; }

public:
	Protocol::ObjectInfo* objectInfo;
	Protocol::PosInfo* posInfo;

public:
	atomic<weak_ptr<Room>> room;

protected:
	bool _isPlayer = false;
};

