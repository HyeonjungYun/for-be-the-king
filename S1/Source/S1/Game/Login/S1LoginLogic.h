// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

/**
 * The parts of the login flow that need no engine: which message a failed request shows, and
 * whether a token string can be the dev bypass. ADR-0011 · design/quick-specs/account-login-mvp-2026-10-09.md
 */
namespace S1LoginLogic
{
	/**
	 * The String Table key for a failed auth-server request. HttpCode 0 = no answer at all (server down,
	 * timeout). ErrorCode is the response's "error" field — the HTTP contract in ADR-0011 § Key Interfaces.
	 */
	const TCHAR* ErrorKey(int32 HttpCode, const FString& ErrorCode);

	/** 401 invalid_token on any page means the token is gone — the player starts again from L1. */
	bool IsTokenRejected(int32 HttpCode, const FString& ErrorCode);

	/** A token is 32 bytes as hex (AuthServer Auth.NewToken · seed SHA256). Anything else is not one. */
	bool LooksLikeToken(const FString& Value);
}
