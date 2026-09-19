#include "pch.h"
#include "Creature.h"

// 링크 확인용. 이게 통과하면 Object.cpp · Creature.cpp · Struct.pb.cc · protobuf 가
// 전부 제대로 붙은 것이다. 실패하면 대부분 LNK2019 로 나오고 무엇이 빠졌는지 보인다
TEST(Creature, 생성하면_protobuf_객체가_붙는다)
{
	CreatureRef creature = make_shared<Creature>();

	ASSERT_NE(creature->objectInfo, nullptr);
	ASSERT_NE(creature->posInfo, nullptr);
}
