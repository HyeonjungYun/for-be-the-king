#include "pch.h"
#include "EntryPointTable.h"
#include "ItemTable.h"

// design/ux/entry-point.md § Acceptance Criteria - 서버 쪽 [unit] 줄 (AC-EP-03 · 08)

namespace
{
	constexpr uint32 FLOORS = 2;

	// 0층 두 곳 · 1층 한 곳. 고정 데이터라 결과가 매번 같다
	const char* VALID =
		"entry_id,floor_id,x,y,z,yaw\n"
		"1,0,2000,-2000,100,0\n"
		"2,0,-2000,2000,100,90\n"
		"11,1,0,0,100,0\n";

	bool LoadValid()
	{
		string error;
		return EntryPointTable::Parse(VALID, FLOORS, OUT error);
	}
}

TEST(EntryPoint, 고른_지점이_그_층에_있으면_그대로_쓴다)
{
	ASSERT_TRUE(LoadValid());

	mt19937 rng(7);
	bool wasRandom = true;
	const EntryPoint* point = EntryPointTable::Resolve(0, 2, rng, OUT wasRandom);

	ASSERT_NE(point, nullptr);
	EXPECT_EQ(point->entryId, 2u);
	EXPECT_FALSE(wasRandom);
	EXPECT_FLOAT_EQ(point->x, -2000.f);
	EXPECT_FLOAT_EQ(point->yaw, 90.f);
}

TEST(EntryPoint, 0이면_그_층_지점_중_무작위다)
{
	ASSERT_TRUE(LoadValid());

	mt19937 rng(7);
	set<uint32> seen;
	for (int32 i = 0; i < 50; i++)
	{
		bool wasRandom = false;
		const EntryPoint* point = EntryPointTable::Resolve(0, 0, rng, OUT wasRandom);

		ASSERT_NE(point, nullptr);
		EXPECT_TRUE(wasRandom);
		EXPECT_EQ(point->floorId, 0u);		// 다른 층 지점은 나오지 않는다
		seen.insert(point->entryId);
	}

	EXPECT_EQ(seen.size(), 2u);				// 두 곳 모두 나온다 - 한 곳으로 쏠리지 않는다
}

TEST(EntryPoint, 없는_id나_다른_층_id는_무작위로_바꾼다)
{
	ASSERT_TRUE(LoadValid());

	mt19937 rng(7);
	bool wasRandom = false;

	const EntryPoint* unknown = EntryPointTable::Resolve(0, 999, rng, OUT wasRandom);
	ASSERT_NE(unknown, nullptr);
	EXPECT_TRUE(wasRandom);
	EXPECT_EQ(unknown->floorId, 0u);

	// 11 은 1층 지점이다 - 0층 입장에 쓰면 안 된다(다른 층 좌표로 들어가게 된다)
	const EntryPoint* otherFloor = EntryPointTable::Resolve(0, 11, rng, OUT wasRandom);
	ASSERT_NE(otherFloor, nullptr);
	EXPECT_TRUE(wasRandom);
	EXPECT_EQ(otherFloor->floorId, 0u);
}

TEST(EntryPoint, 지점이_없는_층이_있으면_부팅을_거부한다)
{
	string error;
	const bool ok = EntryPointTable::Parse(
		"entry_id,floor_id,x,y,z,yaw\n"
		"1,0,0,0,100,0\n", FLOORS, OUT error);

	EXPECT_FALSE(ok);
	EXPECT_NE(error.find("floor 1"), string::npos) << error;
	EXPECT_TRUE(EntryPointTable::All().empty());
}

TEST(EntryPoint, id_0과_중복_id는_부팅을_거부한다)
{
	string error;

	// 0 은 "무작위" 라서 지점 id 로 쓸 수 없다
	EXPECT_FALSE(EntryPointTable::Parse(
		"entry_id,floor_id,x,y,z,yaw\n"
		"0,0,0,0,100,0\n"
		"11,1,0,0,100,0\n", FLOORS, OUT error));
	EXPECT_NE(error.find("entry_id"), string::npos) << error;

	EXPECT_FALSE(EntryPointTable::Parse(
		"entry_id,floor_id,x,y,z,yaw\n"
		"1,0,0,0,100,0\n"
		"1,1,0,0,100,0\n", FLOORS, OUT error));
	EXPECT_NE(error.find("duplicate"), string::npos) << error;
}

TEST(EntryPoint, 실제_데이터_파일이_부팅_검사를_통과한다)
{
	// Server/Data/entry_points.csv - 서버가 실제로 읽는 파일. 4층 모두에 지점이 있어야 한다
	string error;
	EXPECT_TRUE(EntryPointTable::Load(ItemTable::DefaultDataDir(), 4, OUT error)) << error;
	EXPECT_GE(EntryPointTable::All().size(), 4u);
}
