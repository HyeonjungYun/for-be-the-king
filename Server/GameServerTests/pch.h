//
// pch.h — GameServerTests
//
// 🔴 GameServer/pch.h 를 쓰지 않는다. 그쪽은 GameSession · ServerPacketHandler ·
//    DBJobQueue 를 끌어온다. 여기서는 테스트 대상(Object · Creature · SkillTable)이
//    실제로 쓰는 것만 둔다.
//
//      Protocol::* 열거형             Enum.pb.h
//      Protocol::ObjectInfo · PosInfo  Struct.pb.h
//      Utils::GetRandom                ServerCore/Utils.h (헤더 전용 템플릿)
//      weak_ptr<Room>                  전방 선언만 — USING_SHARED_PTR 가 해준다
//
//    PCH 모드(/Yu)에서는 ..\GameServer\Creature.cpp 의 #include "pch.h" 도
//    디스크의 GameServer/pch.h 가 아니라 이 프로젝트가 만든 .pch 로 대체된다.

#pragma once

#define WIN32_LEAN_AND_MEAN

#include "gtest/gtest.h"

#ifdef _DEBUG
#pragma comment(lib, "ServerCore\\Debug\\ServerCore.lib")
#pragma comment(lib, "Protobuf\\Debug\\libprotobufd.lib")
#else
#pragma comment(lib, "ServerCore\\Release\\ServerCore.lib")
#pragma comment(lib, "Protobuf\\Release\\libprotobuf.lib")
#endif

// ServerCore.lib 안의 DBConnection.obj 가 mysql_* 를 미해결로 남길 수 있다.
// ServerCoreSTests 에서 겪었다 (roadmap § 0-5).
#pragma comment(lib, "MySQL\\libmysql.lib")

#include "CorePch.h"
#include "Utils.h"

#include "Enum.pb.h"
#include "Struct.pb.h"

USING_SHARED_PTR(Object);
USING_SHARED_PTR(Creature);
USING_SHARED_PTR(Room);
