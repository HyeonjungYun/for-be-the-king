#include "pch.h"
#include "Vision.h"
#include "WallMap.h"
#include "ItemTable.h"

// FoW 시야 판정 (design/gdd/fog-of-war.md § Formulas · AC-FOW-01 ~ 05). 집합 관리 · 패킷은 Room - 봇 · 클라로 본다

namespace
{
	WallBox Box(float cx, float cy, float halfLength, float halfThickness, float yawDeg, WallKind kind = WallKind::Wall)
	{
		WallBox w;
		w.kind = kind;
		w.cx = cx;
		w.cy = cy;
		w.halfLength = halfLength;
		w.halfThickness = halfThickness;
		w.yawDeg = yawDeg;
		return w;
	}

	WallMap Build(vector<WallBox> walls)
	{
		WallMap map;
		string error;
		EXPECT_TRUE(map.Build(move(walls), OUT error)) << error;
		return map;
	}

	Vision::Source Eye(float x, float y, float r = 1500.f)
	{
		Vision::Source s;
		s.x = x;
		s.y = y;
		s.radiusCm = r;
		return s;
	}
}

// AC-FOW-01 반경 - 안(경계 포함)이면 보이고 밖이면 안 보인다
TEST(Vision, 반경_안은_보이고_밖은_안_보인다)
{
	const WallMap empty = Build({});

	EXPECT_TRUE(Vision::SourceSees(empty, Eye(0.f, 0.f), 1499.f, 0.f));
	EXPECT_TRUE(Vision::SourceSees(empty, Eye(0.f, 0.f), 1500.f, 0.f)) << "경계는 보인다";
	EXPECT_FALSE(Vision::SourceSees(empty, Eye(0.f, 0.f), 1501.f, 0.f));
	EXPECT_FALSE(Vision::SourceSees(empty, Eye(0.f, 0.f), 1100.f, 1100.f)) << "대각선 1556 cm";
}

// AC-FOW-02 시선 - 벽 · 기둥 · 타워는 막고 엄폐물은 통과
TEST(Vision, 벽은_시선을_막고_엄폐물은_통과한다)
{
	// x = 500 에 세로 벽 (길이 4 m)
	const WallMap wall = Build({ Box(500.f, 0.f, 200.f, 25.f, 90.f) });
	EXPECT_FALSE(Vision::SourceSees(wall, Eye(0.f, 0.f), 1000.f, 0.f));
	EXPECT_TRUE(Vision::SourceSees(wall, Eye(0.f, 0.f), 1000.f, 600.f)) << "벽 끝을 돌아 보이는 각도";

	const WallMap pillar = Build({ Box(500.f, 0.f, 30.f, 30.f, 0.f, WallKind::Pillar) });
	EXPECT_FALSE(Vision::SourceSees(pillar, Eye(0.f, 0.f), 1000.f, 0.f));

	const WallMap tower = Build({ Box(500.f, 0.f, 100.f, 100.f, 0.f, WallKind::Tower) });
	EXPECT_FALSE(Vision::SourceSees(tower, Eye(0.f, 0.f), 1000.f, 0.f));

	const WallMap cover = Build({ Box(500.f, 0.f, 200.f, 50.f, 90.f, WallKind::Cover) });
	EXPECT_TRUE(Vision::SourceSees(cover, Eye(0.f, 0.f), 1000.f, 0.f)) << "엄폐물은 낮아서 시선을 막지 않는다";
}

// 면에 닿기만 하는 선은 막히지 않는다 (WallMap 열린 집합 - 평타 시선과 같은 규칙)
TEST(Vision, 벽면을_스치는_시선은_막히지_않는다)
{
	// y = 0 위로 두께 50 의 가로 벽 (y ∈ [0, 50])
	const WallMap wall = Build({ Box(500.f, 25.f, 200.f, 25.f, 0.f) });
	EXPECT_TRUE(Vision::SourceSees(wall, Eye(0.f, 0.f), 1000.f, 0.f)) << "y = 0 면을 따라가는 선";
	EXPECT_FALSE(Vision::SourceSees(wall, Eye(0.f, 10.f), 1000.f, 10.f));
}

// needsLos = false 인 시야원(종이매 · 탐지)은 벽을 넘어 본다
TEST(Vision, 시선이_필요없는_시야원은_벽을_넘어_본다)
{
	const WallMap wall = Build({ Box(500.f, 0.f, 200.f, 25.f, 90.f) });
	Vision::Source hawk = Eye(0.f, 0.f, 800.f);
	hawk.needsLos = false;

	EXPECT_TRUE(Vision::SourceSees(wall, hawk, 700.f, 0.f));
	EXPECT_FALSE(Vision::SourceSees(wall, hawk, 900.f, 0.f)) << "반경은 그대로 적용된다";
}

// AC-FOW-04 은신 - seesStealth 인 시야원만 은신한 대상을 본다
TEST(Vision, 은신은_탐지_시야원만_본다)
{
	const WallMap empty = Build({});
	vector<Vision::Source> sources = { Eye(0.f, 0.f) };

	EXPECT_TRUE(Vision::AnySees(empty, sources, 300.f, 0.f, false));
	EXPECT_FALSE(Vision::AnySees(empty, sources, 300.f, 0.f, true)) << "바로 옆이어도 은신은 안 보인다";

	Vision::Source detect = Eye(0.f, 0.f, 1000.f);
	detect.needsLos = false;
	detect.seesStealth = true;
	sources.push_back(detect);
	EXPECT_TRUE(Vision::AnySees(empty, sources, 300.f, 0.f, true));
	EXPECT_FALSE(Vision::AnySees(empty, sources, 1200.f, 0.f, true)) << "탐지 반경 밖";
}

// 여러 시야원 - 본체가 못 봐도 분신이 보면 보인다
TEST(Vision, 시야원이_여럿이면_하나만_봐도_보인다)
{
	const WallMap wall = Build({ Box(500.f, 0.f, 200.f, 25.f, 90.f) });
	vector<Vision::Source> sources = { Eye(0.f, 0.f) };
	EXPECT_FALSE(Vision::AnySees(wall, sources, 1000.f, 0.f, false));

	sources.push_back(Eye(1200.f, 0.f, 1000.f));		// 벽 너머의 분신
	EXPECT_TRUE(Vision::AnySees(wall, sources, 1000.f, 0.f, false));
}

// 투사체 경로가 시야원 안을 지나는가
TEST(Vision, 선분과_원의_거리)
{
	EXPECT_TRUE(Vision::SegmentNearCircle(-1000.f, 0.f, 1000.f, 0.f, 0.f, 500.f, 500.f)) << "가운데를 지난다";
	EXPECT_FALSE(Vision::SegmentNearCircle(-1000.f, 0.f, 1000.f, 0.f, 0.f, 501.f, 500.f));
	EXPECT_TRUE(Vision::SegmentNearCircle(0.f, 0.f, 0.f, 0.f, 300.f, 0.f, 300.f)) << "길이 0 - 점";
	EXPECT_FALSE(Vision::SegmentNearCircle(-1000.f, 0.f, -600.f, 0.f, 0.f, 0.f, 500.f)) << "끝점이 원 앞에서 멈춘다";
}

// 규칙 파일
TEST(Vision, 규칙_파일을_읽고_범위를_검사한다)
{
	string error;
	EXPECT_TRUE(Vision::Parse("key,value\nvision_radius_cm,1500\nvision_grace_ms,300\nvision_min_radius_cm,150\n", OUT error)) << error;
	EXPECT_FLOAT_EQ(Vision::Rules().radiusCm, 1500.f);
	EXPECT_EQ(Vision::Rules().graceMs, 300u);

	EXPECT_FALSE(Vision::Parse("key,value\nvision_radius_cm,1500\nvision_grace_ms,300\n", OUT error)) << "키가 빠지면 기동 거부";
	EXPECT_FALSE(Vision::Parse("key,value\nvision_radius_cm,0\nvision_grace_ms,300\nvision_min_radius_cm,150\n", OUT error));
	EXPECT_FALSE(Vision::Parse("key,value\nvision_radius_cm,1500\nvision_grace_ms,5000\nvision_min_radius_cm,150\n", OUT error));
	EXPECT_FALSE(Vision::Parse("key,value\nvision_radius_cm,1500\nvision_grace_ms,300\nvision_min_radius_cm,2000\n", OUT error));

	// 실데이터 - 부팅과 같은 폴더
	EXPECT_TRUE(Vision::Load(ItemTable::DefaultDataDir(), OUT error)) << error;
	EXPECT_GT(Vision::Rules().radiusCm, Vision::Rules().minRadiusCm);
}
