// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "S1LoginSubsystem.generated.h"

class FJsonObject;
class US1LoginWidget;
class UWorld;

/** The page the login screen shows. design/quick-specs/account-login-mvp-2026-10-09.md Core Rules 2 · 4. */
enum class ES1LoginPage : uint8
{
	Credentials,	// L1 — 아이디 · 비밀번호 · [로그인] · [가입]
	CharacterName,	// L2 — 이름 · [만들기]. Only for an account with no character
	Connecting,		// Token and character in hand — waiting for the game server's S_LOGIN
};

DECLARE_MULTICAST_DELEGATE(FOnS1LoginChanged);

/**
 * The product login — ADR-0011.
 *
 *   boot ──(-token= · editor DevLoginToken)──▶ connect as before (dev-launch · PIE · bots never see this)
 *     └──▶ L1 ──POST /auth/login | register──▶ token ──GET /auth/characters──▶ one ──▶ connect ──S_LOGIN──▶ S3
 *                                                                └ none ──▶ L2 ──POST /auth/characters──┘
 *
 * The auth server speaks HTTP and is the only thing that sees a password. The game server only ever
 * gets the token, in C_LOGIN — US1GameInstance::ConnectToGameServerWithToken sends it.
 */
UCLASS(Config = Game)
class S1_API US1LoginSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:

	static US1LoginSubsystem* Get();

	virtual void Deinitialize() override;

	/**
	 * AS1NetworkPump::BeginPlay, before anything connects. Puts the configured game-server address on the
	 * game instance. Returns true when this run has a dev token and the pump should connect right away;
	 * otherwise the login screen comes up once every actor has begun play.
	 */
	bool BeginLogin(UWorld* World);

	/** -token= on the command line, or DevLoginToken in the ini — the latter only in editor binaries (PIE, -game). */
	static bool HasDevToken();

	// ── From the login screen ────────────────────────────────────────────

	void SubmitCredentials(const FString& Username, const FString& Password, bool bRegister);
	void SubmitCharacterName(const FString& Name);

	// ── Netcode hooks ────────────────────────────────────────────────────

	/** S_LOGIN failed (or came back with no character). Back to L1 with a reason — AC-AL-09. ClientPacketHandler. */
	void HandleGameLoginFailed();

	/** S_LOGIN succeeded — S3 takes the screen. US1BaseCampSubsystem::HandleLogin. */
	void HandleGameLoginSucceeded();

	/**
	 * S_KICK (anti-cheat 2026-10-19) — the server is about to close the connection. Reason = Protocol::KickReason,
	 * BannedUntil = unix seconds (0 = not suspended). On the login screen the reason shows there; in game the level
	 * reloads (a clean world) and the login screen comes back with the reason.
	 */
	void HandleKicked(int32 Reason, uint64 BannedUntil);

	// ── For the widget ───────────────────────────────────────────────────

	ES1LoginPage GetPage() const { return Page; }
	bool IsBusy() const { return bBusy; }
	const FText& GetMessage() const { return Message; }

	FOnS1LoginChanged OnChanged;

private:

	using FJsonDone = TFunction<void(int32 /*HttpCode, 0 = no answer*/, const TSharedPtr<FJsonObject>& /*body or null*/)>;

	/** One request to the auth server. Done runs on the game thread — the HTTP module's default policy. */
	void SendJson(const FString& Verb, const FString& Path, const TSharedPtr<FJsonObject>& Body, FJsonDone Done);

	void RequestLogin(const FString& Username, const FString& Password);
	void RequestCharacters();
	void ConnectNextTick();
	void Connect();

	/** The refused connection, closed a tick after S_LOGIN - never from inside the packet loop. */
	void DisconnectGame();

	/** Kicked in game: close the connection and reload the level - BeginLogin then shows PendingMessage. */
	void ReloadToLogin();

	void ShowScreen();
	void HideScreen();
	void SetPage(ES1LoginPage NewPage, const FText& NewMessage);
	void Fail(int32 HttpCode, const TSharedPtr<FJsonObject>& Json);
	void Fail(const FText& Reason);

	/** Where the auth server listens — AuthServer appsettings.json Kestrel. 🔴 http only: LAN play test (quick-spec OQ-1). */
	UPROPERTY(Config)
	FString AuthServerUrl = TEXT("http://127.0.0.1:8080");

	UPROPERTY(Config)
	FString GameServerHost = TEXT("127.0.0.1");

	UPROPERTY(Config)
	int32 GameServerPort = 7777;

	/** Per request. The HTTP module's own default total timeout is none at all. */
	UPROPERTY(Config)
	float RequestTimeoutSeconds = 10.f;

	UPROPERTY() TObjectPtr<US1LoginWidget>	Widget;

	TWeakObjectPtr<UWorld>	LoginWorld;
	FDelegateHandle			WorldBeginPlayHandle;
	ES1LoginPage			Page = ES1LoginPage::Credentials;
	FText					Message;
	FText					PendingMessage;		// survives the level reload after a kick - BeginLogin shows it
	FString					Token;
	bool					bBusy = false;
	bool					bActive = false;		// The product flow runs this session (no dev token)
};
