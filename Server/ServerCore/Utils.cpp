#include "pch.h"
#include "Utils.h"
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

string Utils::Sha256Hex(const string& input)
{
	BCRYPT_ALG_HANDLE algorithm = nullptr;
	BCRYPT_HASH_HANDLE hash = nullptr;
	string result;

	do
	{
		if (::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
			break;

		if (::BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) != 0)
			break;

		if (::BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())), static_cast<ULONG>(input.size()), 0) != 0)
			break;

		BYTE digest[32] = {};
		if (::BCryptFinishHash(hash, digest, sizeof(digest), 0) != 0)
			break;

		static const char* kHex = "0123456789abcdef";
		result.reserve(64);

		for (BYTE b : digest)
		{
			result.push_back(kHex[b >> 4]);
			result.push_back(kHex[b & 0x0F]);
		}

	} while (false);

	if (hash != nullptr)
		::BCryptDestroyHash(hash);
	if (algorithm != nullptr)
		::BCryptCloseAlgorithmProvider(algorithm, 0);

	return result;
}
