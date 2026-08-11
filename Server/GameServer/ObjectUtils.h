#pragma once

class ObjectUtils
{
public:
	static PlayerRef CreatPlayer(GameSessionRef session);

private:
	static atomic<int64> s_idGenerator;
};

