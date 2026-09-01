//
// pch.h
//

#pragma once

#define WIN32_LEAN_AND_MEAN

#include "gtest/gtest.h"

#ifdef _DEBUG
#pragma comment(lib, "ServerCore\\Debug\\ServerCore.lib")
#else
#pragma comment(lib, "ServerCore\\Release\\ServerCore.lib")
#endif

#pragma comment(lib, "MySQL\\libmysql.lib")

#include "CorePch.h"
#include "Utils.h"
