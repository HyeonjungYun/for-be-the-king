#pragma once
#include <random>

class Utils
{
public:
	template<typename T>
	static T GetRandom(T min, T max)
	{
		// 스레드마다 엔진 하나 - 예전엔 호출마다 random_device 로 mt19937(약 5 KB)을 새로 시드했다 (치명타 굴림마다)
		static thread_local std::mt19937 generator{ std::random_device{}() };

		if constexpr (std::is_integral_v<T>)
		{
			std::uniform_int_distribution<T> distribution(min, max);
			return distribution(generator);
		}
		else
		{
			std::uniform_real_distribution<T> distribution(min, max);
			return distribution(generator);
		}
	}
	static uint64 NowMicroseconds()
	{
		return std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	// 잡 타이머 · 워커 타임슬라이스의 시계 (ms). 예전엔 GetTickCount64 였다 - 분해능이 시스템 타이머(기본 15.6 ms)라
	// 33 ms 예약이 31 ~ 47 ms 에 발화했다. steady_clock(QPC) 은 µs 분해능이고 벽시계 조정에 흔들리지 않는다 (2026-10-21)
	static uint64 NowMilliseconds()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	static string Sha256Hex(const string& input);
};

