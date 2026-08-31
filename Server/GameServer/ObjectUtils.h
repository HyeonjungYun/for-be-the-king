#pragma once

class ObjectUtils
{
public:
	static PlayerRef CreatPlayer(GameSessionRef session, uint64 characterId);

private:
	static atomic<int64> s_idGenerator;
};

