#include "pch.h"
#include "KillBatch.h"
#include "WallMap.h"

// 동시 치사 (combat-system.md TR-combat-022 · 사용자 결정 2026-10-08 "양쪽 모두 사망") · 평타 시선 (Rule 1 · 7, 같은 날 "평타는 벽 너머 불가능")
// Room 은 테스트에 넣지 않는다 - Room 이 쓰는 두 조각(KillBatch · WallMap 의 r = 0 스윕)을 그대로 시험한다.

namespace
{
	// 회귀 대상: Room::ResolveAttacks 의 공격자 조건. 예전엔 IsAlive() 만 봤다
	bool CanStillStrike(bool alive, const KillBatch<int32>& kills, uint64 id)
	{
		return alive || kills.DiedThisFlush(id);
	}
}

TEST(KillBatch, 같은_Flush_에서_서로를_죽이면_둘_다_죽고_서로가_가해자다)
{
	KillBatch<int32> kills;
	kills.Begin();

	// A(1) 의 평타가 먼저 해소돼 B(2) 가 0 이 됐다
	EXPECT_TRUE(kills.Note(2, 1, 0, 0));

	// 예전 코드: B 는 IsAlive() == false 라 B 의 평타가 "공격자 사망" 으로 취소됐다
	EXPECT_TRUE(CanStillStrike(/*alive*/ false, kills, 2)) << "막 죽은 B 의 이미 때가 된 히트는 들어간다";
	EXPECT_TRUE(kills.Note(1, 2, 0, 0));

	const auto ended = kills.End();
	ASSERT_EQ(ended.size(), 2u);
	EXPECT_EQ(ended[0].victimId, 2u);
	EXPECT_EQ(ended[0].instigatorId, 1u);
	EXPECT_EQ(ended[1].victimId, 1u);
	EXPECT_EQ(ended[1].instigatorId, 2u);
	EXPECT_TRUE(ended[0].mutual);
	EXPECT_TRUE(ended[1].mutual);
	EXPECT_FALSE(kills.IsOpen());
}

TEST(KillBatch, 한_대상은_한_번만_죽고_닫힌_뒤에는_막_죽은_표시가_남지_않는다)
{
	KillBatch<int32> kills;
	kills.Begin();

	EXPECT_TRUE(kills.Note(5, 1, 0, 0));
	EXPECT_FALSE(kills.Note(5, 3, 0, 0)) << "같은 Flush 에 두 번 죽지 않는다 - 첫 가해자가 처치자";

	const auto ended = kills.End();
	ASSERT_EQ(ended.size(), 1u);
	EXPECT_EQ(ended[0].instigatorId, 1u);
	EXPECT_FALSE(ended[0].mutual) << "1 은 죽지 않았다";

	// 다음 Flush 에서는 죽은 몸이 더는 때리지 못한다
	kills.Begin();
	EXPECT_FALSE(CanStillStrike(/*alive*/ false, kills, 5));
	EXPECT_TRUE(kills.End().empty());
}

TEST(KillBatch, 셋이_엇갈려_죽으면_서로_죽인_쌍만_mutual)
{
	KillBatch<int32> kills;
	kills.Begin();

	kills.Note(2, 1, 0, 0);		// 1 → 2
	kills.Note(3, 2, 0, 0);		// 2(막 죽음) → 3
	kills.Note(1, 3, 0, 0);		// 3(막 죽음) → 1 - 고리지만 쌍은 아니다

	for (const auto& kill : kills.End())
		EXPECT_FALSE(kill.mutual) << kill.victimId;
}

TEST(AttackLine, 엄폐물을_가로지르는_평타는_막히고_비켜_서거나_면에_붙으면_통과한다)
{
	// 엄폐물 하나 - 1층 개활지 cover 와 같은 크기 (길이 6 m · 두께 1 m, yaw 90 = Y 방향으로 길다)
	WallBox cover;
	cover.kind = WallKind::Cover;
	cover.cx = 0.f;
	cover.cy = 0.f;
	cover.halfLength = 300.f;
	cover.halfThickness = 50.f;
	cover.yawDeg = 90.f;

	WallMap map;
	string error;
	ASSERT_TRUE(map.Build({ cover }, OUT error)) << error;

	// Room::IsAttackLineClear 와 같은 질의 - 두 중심 사이 r = 0 스윕
	auto clear = [&map](float ax, float ay, float bx, float by) { return map.SweepBlocked(ax, ay, bx, by, 0.f) == false; };

	// 사거리 150 cm 안, 엄폐물 양쪽 (x = -75 / +75) - 막힌다
	EXPECT_FALSE(clear(-75.f, 0.f, 75.f, 0.f));

	// 엄폐물 끝(y = 300) 밖으로 비켜 서면 통과
	EXPECT_TRUE(clear(-75.f, 360.f, 75.f, 360.f));

	// 같은 쪽 - 통과
	EXPECT_TRUE(clear(-150.f, 0.f, -60.f, 0.f));

	// 대상이 엄폐물 면(x = 50)에 딱 붙어 있다 - 면에 닿는 것은 막힘이 아니다 (WallMap 열린 집합, AC-LM-04)
	EXPECT_TRUE(clear(150.f, 0.f, 50.f, 0.f));
}
