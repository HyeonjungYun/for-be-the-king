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

	static string Sha256Hex(const string& input);
};

