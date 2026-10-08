// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Game/Login/S1LoginLogic.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * design/quick-specs/account-login-mvp-2026-10-09.md AC-AL-09 (the message half) · ADR-0011 § Key Interfaces.
 *
 * Run headless:
 *   UnrealEditor-Cmd.exe S1.uproject -ExecCmds="Automation RunTests S1.Login; Quit" -nullrhi -unattended
 */
namespace
{
	constexpr EAutomationTestFlags GS1LoginTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1LoginErrorKeyTest, "S1.Login.Logic.ErrorKey_EveryContractErrorHasAMessage", GS1LoginTestFlags)
bool FS1LoginErrorKeyTest::RunTest(const FString& Parameters)
{
	// No answer at all - auth server down or the request timed out (AC-AL-09)
	TestEqual(TEXT("no response"), FString(S1LoginLogic::ErrorKey(0, FString())), FString(TEXT("login.error.unreachable")));
	TestEqual(TEXT("429 has no body"), FString(S1LoginLogic::ErrorKey(429, FString())), FString(TEXT("login.error.too_many")));

	TestEqual(TEXT("401 invalid_credentials"), FString(S1LoginLogic::ErrorKey(401, TEXT("invalid_credentials"))), FString(TEXT("login.error.invalid_credentials")));
	TestEqual(TEXT("409 username_taken"), FString(S1LoginLogic::ErrorKey(409, TEXT("username_taken"))), FString(TEXT("login.error.username_taken")));
	TestEqual(TEXT("400 invalid_username"), FString(S1LoginLogic::ErrorKey(400, TEXT("invalid_username"))), FString(TEXT("login.error.invalid_username")));
	TestEqual(TEXT("400 password_too_short"), FString(S1LoginLogic::ErrorKey(400, TEXT("password_too_short"))), FString(TEXT("login.error.password_too_short")));
	TestEqual(TEXT("401 invalid_token"), FString(S1LoginLogic::ErrorKey(401, TEXT("invalid_token"))), FString(TEXT("login.error.expired")));
	TestEqual(TEXT("400 invalid_name"), FString(S1LoginLogic::ErrorKey(400, TEXT("invalid_name"))), FString(TEXT("character.error.invalid_name")));
	TestEqual(TEXT("409 name_taken"), FString(S1LoginLogic::ErrorKey(409, TEXT("name_taken"))), FString(TEXT("character.error.name_taken")));

	// A code the client does not know still says something rather than nothing
	TestEqual(TEXT("500"), FString(S1LoginLogic::ErrorKey(500, FString())), FString(TEXT("login.error.unknown")));
	TestEqual(TEXT("new code"), FString(S1LoginLogic::ErrorKey(409, TEXT("something_new"))), FString(TEXT("login.error.unknown")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1LoginTokenRejectedTest, "S1.Login.Logic.TokenRejected_OnlyInvalidToken401", GS1LoginTestFlags)
bool FS1LoginTokenRejectedTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("401 invalid_token"), S1LoginLogic::IsTokenRejected(401, TEXT("invalid_token")));
	TestFalse(TEXT("401 invalid_credentials stays on L1 without dropping anything"), S1LoginLogic::IsTokenRejected(401, TEXT("invalid_credentials")));
	TestFalse(TEXT("409 character_limit"), S1LoginLogic::IsTokenRejected(409, TEXT("character_limit")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1LoginTokenShapeTest, "S1.Login.Logic.LooksLikeToken_64Hex", GS1LoginTestFlags)
bool FS1LoginTokenShapeTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("SHA256(\"dev_1\")"), S1LoginLogic::LooksLikeToken(TEXT("9f32e2436dd0db4a5737ca5611fa747d77a7273309877c94881b38d42a2c98fe")));
	TestTrue(TEXT("upper case hex"), S1LoginLogic::LooksLikeToken(TEXT("9F32E2436DD0DB4A5737CA5611FA747D77A7273309877C94881B38D42A2C98FE")));
	TestFalse(TEXT("63 chars"), S1LoginLogic::LooksLikeToken(TEXT("9f32e2436dd0db4a5737ca5611fa747d77a7273309877c94881b38d42a2c98f")));
	TestFalse(TEXT("non-hex"), S1LoginLogic::LooksLikeToken(TEXT("zf32e2436dd0db4a5737ca5611fa747d77a7273309877c94881b38d42a2c98fe")));
	TestFalse(TEXT("empty"), S1LoginLogic::LooksLikeToken(FString()));
	return true;
}

#endif
