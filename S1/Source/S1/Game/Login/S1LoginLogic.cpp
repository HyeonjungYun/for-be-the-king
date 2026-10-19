// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/Login/S1LoginLogic.h"

namespace S1LoginLogic
{
	const TCHAR* ErrorKey(int32 HttpCode, const FString& ErrorCode)
	{
		if (HttpCode == 0)
			return TEXT("login.error.unreachable");

		// The rate limiter answers 429 with no body (AuthServer Program.cs — per-IP windows)
		if (HttpCode == 429)
			return TEXT("login.error.too_many");

		if (ErrorCode == TEXT("invalid_credentials"))	return TEXT("login.error.invalid_credentials");
		if (ErrorCode == TEXT("username_taken"))		return TEXT("login.error.username_taken");
		if (ErrorCode == TEXT("invalid_username"))		return TEXT("login.error.invalid_username");
		if (ErrorCode == TEXT("password_too_short"))	return TEXT("login.error.password_too_short");
		if (ErrorCode == TEXT("invalid_token"))			return TEXT("login.error.expired");
		if (ErrorCode == TEXT("invalid_name"))			return TEXT("character.error.invalid_name");
		if (ErrorCode == TEXT("name_taken"))			return TEXT("character.error.name_taken");
		if (ErrorCode == TEXT("account_suspended"))		return TEXT("login.error.suspended_login");	// 403 - anti-cheat ban (2026-10-19)

		return TEXT("login.error.unknown");
	}

	bool IsTokenRejected(int32 HttpCode, const FString& ErrorCode)
	{
		return HttpCode == 401 && ErrorCode == TEXT("invalid_token");
	}

	bool LooksLikeToken(const FString& Value)
	{
		if (Value.Len() != 64)
			return false;

		for (const TCHAR Ch : Value)
		{
			if (FChar::IsHexDigit(Ch) == false)
				return false;
		}
		return true;
	}
}
